/* Production directory/private/create_text with real fd-relative filesystem
 * operations and real modes; only Android uid/gid and SELinux are projected. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
static char fixture[256];
static int bad_uid, bad_gid, bad_label, ancestor_path, leaf_read, forbidden_read;
static void fixture_path(int fd,char *p,size_t cap) {
 char q[64];snprintf(q,sizeof q,"/proc/self/fd/%d",fd);
 ssize_t n=readlink(q,p,cap-1);assert(n>=0);p[n]=0;
}
static int fixture_open(const char *p,int flags,...) {
 assert(!strcmp(p,"/"));
 assert((flags&O_PATH)&&(flags&O_NOFOLLOW));ancestor_path++;
 return open(fixture,flags);
}
static int fixture_openat(int d,const char *p,int flags,...) {
 mode_t mode=0;
 if(flags&O_CREAT){va_list a;va_start(a,flags);mode=(mode_t)va_arg(a,int);va_end(a);}
 char base[512],full[768];fixture_path(d,base,sizeof base);
 snprintf(full,sizeof full,"%s/%s",base,p);
 const char *rel=full+strlen(fixture);
 if(!strcmp(rel,"/mnt") && !(flags&O_PATH)){
  forbidden_read++;errno=EACCES;return -1;
 }
 if(flags&O_DIRECTORY){assert(flags&O_NOFOLLOW);if(flags&O_PATH)ancestor_path++;else leaf_read++;}
 return openat(d,p,flags,mode);
}
static int fixture_fstat(int fd,struct stat *s) {
 int rc=fstat(fd,s);if(rc)return rc;
 char p[512];fixture_path(fd,p,sizeof p);const char *rel=p+strlen(fixture);
 /* Host is nonroot: project only the root owner, preserve real modes. */
 s->st_uid=0;s->st_gid=0;
 if(!strcmp(rel,"/mnt")||!strcmp(rel,"/mnt/vendor/persist"))s->st_gid=1000;
 if(!strcmp(rel,"/mnt/vendor/persist")){if(bad_uid)s->st_uid=1000;if(bad_gid)s->st_gid=0;}
 return 0;
}
static ssize_t fixture_xattr(int fd,const char *name,void *b,size_t cap) {
 (void)name;assert(!(fcntl(fd,F_GETFL)&O_PATH));
 char p[512];fixture_path(fd,p,sizeof p);
 const char *label=strstr(p,"lge-zram-wear")?"u:object_r:extm_wear_dir:s0":"u:object_r:lge_zram_runtime_file:s0";
 if(bad_label)label="u:object_r:tmpfs:s0";
 size_t n=strlen(label)+1;assert(n<=cap);memcpy(b,label,n);return (ssize_t)n;
}
/* Bionic-compatible exhausted-token contract: final token sets save=NULL.
 * A continuation with NULL state is invalid. Production directory must not
 * depend on libc preserving a non-NULL pointer after the last component. */
