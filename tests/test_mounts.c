/* Full production mounted()/mounts_verify syscall-chain fixture. No device
 * paths are accessed: block stat, directory dev and proc/sys opens are adapted. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
static char fixture[256];
static int mode,opens,stats;
static dev_t data_dev(void){return mode>=13?makedev(259,38):makedev(259,10);}
static dev_t persist_dev(void){return mode==14?makedev(259,47):makedev(8,5);}
static void fdpath(int fd,char *b,size_t cap) {
 char p[64];snprintf(p,sizeof p,"/proc/self/fd/%d",fd);
 ssize_t n=readlink(p,b,cap-1);assert(n>=0);b[n]=0;
}
static int mount_open(const char *p,int flags,...) {
 char dest[512],name[64];const char *base=NULL;
 opens++;
 if(!strcmp(p,"/proc/self/mountinfo"))base="mountinfo";
 else if(!strcmp(p,"/sys/class/block/sda26/dev"))base="sysdata";
 else if(!strcmp(p,"/sys/class/block/sda5/dev"))base="syspersist";
 else if(!strcmp(p,"/sys/class/block/sda26/partition"))base="partdata";
 else if(!strcmp(p,"/sys/class/block/sda5/partition"))base="partpersist";
 else if(!strcmp(p,"/data"))base="data";
 else if(!strcmp(p,"/mnt/vendor/persist"))base="persist";
 else {int fd,n=0;assert(sscanf(p,"/proc/self/fdinfo/%d%n",&fd,&n)==1&&!p[n]);
  char path[512];fdpath(fd,path,sizeof path);
  snprintf(name,sizeof name,"info%s",strrchr(path,'/')+1);base=name;}
 snprintf(dest,sizeof dest,"%s/%s",fixture,base);
 return open(dest,flags);
}
static int block_stat(const char *p,struct stat *s) {
 stats++;
 if(mode==11&&!strcmp(p,"/dev/block/sda5")){errno=EACCES;return -1;}
 if(mode==12&&!strcmp(p,"/dev/block/bootdevice/by-name/persist")){errno=EACCES;return -1;}
 memset(s,0,sizeof *s);s->st_mode=S_IFBLK|0600;
 int data=strstr(p,"sda26")||strstr(p,"userdata");
 s->st_rdev=data?data_dev():persist_dev();s->st_dev=makedev(0,20);s->st_ino=data?26:5;
 if(mode==7&&data)s->st_rdev=makedev(8,26);
 if(mode==15&&strstr(p,"userdata"))s->st_rdev=persist_dev();
 if(mode==16&&strstr(p,"persist"))s->st_ino=55;
 if(mode==21&&strstr(p,"userdata"))s->st_dev=makedev(0,21);
 return 0;
}
static int mount_fstat(int fd,struct stat *s) {
 int rc=fstat(fd,s);if(rc)return rc;
 if(S_ISDIR(s->st_mode)){char p[512];fdpath(fd,p,sizeof p);s->st_dev=!strcmp(strrchr(p,'/')+1,"data")?data_dev():persist_dev();if(mode==8)s->st_dev=makedev(1,1);}
 return 0;
}
#define open mount_open
#define stat(p,s) block_stat(p,s)
#define lstat block_stat
#define fstat mount_fstat
#include "../src/io.c"
#define NZM_LEGACY_TEST
#include "../src/recovery.c"
#undef open
#undef stat
#undef lstat
#undef fstat
static void put(const char *name,const char *text) {
 char p[512];snprintf(p,sizeof p,"%s/%s",fixture,name);int f=open(p,O_WRONLY|O_CREAT|O_TRUNC,0600);assert(f>=0);assert(write(f,text,strlen(text))==(ssize_t)strlen(text));assert(!close(f));
}
int main(void) {
 strcpy(fixture,"/tmp/nzm-mounts-XXXXXX");assert(mkdtemp(fixture));
 char p[512];snprintf(p,sizeof p,"%s/data",fixture);assert(!mkdir(p,0700));snprintf(p,sizeof p,"%s/persist",fixture);assert(!mkdir(p,0700));
 char table[1024];
 for(mode=0;mode<=21;mode++) {
  snprintf(table,sizeof table,"32 1 %u:%u / /data rw - f2fs /dev/block/bootdevice/by-name/userdata rw\n33 1 %u:%u / /mnt/vendor/persist rw - ext4 /dev/block/bootdevice/by-name/persist rw\n",major(data_dev()),minor(data_dev()),major(persist_dev()),minor(persist_dev()));
  opens=stats=0;char devtext[64];
  snprintf(devtext,sizeof devtext,"%u:%u\n",major(data_dev()),minor(data_dev()));
  put("sysdata",mode==6||mode==19?"259:11\n":devtext);
  snprintf(devtext,sizeof devtext,"%u:%u\n",major(persist_dev()),minor(persist_dev()));
  put("syspersist",mode==20?"8:6\n":devtext);
  put("partdata",mode==17?"25\n":"26\n");put("partpersist",mode==18?"6\n":"5\n");
  put("infodata",mode==1?"pos:\t0\n":mode==2?"mnt_id:\t32\nmnt_id:\t32\n":mode==3?"mnt_id:\t99\n":"mnt_id:\t32\n");
  put("infopersist",mode==9?"pos:\t0\n":"mnt_id:\t33\n");
  char b[2048];snprintf(b,sizeof b,"%s%s",table,mode==4?"34 32 259:10 / /data/vendor/extm rw - f2fs /dev/block/bootdevice/by-name/userdata rw\n":mode==5?"34 33 8:5 / /mnt/vendor/persist/lge-zram-wear/child rw - ext4 /dev/block/bootdevice/by-name/persist rw\n":mode==10?"32 1 259:10 / /data rw - f2fs /dev/block/bootdevice/by-name/userdata rw\n":"");put("mountinfo",b);
  int rc=nzm_mounts_verify();int success=mode==0||mode==13||mode==14;
  assert(success?rc==0:rc<0);
  if(mode==11||mode==12)assert(rc==-EACCES);
  assert(opens&&stats);if(success){assert(opens==9);assert(stats==4);}
  /* Production userdata verifier must neither inspect nor require persist. */
  opens=stats=0;rc=nzm_data_verify();
  int data_success=success||mode==5||mode==9||mode==11||mode==12||mode==16||mode==18||mode==20;
  assert(data_success?rc==0:rc<0);
  if(data_success){assert(opens==5);assert(stats==2);}
 }
 /* Real userdata-only mount tables; no persist entry or fixture is needed. */
 mode=0;put("sysdata","259:10\n");put("partdata","26\n");put("infodata","mnt_id:\t32\n");
 const char *data_tables[]={
  "32 1 259:10 / /data rw - f2fs /dev/block/bootdevice/by-name/userdata rw\n",
  "32 1 259:10 /sub /data rw - f2fs /dev/block/bootdevice/by-name/userdata rw\n",
  "32 1 259:10 / /data rw - f2fs /dev/block/sda26 rw\n",
  "32 1 259:10 / /data ro - f2fs /dev/block/bootdevice/by-name/userdata ro\n",
  "32 1 259:10 / /data rw - ext4 /dev/block/bootdevice/by-name/userdata rw\n",
  "32 1 259:10 / /data rw - f2fs /dev/block/bootdevice/by-name/userdata rw\n34 32 259:10 / /data/vendor/extm/state rw - f2fs /dev/block/bootdevice/by-name/userdata rw\n",
  "32 1 259:10 / /data rw - f2fs /dev/block/bootdevice/by-name/userdata rw\n34 32 259:10 / /data/vendor rw - f2fs /dev/block/bootdevice/by-name/userdata rw\n",
  "32 1 259:10 / /data rw - f2fs /dev/block/bootdevice/by-name/userdata rw\n34 32 259:10 / /data/vendor/extm rw - f2fs /dev/block/bootdevice/by-name/userdata rw\n",
  "32 1 259:10 / /data rw - f2fs /dev/block/bootdevice/by-name/userdata rw\n34 32 259:10 / /data/vendor/extm2 rw - f2fs /dev/block/bootdevice/by-name/userdata rw\n"};
 for(size_t i=0;i<sizeof data_tables/sizeof data_tables[0];i++){
  put("mountinfo",data_tables[i]);int rc=nzm_data_verify();assert((!i||i==8)?rc==0:rc==-EXDEV);
 }
 const char *files[]={"sysdata","syspersist","partdata","partpersist","infodata","infopersist","mountinfo"};for(size_t i=0;i<sizeof files/sizeof files[0];i++){snprintf(p,sizeof p,"%s/%s",fixture,files[i]);assert(!unlink(p));}
 snprintf(p,sizeof p,"%s/data",fixture);assert(!rmdir(p));snprintf(p,sizeof p,"%s/persist",fixture);assert(!rmdir(p));assert(!rmdir(fixture));
 puts("mounts: 22 legacy and 31 userdata-only syscall-chain fixtures passed (no persist dependency, dynamic dev_t, alias, partition, sysfs, root/source and overlays)");return 0;
}
