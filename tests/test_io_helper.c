/* Original standalone fixture regressions; build via ../build-and-test.sh
 * with spawn_fd.c and -pthread, separately plain/ASan/UBSan/TSan.
 * All path redirects and credential/MAC substitutions are syscall boundaries.
 * No production helper, sysfs or device path is opened/executed. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/xattr.h>
#include <sys/wait.h>
static int fixture=-1, bad_context, child_mode, short_write, write_calls;
static int wrong_helper_gid;
static dev_t vendor_dev,bin_dev;static ino_t vendor_ino,bin_ino;
static int sync_count, fail_sync, data_bad_uid, data_bad_gid;
static int sync_errno=EIO,space_error;
static fsblkcnt_t space_blocks=UINT64_C(1)<<30;
static unsigned long space_unit=4096;
static int test_fstatvfs(int fd,struct statvfs *s) {
    (void)fd;if(space_error){errno=space_error;return -1;}
    memset(s,0,sizeof *s);s->f_bavail=space_blocks;s->f_frsize=space_unit;return 0;
}
static int test_fsync(int fd) {
    if(++sync_count==fail_sync){errno=sync_errno;return -1;}return fsync(fd);
}
static int test_fstat(int fd,struct stat *s) {
    int r=fstat(fd,s);if(!r){s->st_uid=0;s->st_gid=0;
        if(S_ISDIR(s->st_mode)&&s->st_dev==vendor_dev&&s->st_ino==vendor_ino)s->st_gid=wrong_helper_gid==1?0:2000;
        if(S_ISDIR(s->st_mode)&&s->st_dev==bin_dev&&s->st_ino==bin_ino)s->st_gid=wrong_helper_gid==2?0:2000;
        if(S_ISDIR(s->st_mode)&&(s->st_mode&07777)==0771){s->st_uid=data_bad_uid?1001:1000;s->st_gid=data_bad_gid?1001:1000;}}
    return r;
}
static ssize_t test_xattr(int fd,const char *key,void *b,size_t cap) {
    struct stat st;assert(!fstat(fd,&st));
    assert(!(fcntl(fd,F_GETFL)&O_PATH)); /* Never MAC-read an O_PATH parent. */
    const char *s="u:object_r:extm_file_helper_exec:s0";
    /* Independent fixture labels: derive actual object class/name, never the
     * requested expected context. This catches swapped/equal API arguments. */
    char link[64],path[4096];snprintf(link,sizeof link,"/proc/self/fd/%d",fd);
    ssize_t z=readlink(link,path,sizeof path-1);assert(z>=0);path[z]=0;
    const char *base=strrchr(path,'/');base=base?base+1:path;
    if(S_ISDIR(st.st_mode)&&(st.st_mode&07777)==0700)s="u:object_r:extm_wear_dir:s0";
    if(!strcmp(base,"ledger")||!strcmp(base,"ledger.new")||!strcmp(base,"ledger.txn"))s="u:object_r:extm_wear_file:s0";
    if(bad_context)s="u:object_r:wrong:s0";
    assert(!strcmp(key,"security.selinux"));
    if(cap<strlen(s)+1){errno=ERANGE;return -1;}memcpy(b,s,strlen(s)+1);return (ssize_t)strlen(s)+1;
}
static ssize_t test_write(int fd,const void *b,size_t n) {
    write_calls++;if(short_write){return n>1?(ssize_t)n-1:0;}return write(fd,b,n);
}
#define fstat test_fstat
#define fgetxattr test_xattr
#define write test_write
#define fsync test_fsync
#define fstatvfs test_fstatvfs
#include "../src/io.c"
#undef fstatvfs
#undef fsync
#undef write
static int test_open(const char *path,int flags,...) {
    if(!strcmp(path,"/"))return dup(fixture);
    /* Child stdin is deliberately a pipe EOF, not a real device open. */
    if(!strcmp(path,"/dev/null")){int p[2];if(pipe(p))return -1;close(p[1]);return p[0];}
    errno=EPERM;(void)flags;return -1;
}
static int test_exec(const char *path,char *const argv[],char *const env[]) {
    assert(!strncmp(path,"/proc/self/fd/",14));
    assert(!strcmp(argv[0],"/vendor/bin/extm-file-helper"));
    assert(!strcmp(argv[1],"bind"));assert(!strcmp(argv[2],"536870912"));assert(!argv[3]);
    if(child_mode==6)return execve(path,argv,env);
    if(child_mode==1){const char err[]="denied\n\033\"\\";(void)write(2,err,sizeof err-1);_exit(42);}
    if(child_mode==2){sleep(2);_exit(0);}
    if(child_mode==3){char b[8192];memset(b,'x',sizeof b);(void)write(2,b,sizeof b);_exit(0);}
    if(child_mode==4){(void)write(1,"partial bind\n",13);_exit(0);}
    if(child_mode==5){raise(SIGTERM);_exit(1);}
    errno=ENOEXEC;return -1;
}
#define open test_open
#define execve test_exec
#include "../src/helper.c"
#undef open
#undef execve
#undef fstat
#undef fgetxattr
static const char *ctx="u:object_r:extm_wear_dir:s0";
static const char *filectx="u:object_r:extm_wear_file:s0";
static void makefile(int d,const char *s,mode_t mode,const char *text) {
    int fd=openat(d,s,O_CREAT|O_EXCL|O_WRONLY,mode);assert(fd>=0);
    assert(!fchmod(fd,mode));if(text)assert(write(fd,text,strlen(text))==(ssize_t)strlen(text));close(fd);
}
static void parser_tests(void) {
    struct nzm_helper_identity id={.bytes=536870912,.inode=42,.fs_dev=2049,.backing_major=8,.backing_minor=1,.require_dm=1,.dm_major=253,.dm_minor=0};
    struct nzm_helper_result r;
    const char *s="extm-helper-v2 operation=bind ready=1 removed=0 bound=1 name=extm-file uuid=EXTM-FILE-v1-801-2a-20000000 target=extm-file target_version=1.0.0 bytes=536870912 dm_major=253 dm_minor=0 backing_major=8 backing_minor=1 inode=42 extents=1\n";
    size_t n=strlen(s);char b[1024];
    assert(!nzm_helper_parse(NZM_HELPER_BIND,&id,s,n,&r)&&r.bound&&r.dm_major==253);
    assert(nzm_helper_parse(NZM_HELPER_QUERY,&id,s,n,&r)<0);
    assert(nzm_helper_parse(NZM_HELPER_BIND,&id,s,n-1,&r)<0);
    for(size_t i=0;i<n;i++){memcpy(b,s,n);b[i]='\t';assert(nzm_helper_parse(NZM_HELPER_BIND,&id,b,n,&r)<0);}
    memcpy(b,s,n);b[n]='\n';assert(nzm_helper_parse(NZM_HELPER_BIND,&id,b,n+1,&r)<0);
    id.dm_minor=1;assert(nzm_helper_parse(NZM_HELPER_BIND,&id,s,n,&r)<0);id.dm_minor=0;
    id.inode++;assert(nzm_helper_parse(NZM_HELPER_BIND,&id,s,n,&r)<0);id.inode--;
    id.bytes*=2;assert(nzm_helper_parse(NZM_HELPER_BIND,&id,s,n,&r)<0);
}
static int exec_fixture(int argc,char **argv,char **env) {
    char c;struct stat st;
    assert(argc==3&&!strcmp(argv[0],"/vendor/bin/extm-file-helper")&&!strcmp(argv[1],"bind")&&!strcmp(argv[2],"536870912"));
    assert(env[0]&&!strcmp(env[0],"PATH=/vendor/bin:/system/bin"));
    assert(env[1]&&!strcmp(env[1],"LANG=C")&&!env[2]);
    for(int i=0;i<3;i++)assert(!fstat(i,&st));
    assert(read(0,&c,1)==0);
    DIR *d=opendir("/proc/self/fd");assert(d);struct dirent *e;
    while((e=readdir(d)))if(e->d_name[0]!='.') {int fd=atoi(e->d_name);assert(fd<=2||fd==dirfd(d));}
    closedir(d);
    const char *s="extm-helper-v2 operation=bind ready=1 removed=0 bound=1 name=extm-file uuid=EXTM-FILE-v1-801-2a-20000000 target=extm-file target_version=1.0.0 bytes=536870912 dm_major=253 dm_minor=0 backing_major=8 backing_minor=1 inode=42 extents=1\n";
    assert(write(1,s,strlen(s))==(ssize_t)strlen(s));assert(write(2,"fixture-ok",10)==10);return 0;
}
static void txn_tests(void) {
    for(int failure=1;failure<=10;failure++) {
        pid_t p=fork();assert(p>=0);
        if(!p) {
            int boundary=(failure-1)%5+1;
            sync_count=0;fail_sync=boundary;sync_errno=failure>5 ? ENOSPC : EIO;
            assert(nzm_io_ledger_commit(fixture,"new\n",4,ctx,filectx)==-sync_errno);
            fail_sync=0;
            assert(nzm_io_ledger_commit(fixture,"retry\n",6,ctx,filectx)<0);
            struct stat st;assert(!fstatat(fixture,"ledger.txn",&st,AT_SYMLINK_NOFOLLOW));
            if(boundary>=4)assert(fstatat(fixture,"ledger.new",&st,AT_SYMLINK_NOFOLLOW)<0&&errno==ENOENT);
            if(boundary==5) {
                /* Model failed restoration/absent evidence without clearing
                 * process poison: no caller can turn this into retry success. */
                assert(!unlinkat(fixture,"ledger.txn",0));
                assert(nzm_io_ledger_commit(fixture,"retry\n",6,ctx,filectx)==-EIO);
            }
            _exit(0);
        }
        int status;assert(waitpid(p,&status,0)==p&&WIFEXITED(status)&&!WEXITSTATUS(status));
        if(unlinkat(fixture,"ledger.txn",0))assert((failure-1)%5==4&&errno==ENOENT);
        if(unlinkat(fixture,"ledger.new",0))assert(errno==ENOENT);
    }
}
int main(int argc,char **argv,char **env) {
    if(argc==3)return exec_fixture(argc,argv,env);
    char path[]="/tmp/nzm-io-test-XXXXXX",b[16];size_t n;int fd,d,v,bin;
    struct nzm_meta fm={0,0,0600,1,NULL};struct nzm_run_lock lock;
    struct nzm_helper_identity id={.bytes=536870912,.inode=42,.fs_dev=2049,.backing_major=8,.backing_minor=1,.require_dm=1,.dm_major=253};
    struct nzm_helper_run run;struct nzm_helper_result result;
    assert(mkdtemp(path));fixture=open(path,O_RDONLY|O_DIRECTORY);assert(fixture>=0);assert(!fchmod(fixture,0700));
    makefile(fixture,"file",0600,"abc");
    assert(!nzm_io_read_at(fixture,"file",&fm,b,3,&n)&&n==3);
    assert(nzm_io_read_at(fixture,"file",&fm,b,2,&n)==-EFBIG);
    assert(nzm_io_read_at(fixture,"../file",&fm,b,sizeof b,&n)==-EINVAL);
    assert(!symlinkat("file",fixture,"sym"));assert(nzm_io_read_at(fixture,"sym",&fm,b,sizeof b,&n)<0);
    assert(!linkat(fixture,"file",fixture,"hard",0));assert(nzm_io_read_at(fixture,"file",&fm,b,sizeof b,&n)==-EPERM);assert(!unlinkat(fixture,"hard",0));
    fd=openat(fixture,"file",O_WRONLY);assert(fd>=0);write_calls=0;short_write=1;
    assert(nzm_io_sysfs_write(fd,"abc",3)==-EIO&&write_calls==1);short_write=0;close(fd);
    assert(!mkdirat(fixture,"data",0771));assert(!fchmodat(fixture,"data",0771,0));
    d=nzm_io_data_dir(fixture);assert(d>=0);
    assert((fcntl(d,F_GETFL)&(O_PATH|O_DIRECTORY|O_NOFOLLOW))==(O_PATH|O_DIRECTORY|O_NOFOLLOW));
    assert(fcntl(d,F_GETFD)&FD_CLOEXEC);
    assert(read(d,b,sizeof b)<0&&errno==EBADF);
    assert(!mkdirat(d,"extm",0700));
    int child=nzm_io_private_dir_at(d,"extm",ctx);assert(child>=0);
    assert(!(fcntl(child,F_GETFL)&O_PATH));close(child);
    bad_context=1;assert(nzm_io_private_dir_at(d,"extm",ctx)==-EPERM);bad_context=0;
    assert(!unlinkat(d,"extm",AT_REMOVEDIR));close(d);
    data_bad_uid=1;assert(nzm_io_data_dir(fixture)==-EPERM);data_bad_uid=0;
    data_bad_gid=1;assert(nzm_io_data_dir(fixture)==-EPERM);data_bad_gid=0;
    assert(!fchmodat(fixture,"data",0770,0));assert(nzm_io_data_dir(fixture)==-EPERM);
    assert(!fchmodat(fixture,"data",0771,0));
    assert(!renameat(fixture,"data",fixture,"data-real"));
    assert(!symlinkat("data-real",fixture,"data"));assert(nzm_io_data_dir(fixture)<0);
    assert(!unlinkat(fixture,"data",0));assert(!renameat(fixture,"data-real",fixture,"data"));
    assert(nzm_io_ledger_commit(fixture,"wear-v1\n",8,filectx,filectx)==-EPERM);
    space_unit=1;space_blocks=1048576+7;
    assert(nzm_io_ledger_preflight(fixture,8)==-ENOSPC);
    assert(nzm_io_ledger_commit(fixture,"wear-v1\n",8,ctx,filectx)==-ENOSPC);
    {struct stat s;assert(fstatat(fixture,"ledger.txn",&s,0)<0&&errno==ENOENT);
     assert(fstatat(fixture,"ledger.new",&s,0)<0&&errno==ENOENT);}
    space_error=EIO;assert(nzm_io_ledger_commit(fixture,"wear-v1\n",8,ctx,filectx)==-EIO);space_error=0;
    space_blocks=(fsblkcnt_t)UINT64_MAX;space_unit=4096;
    assert(nzm_io_ledger_preflight(fixture,8)==-EOVERFLOW);
    assert(nzm_io_ledger_preflight(fixture,NZM_LEDGER_MAX_BYTES+1)==-E2BIG);
    space_unit=1;space_blocks=1048576+8; /* Exact whole-new-file plus reserve. */
    assert(!nzm_io_ledger_preflight(fixture,8));
    assert(!nzm_io_ledger_commit(fixture,"wear-v1\n",8,ctx,filectx));
    space_unit=4096;space_blocks=UINT64_C(1)<<30;
    assert(nzm_io_ledger_commit(fixture,"wear-v1\n",8,ctx,ctx)==-EPERM);
    assert(nzm_io_ledger_commit(fixture,"wear-v1\n",8,filectx,ctx)==-EPERM);
    assert(nzm_io_ledger_commit(fixture,"wear-v1\n",8,NULL,filectx)==-EINVAL);
    assert(nzm_io_ledger_commit(fixture,"wear-v1\n",8,ctx,NULL)==-EINVAL);
    makefile(fixture,"ledger.new",0600,"pending");assert(nzm_io_ledger_commit(fixture,"x",1,ctx,filectx)==-EEXIST);
    assert(!unlinkat(fixture,"ledger.new",0));
    short_write=1; /* Persistent short writes eventually reach zero: retain txn. */
    assert(nzm_io_ledger_commit(fixture,"broken",6,ctx,filectx)==-EIO);short_write=0;
    assert(nzm_io_ledger_commit(fixture,"x",1,ctx,filectx)==-EEXIST);
    assert(!unlinkat(fixture,"ledger.txn",0)); /* Test fixture teardown only. */
    txn_tests();
    assert(!fchmodat(fixture,"file",0640,0));
    assert(nzm_io_read_at(fixture,"file",&fm,b,sizeof b,&n)==-EPERM);
    assert(!fchmodat(fixture,"file",0600,0));
    bad_context=1;assert(nzm_io_ledger_commit(fixture,"x",1,ctx,filectx)<0);bad_context=0;
    assert(!mkdirat(fixture,"lock",0700));assert(nzm_io_lock(fixture,ctx,&lock)==-EEXIST);assert(!unlinkat(fixture,"lock",AT_REMOVEDIR));
    assert(!nzm_io_lock(fixture,ctx,&lock));{struct nzm_run_lock other;assert(nzm_io_lock(fixture,ctx,&other)==-EEXIST);}assert(!nzm_io_unlock(fixture,&lock));
    /* Fake fixed filesystem tree for syscall-only helper execution adapter. */
    assert(!fchmod(fixture,0755));assert(!mkdirat(fixture,"vendor",0755));v=openat(fixture,"vendor",O_RDONLY|O_DIRECTORY);assert(v>=0);
    assert(!mkdirat(v,"bin",0755));bin=openat(v,"bin",O_RDONLY|O_DIRECTORY);assert(bin>=0);makefile(bin,"extm-file-helper",0755,NULL);
    {struct stat s;assert(!fstat(v,&s));vendor_dev=s.st_dev;vendor_ino=s.st_ino;assert(!fstat(bin,&s));bin_dev=s.st_dev;bin_ino=s.st_ino;}
    assert(!nzm_helper_preflight());
    wrong_helper_gid=1;assert(nzm_helper_preflight()==-EPERM);
    wrong_helper_gid=2;assert(nzm_helper_preflight()==-EPERM);wrong_helper_gid=0;
    assert(!fchmod(bin,0775));assert(nzm_helper_preflight()==-EPERM);assert(!fchmod(bin,0755));
    assert(!fchmod(v,0700));assert(nzm_helper_preflight()==-EPERM);assert(!fchmod(v,0755));
    bad_context=1;assert(nzm_helper_preflight()==-EPERM);bad_context=0;
    assert(!nzm_helper_preflight());
    for(child_mode=0;child_mode<=5;child_mode++) {
        int rc=nzm_helper_call(NZM_HELPER_BIND,&id,50,&run,&result);
        assert(rc<0&&run.attempted&&run.reaped&&!result.bound);
        if(child_mode==1){
            assert(rc==-ECHILD&&WIFEXITED(run.wait_status)&&WEXITSTATUS(run.wait_status)==42);
            char *log=NULL;size_t size=0;FILE *f=open_memstream(&log,&size);assert(f);
            nzm_helper_diagnostic(f,NZM_HELPER_BIND,&run,rc,0);assert(!fclose(f));
            assert(strstr(log,"helper_attempted=1 helper_reaped=1 wait_status=10752 writeback_attempted=0"));
            assert(strstr(log,"denied\\x0a\\x1b\\x22\\x5c"));
            assert(strchr(log,'\n')==log+size-1);free(log);
        }
        if(child_mode==2)assert(rc==-ETIMEDOUT);
        if(child_mode==3)assert(rc==-EOVERFLOW);
        if(child_mode==4)assert(rc==-EPROTO);
        errno=0;assert(waitpid(-1,NULL,WNOHANG)==-1&&errno==ECHILD);
    }
    bad_context=1;assert(nzm_helper_call(NZM_HELPER_BIND,&id,50,&run,&result)<0&&!run.attempted);bad_context=0;
    {char *log=NULL;size_t size=0;FILE *f=open_memstream(&log,&size);assert(f);
     memset(run.err,'x',sizeof run.err);run.err_len=NZM_HELPER_CAPTURE;
     nzm_helper_diagnostic(f,NZM_HELPER_BIND,&run,-EPERM,0);assert(!fclose(f));
     assert(size<600&&strstr(log,"helper_attempted=0")&&strstr(log,"stderr_truncated=1"));free(log);}
    /* Replace only our temporary fake helper with this host executable. */
    {int src=open("/proc/self/exe",O_RDONLY),dst=openat(bin,"extm-file-helper",O_WRONLY|O_TRUNC);char chunk[8192];ssize_t z;
     assert(src>=0&&dst>=0);while((z=read(src,chunk,sizeof chunk))>0)assert(write(dst,chunk,(size_t)z)==z);assert(!z);close(src);close(dst);}
    child_mode=6;
    for(int mask=0;mask<8;mask++) {
        pid_t p=fork();assert(p>=0);
        if(!p) {
            int leak=fcntl(fixture,F_DUPFD,128);assert(leak>=128);
            assert(!setenv("NZM_SECRET_MUST_NOT_LEAK","secret",1));
            for(int i=0;i<3;i++)if(mask&(1<<i))close(i);
            int rc=nzm_helper_call(NZM_HELPER_BIND,&id,2000,&run,&result);
            _exit(rc||!run.reaped||!result.bound||run.err_len!=10||memcmp(run.err,"fixture-ok",10)?1:0);
        }
        int status;assert(waitpid(p,&status,0)==p&&WIFEXITED(status)&&!WEXITSTATUS(status));
    }
    parser_tests();
    assert(!unlinkat(bin,"extm-file-helper",0));close(bin);assert(!unlinkat(v,"bin",AT_REMOVEDIR));close(v);assert(!unlinkat(fixture,"vendor",AT_REMOVEDIR));
    assert(!unlinkat(fixture,"data",AT_REMOVEDIR));assert(!unlinkat(fixture,"file",0));assert(!unlinkat(fixture,"sym",0));assert(!unlinkat(fixture,"ledger",0));close(fixture);assert(!rmdir(path));
    puts("io/helper tests passed (fixtures only)");return 0;
}