static char *strict_strtok_r(char *s,const char *delim,char **save) {
 if(!s){assert(*save);s=*save;}
 s+=strspn(s,delim);
 if(!*s){*save=NULL;return NULL;}
 char *end=s+strcspn(s,delim);
 if(*end){*end='\0';*save=end+1;}else *save=NULL;
 return s;
}
#define strtok_r strict_strtok_r
#define open fixture_open
#define openat fixture_openat
#define fstat fixture_fstat
#define fgetxattr fixture_xattr
#include "../src/core.c"
#include "../src/io.c"
#undef open
#undef openat
#undef fstat
#undef fgetxattr
#undef strtok_r
static void dir(const char *rel,mode_t mode) {
 char p[512];snprintf(p,sizeof p,"%s%s",fixture,rel);assert(!mkdir(p,0700));assert(!chmod(p,mode));
}
static void change(const char *rel,mode_t mode) {
 char p[512];snprintf(p,sizeof p,"%s%s",fixture,rel);assert(!chmod(p,mode));
}
int main(void) {
 char token[]="vendor/etc",*state=NULL;
 assert(!strcmp(strict_strtok_r(token,"/",&state),"vendor"));
 assert(!strcmp(strict_strtok_r(NULL,"/",&state),"etc")&&!state);
 strcpy(fixture,"/tmp/nzm-directory-XXXXXX");assert(mkdtemp(fixture));
 dir("/mnt",0755);dir("/mnt/vendor",0755);dir("/mnt/vendor/persist",0771);dir("/mnt/vendor/persist/lge-zram-wear",0700);
 dir("/dev",0755);dir("/dev/lge-zram-manager",0700);dir("/vendor",0755);dir("/vendor/etc",0755);dir("/sys",0755);dir("/sys/block",0755);
 /* Verify simulated SELinux denial is live, not accidentally bypassed. */
 int root=open(fixture,O_PATH|O_DIRECTORY);assert(root>=0);
 assert(fixture_openat(root,"mnt",O_RDONLY|O_DIRECTORY|O_NOFOLLOW)==-1&&errno==EACCES);close(root);
 int d=nzm_private(WEAR,WCTX);assert(d>=0);assert((fcntl(d,F_GETFL)&O_PATH)==0);close(d);
 assert(ancestor_path>=4&&leaf_read==1&&forbidden_read==1);
 bad_uid=1;assert(nzm_private(WEAR,WCTX)==-EPERM);bad_uid=0;
 bad_gid=1;assert(nzm_private(WEAR,WCTX)==-EPERM);bad_gid=0;
 mode_t wrong[]={0773,02771};
 for(size_t i=0;i<sizeof wrong/sizeof wrong[0];i++){change("/mnt/vendor/persist",wrong[i]);assert(nzm_private(WEAR,WCTX)==-EPERM);}change("/mnt/vendor/persist",0771);
 change("/mnt/vendor/persist",0755);d=nzm_private(WEAR,WCTX);assert(d>=0);close(d);
 change("/mnt/vendor/persist",0770);d=nzm_private(WEAR,WCTX);assert(d>=0);close(d);change("/mnt/vendor/persist",0771);
 change("/mnt/vendor",0771);assert(nzm_private(WEAR,WCTX)==-EPERM);change("/mnt/vendor",0755);
 change("/mnt/vendor/persist/lge-zram-wear",0755);assert(nzm_private(WEAR,WCTX)==-EPERM);change("/mnt/vendor/persist/lge-zram-wear",0700);
 bad_label=1;assert(nzm_private(WEAR,WCTX)==-EPERM);bad_label=0;
 char a[512],b[512];snprintf(a,sizeof a,"%s/mnt/vendor",fixture);snprintf(b,sizeof b,"%s/mnt/realvendor",fixture);
 assert(!rename(a,b));assert(!symlink("realvendor",a));assert(nzm_directory(WEAR)<0);assert(!unlink(a));assert(!rename(b,a));
 snprintf(a,sizeof a,"%s/mnt/vendor/persist/lge-zram-wear",fixture);snprintf(b,sizeof b,"%s/mnt/vendor/persist/realwear",fixture);
 assert(!rename(a,b));assert(!symlink("realwear",a));assert(nzm_private(WEAR,WCTX)<0);assert(!unlink(a));assert(!rename(b,a));
 d=nzm_private(RUN,RCTX);assert(d>=0);
 assert(!nzm_create_text(d,"minfree",RCTX,"100 200\n"));
 char value[64];assert(!nzm_text(d,"minfree",RCTX,value,sizeof value));assert(!strcmp(value,"100 200\n"));assert(!unlinkat(d,"minfree",0));close(d);
 d=nzm_directory("/vendor/etc");assert(d>=0&&!(fcntl(d,F_GETFL)&O_PATH));close(d);
 d=nzm_directory("/vendor/etc///");assert(d>=0&&!(fcntl(d,F_GETFL)&O_PATH));close(d);
 change("/dev/lge-zram-manager",0755);assert(nzm_private(RUN,RCTX)==-EPERM);change("/dev/lge-zram-manager",0700);
 d=nzm_directory("/sys/block");assert(d>=0&&!(fcntl(d,F_GETFL)&O_PATH));close(d);
 assert(nzm_directory("/mnt/../vendor/etc")==-EINVAL);
 const char *dirs[]={"/mnt/vendor/persist/lge-zram-wear","/mnt/vendor/persist","/mnt/vendor","/mnt","/dev/lge-zram-manager","/dev","/vendor/etc","/vendor","/sys/block","/sys"};
 for(size_t i=0;i<sizeof dirs/sizeof dirs[0];i++){snprintf(a,sizeof a,"%s%s",fixture,dirs[i]);assert(!rmdir(a));}assert(!rmdir(fixture));
 puts("directory: O_PATH ancestors, tmpfs read denial, root-owned ancestors and scoped persist system-write trust, leaf metadata/xattr, symlinks, RUN/minfree/vendor/sys passed");return 0;
}
