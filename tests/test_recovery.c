#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <sys/sysmacros.h>
#include <unistd.h>
static int fail_sync,syncs,bad_label,write_fail,checks,fail_check;
static int fake_stat(int f,struct stat *s){int r=fstat(f,s);if(!r)s->st_uid=s->st_gid=0;return r;}
static int state_fd(int f){char p[64],b[512];snprintf(p,sizeof p,"/proc/self/fd/%d",f);ssize_t n=readlink(p,b,sizeof b-1);assert(n>=0);b[n]=0;return !strcmp(strrchr(b,'/')+1,"state");}
static int bad_state_label;
static ssize_t label(int f,const char *k,void *b,size_t c){(void)k;struct stat s;assert(!fstat(f,&s));const char *v=bad_label||(bad_state_label&&state_fd(f))?"wrong":S_ISDIR(s.st_mode)?state_fd(f)?"u:object_r:extm_wear_dir:s0":"u:object_r:extm_root_data:s0":"u:object_r:extm_owner_data:s0";assert(c>strlen(v));memcpy(b,v,strlen(v)+1);return (ssize_t)strlen(v)+1;}
static int sync_fn(int f){if(++syncs==fail_sync){errno=EIO;return -1;}return fsync(f);}
static ssize_t write_fn(int f,const void *b,size_t n){if(write_fail){errno=ENOSPC;return -1;}return write(f,b,n);}
#define fstat fake_stat
#define fgetxattr label
#define fsync sync_fn
#define write write_fn
#include "../src/io.c"
#include "../src/recovery.c"
#undef fstat
#undef fgetxattr
#undef fsync
#undef write
static int verify(void *p){(void)p;return ++checks==fail_check?-EBUSY:0;}
static int cases;
static void state_case(int mode) {
 char path[]="/tmp/nzm-state-recovery-XXXXXX";assert(mkdtemp(path));
 int data=open(path,O_RDONLY|O_DIRECTORY);assert(data>=0);assert(!mkdirat(data,"extm",0700));
 int d=openat(data,"extm",O_RDONLY|O_DIRECTORY);assert(d>=0);
 bad_label=fail_sync=write_fail=fail_check=bad_state_label=0;
 if(mode==2)assert(!symlinkat("/tmp",d,"state"));
 else if(mode==3){int f=openat(d,"state",O_CREAT|O_WRONLY,0600);assert(f>=0);close(f);}
 else {assert(!mkdirat(d,"state",0700));if(mode==1)assert(!fchmodat(d,"state",0755,0));}
 if(mode==4)bad_state_label=1;
 struct nzm_recovery_ops o={verify,&data};int out=-1;
 int rc=nzm_recovery_unowned(d);assert(mode?rc<0:rc==0);
 rc=nzm_recovery_owner(data,&o,&out);assert(mode?rc<0:rc==0);if(out>=0)close(out);
 if(!mode)assert(nzm_recovery_unowned(d)==-EPERM);
 struct stat s;assert(!fstatat(d,"state",&s,AT_SYMLINK_NOFOLLOW));
 if(mode==1)assert((s.st_mode&0777)==0755); /* No chmod repairs. */
 assert(!unlinkat(d,"state",S_ISDIR(s.st_mode)?AT_REMOVEDIR:0));
 unlinkat(d,"owner",0);close(d);assert(!unlinkat(data,"extm",AT_REMOVEDIR));close(data);assert(!rmdir(path));cases++;
 bad_state_label=0;
}
static void run(int mode,int failure) {
 char path[]="/tmp/nzm-recovery-test-XXXXXX";assert(mkdtemp(path));int data=open(path,O_RDONLY|O_DIRECTORY);assert(data>=0);
 fail_sync=syncs=bad_label=write_fail=checks=fail_check=0;
 if(mode){assert(!mkdirat(data,"extm",0700));int d=openat(data,"extm",O_RDONLY|O_DIRECTORY);assert(d>=0);if(mode==7)assert(!fchmod(d,0755));
 const char *name=mode==2?"unknown":mode==3?"file":"owner";
 if(mode>=2&&mode<=6){int f=openat(d,name,O_CREAT|O_EXCL|O_WRONLY,0600);assert(f>=0);if(mode==4||mode==6)assert(write(f,OWNER,sizeof OWNER-1)==sizeof OWNER-1);close(f);}
 if(mode==5){assert(!unlinkat(d,"owner",0));assert(!symlinkat("/etc/passwd",d,"owner"));}
 close(d);}
 if(failure==1)fail_sync=1;
 if(failure==2)write_fail=1;
 if(failure==3)bad_label=1;
 if(failure==4)fail_check=3;

 struct nzm_recovery_ops o={verify,&data};int out=-1;int rc=nzm_recovery_owner(data,&o,&out);
 int good=(mode==1||mode==4||mode==6)&&!failure;
 assert(good?rc==0:rc<0);assert(good?out>=0:out==-1);if(out>=0)close(out);

 /* Failures never remove partial evidence, repair modes or overwrite owner. */
 if(failure==1||failure==2){int d=openat(data,"extm",O_RDONLY|O_DIRECTORY);struct stat s;assert(!fstatat(d,"owner",&s,AT_SYMLINK_NOFOLLOW));close(d);}
 int d=openat(data,"extm",O_RDONLY|O_DIRECTORY);if(d>=0){unlinkat(d,"owner",0);unlinkat(d,"file",0);unlinkat(d,"unknown",0);close(d);unlinkat(data,"extm",AT_REMOVEDIR);}close(data);assert(!rmdir(path));cases++;
}
int main(void){umask(077);for(int m=0;m<=7;m++)run(m,0);for(int f=1;f<=4;f++)run(1,f);for(int m=0;m<5;m++)state_case(m);
 assert(!nzm_recovery_crypto_allowed(""));
 assert(!nzm_recovery_crypto_allowed("none"));
 assert(nzm_recovery_crypto_allowed("file")<0);
 assert(nzm_recovery_crypto_allowed("block")<0);
 assert(nzm_recovery_crypto_allowed("unknown")<0);
 assert(nzm_recovery_crypto_allowed(NULL)<0);
 const char *valid="32 1 259:10 / /data rw,nosuid shared:1 - f2fs /dev/block/bootdevice/by-name/userdata rw,lazytime\n";
 assert(!nzm_mount_record(valid,"/data","f2fs",makedev(259,10),32));
 assert(nzm_mount_record(valid,"/data","f2fs",makedev(8,26),32)<0);
 assert(nzm_mount_record(valid,"/data","ext4",makedev(259,10),32)<0);
 assert(nzm_mount_record(valid,"/data","f2fs",makedev(259,10),33)<0);
 assert(nzm_mount_record("32 1 259:10 /sub /data rw - f2fs /dev/block/bootdevice/by-name/userdata rw\n","/data","f2fs",makedev(259,10),32)<0);
 assert(nzm_mount_record("32 1 259:10 / /data ro - f2fs /dev/block/bootdevice/by-name/userdata ro\n","/data","f2fs",makedev(259,10),32)<0);
 assert(nzm_mount_record("32 1 259:10 / /data rw - f2fs /dev/block/sda26 rw\n","/data","f2fs",makedev(259,10),32)<0);
 assert(nzm_mount_record("garbage\n","/data","f2fs",makedev(259,10),32)<0);
 printf("recovery: %d filesystem/fault cases and 8 mount parser cases passed\n",cases);return 0;}
