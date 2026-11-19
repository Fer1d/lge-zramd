#define _GNU_SOURCE
#include "recovery.h"
#include "nzm_io.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#ifdef NZM_RECOVERY_ADAPTER
#include NZM_RECOVERY_ADAPTER
#endif
#define OWNER "extm-dm-owner-v1\n"
static const struct nzm_meta owner_meta={0,0,0600,1,"u:object_r:extm_owner_data:s0"};
static int bound(int p,int d) {
    struct stat a,b;
    if(fstat(d,&a)||fstatat(p,"extm",&b,AT_SYMLINK_NOFOLLOW))return -errno;
    return S_ISDIR(b.st_mode)&&a.st_dev==b.st_dev&&a.st_ino==b.st_ino?0:-ESTALE;
}
/* Only empty, owner-only, or complete existing owner+file containers are legal.
 * A file without owner and every unknown object are preserved and rejected. */
static int contents(int d,int *has_owner,int *has_file) {
    int fd=openat(d,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(fd<0)return -errno;
    DIR *dir=fdopendir(fd);if(!dir){int rc=-errno;close(fd);return rc;}
    int rc=0;struct dirent *e;*has_owner=*has_file=0;errno=0;
    while((e=readdir(dir))) {
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        if(!strcmp(e->d_name,"owner"))*has_owner=1;
        else if(!strcmp(e->d_name,"file"))*has_file=1;
        else {rc=-ENOTEMPTY;break;}
    }
    if(!rc&&errno)rc=-errno;
    if(closedir(dir)&&!rc)rc=-errno;
    return rc;
}
static int owner_read(int d) {
    char b[64];size_t n;
    int rc=nzm_io_read_at(d,"owner",&owner_meta,b,sizeof b,&n);
    return rc?rc:(n==sizeof OWNER-1&&!memcmp(b,OWNER,n)?0:-EPERM);
}
int nzm_recovery_owner(int data,const struct nzm_recovery_ops *o,int *out) {
    if(!o||!o->verify||!o->request||!out)return -EINVAL;
    *out=-1;int rc=o->verify(o->arg),d=-1,f=-1,has_owner,has_file;
    if(rc)return rc;
    d=nzm_io_private_dir_at(data,"extm","u:object_r:extm_root_data:s0");
    if(d==-ENOENT) {
        struct stat s;
        if(!fstatat(data,"extm",&s,AT_SYMLINK_NOFOLLOW))return -EEXIST;
        if(errno!=ENOENT)return -errno;
        if((rc=o->verify(o->arg))||(rc=o->request(o->arg)))return rc;
        d=nzm_io_private_dir_at(data,"extm","u:object_r:extm_root_data:s0");
    }
    if(d<0)return d;
    struct stat a,b;
    if(fstat(data,&a)||fstat(d,&b)){rc=-errno;goto done;}
    if(a.st_dev!=b.st_dev){rc=-EXDEV;goto done;}
    if((rc=bound(data,d))||(rc=contents(d,&has_owner,&has_file))||
       (rc=o->verify(o->arg)))goto done;
    if(!has_owner) {
        if(has_file){rc=-EPERM;goto done;}
        f=openat(d,"owner",O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        if(f<0){rc=-errno;goto done;}
        if((rc=nzm_io_check(f,S_IFREG,&owner_meta)))goto done;
        size_t n=0;
        while(n<sizeof OWNER-1) {
            ssize_t w=write(f,&OWNER[n],sizeof OWNER-1-n);
            if(w<0){if(errno==EINTR)continue;rc=-errno;goto done;}
            if(!w){rc=-EIO;goto done;}n+=(size_t)w;
        }
        if(fsync(f)){rc=-errno;goto done;}
        if(close(f)){f=-1;rc=-errno;goto done;}f=-1;
    }
    if((rc=owner_read(d)))goto done;
    /* Revalidate complete owner on a fresh invocation, not an assertion that
     * any previous operation committed. No fsync on O_PATH /data parent. */
    f=openat(d,"owner",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(f<0){rc=-errno;goto done;}
    if(fsync(f)||fsync(d)){rc=-errno;goto done;}
    if((rc=bound(data,d))||(rc=contents(d,&has_owner,&has_file))||
       (rc=owner_read(d))||(rc=o->verify(o->arg)))goto done;
    *out=d;d=-1;
done:
    if(f>=0&&close(f)&&!rc)rc=-errno;
    if(d>=0)close(d);
    if(rc&&*out>=0){close(*out);*out=-1;}
    return rc;
}
int nzm_recovery_crypto_allowed(const char *type) {
    return type && (!*type || !strcmp(type,"none")) ? 0 : -EPERM;
}
static int option(const char *s,const char *v) {
    size_t n=strlen(v);while(*s){const char *e=strchr(s,',');size_t k=e?(size_t)(e-s):strlen(s);if(k==n&&!memcmp(s,v,n))return 1;if(!e)break;s=e+1;}return 0;
}
int nzm_mount_record(const char *text,const char *target,const char *fs,dev_t dev,unsigned id) {
    char *copy=strdup(text);if(!copy)return -ENOMEM;
    int rc=-ENODEV,count=0;char *save=NULL;
    for(char *line=strtok_r(copy,"\n",&save);line;line=strtok_r(NULL,"\n",&save)) {
        unsigned mid,parent,ma,mi;char root[256],path[512],opts[256];int n=0;
        if(sscanf(line,"%u %u %u:%u %255s %511s %255s%n",&mid,&parent,&ma,&mi,root,path,opts,&n)!=7){rc=-EINVAL;break;}
        /* Reject bind/submount overlays even when they reuse the approved
         * superblock device number. Reserved containers must not be mounts. */
        const char *reserved=!strcmp(target,"/data")?"/data/extm":"/mnt/vendor/persist/lge-zram-wear";
        size_t reserved_len=strlen(reserved);
        if(!strncmp(path,reserved,reserved_len) &&
           (!path[reserved_len] || path[reserved_len]=='/')){rc=-EXDEV;break;}
        if(strcmp(path,target))continue;
        count++;char *sep=strstr(line+n," - ");char type[64],source[512],super[256],extra;
        if(!sep||sscanf(sep+3,"%63s %511s %255s %c",type,source,super,&extra)!=3){rc=-EINVAL;break;}
        const char *expected=!strcmp(target,"/data")?"/dev/block/bootdevice/by-name/userdata":"/dev/block/bootdevice/by-name/persist";
        rc=mid==id&&ma==major(dev)&&mi==minor(dev)&&!strcmp(root,"/")&&
            !strcmp(type,fs)&&!strcmp(source,expected)&&option(opts,"rw")&&
            option(super,"rw")?0:-EXDEV;
        if(rc)break;
    }
    free(copy);return count==1?rc:(rc==-EINVAL?rc:-EXDEV);
}
static int mount_error(const char *op,const char *path,int rc) {
    fprintf(stderr,"mount_verify syscall=%s path=%s errno=%d (%s)\n",op,path,-rc,strerror(-rc));
    return rc;
}
static int raw(const char *path,char *b,size_t cap) {
    int f=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(f<0)return mount_error("open",path,-errno);
    size_t n;int rc=nzm_io_read_fd(f,b,cap-1,&n);
    if(rc)mount_error("read",path,rc);
    if(!rc){if(memchr(b,0,n))rc=mount_error("validate-no-NUL",path,-EINVAL);else b[n]=0;}
    if(close(f)&&!rc)rc=mount_error("close",path,-errno);
    return rc;
}
static int mounted(const char *table,const char *path,const char *node,const char *alias,const char *sys,const char *partition,const char *partno,const char *fs) {
    struct stat st,a;char b[4096];int rc;
    if(lstat(node,&st))return mount_error("lstat",node,-errno);
    if(stat(alias,&a))return mount_error("stat",alias,-errno);
    if(!S_ISBLK(st.st_mode))return mount_error("validate-block-node",node,-EXDEV);
    /* dev_t allocation may differ across boots/recovery. Trust only the fixed
     * partition node corroborated by alias inode identity and sysfs below. */
    dev_t dev=st.st_rdev;
    if(!S_ISBLK(a.st_mode)||a.st_rdev!=dev||a.st_dev!=st.st_dev||a.st_ino!=st.st_ino)
        return mount_error("validate-alias-identity",alias,-EXDEV);
    if((rc=raw(partition,b,sizeof b)))return rc;
    if(strcmp(b,partno))return mount_error("validate-partition-number",partition,-EXDEV);
    if((rc=raw(sys,b,sizeof b)))return rc;
    char expected[32];snprintf(expected,sizeof expected,"%u:%u\n",major(dev),minor(dev));
    if(strcmp(b,expected))return mount_error("validate-sysfs-dev",sys,-EXDEV);
    int fd=open(path,O_PATH|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return mount_error("open-O_PATH",path,-errno);
    if(fstat(fd,&st)){rc=mount_error("fstat",path,-errno);close(fd);return rc;}
    if(st.st_dev!=dev){close(fd);return mount_error("validate-st_dev",path,-EXDEV);}
    char p[64];snprintf(p,sizeof p,"/proc/self/fdinfo/%d",fd);
    rc=raw(p,b,sizeof b);unsigned id=0;int found=0;
    if(!rc){char *s=b;while(s&&*s){unsigned v;char tail;if(sscanf(s,"mnt_id:\t%u%c",&v,&tail)==2&&tail=='\n'){id=v;found++;}s=strchr(s,'\n');if(s)s++;}if(found!=1)rc=mount_error("parse-mnt_id",p,-EINVAL);}
    if(!rc){rc=nzm_mount_record(table,path,fs,dev,id);if(rc)mount_error("validate-mountinfo",path,rc);}
    if(close(fd)&&!rc)rc=mount_error("close-O_PATH",path,-errno);
    return rc;
}
static int verify_mounts(int data) {
    char *b=malloc(262144);if(!b)return -ENOMEM;
    int rc=raw("/proc/self/mountinfo",b,262144);
    if(!rc && data)rc=mounted(b,"/data","/dev/block/sda26","/dev/block/bootdevice/by-name/userdata","/sys/class/block/sda26/dev","/sys/class/block/sda26/partition","26\n","f2fs");
    if(!rc)rc=mounted(b,"/mnt/vendor/persist","/dev/block/sda5","/dev/block/bootdevice/by-name/persist","/sys/class/block/sda5/dev","/sys/class/block/sda5/partition","5\n","ext4");
    free(b);return rc;
}
int nzm_mounts_verify(void) { return verify_mounts(1); }
int nzm_persist_verify(void) { return verify_mounts(0); }
