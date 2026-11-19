/* An ordinary ELF actually exec'd by the helper, never an exec syscall mock.
 * Run unsanitized so sanitizer startup cannot hide/open inherited descriptors. */
#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc,char **argv,char **env) {
    struct stat st;sigset_t mask;char c;
    assert(argc==3 && !strcmp(argv[0],"/vendor/bin/extm-file-helper"));
    assert(!strcmp(argv[1],"preparefile"));
    assert(env[0] && !strcmp(env[0],"PATH=/vendor/bin:/system/bin"));
    assert(env[1] && !strcmp(env[1],"LANG=C") && !env[2]);
    for(int i=0;i<3;i++)assert(!fstat(i,&st));
    assert(read(0,&c,1)==0);
    assert(!sigprocmask(SIG_SETMASK,NULL,&mask));
    for(int i=1;i<NSIG;i++)
        assert(sigismember(&mask,i)==(!strcmp(argv[2],"32768")&&i==SIGUSR1));
    DIR *d=opendir("/proc/self/fd");struct dirent *e;assert(d);
    errno=0;
    while((e=readdir(d))) {
        if(e->d_name[0]=='.')continue;
        char *end;long fd=strtol(e->d_name,&end,10);
        assert(!*end && fd>=0 && (fd<=2||fd==dirfd(d)));
    }
    assert(!errno);assert(!closedir(d));
#ifdef IMPOSTOR
    (void)write(2,"WRONG-INODE",11);return 93;
#endif
    unsigned long bytes=strtoul(argv[2],NULL,10);
    if(bytes==8192) {
        assert(write(2,"started",7)==7);
        sleep(5);return 0;
    }
    if(bytes==12288 || bytes==16384) {
        char chunk[8192];memset(chunk,'x',sizeof chunk);
        assert(write(bytes==12288?2:1,chunk,sizeof chunk)==(ssize_t)sizeof chunk);
        return 0;
    }
    if(bytes==20480){assert(write(1,"partial bind\n",13)==13);return 0;}
    if(bytes==24576){assert(write(2,"denied\n\033\"\\",10)==10);return 42;}
    if(bytes==28672){raise(SIGTERM);return 94;}
    char text[256];int n=snprintf(text,sizeof text,
        "candidate prepared bytes=%lu kernel_materialization_unverified=1\n",bytes);
    assert(n>0 && (size_t)n<sizeof text);
    assert(write(1,text,(size_t)n)==n);
    assert(write(2,"fixture-ok",10)==10);
    return 0;
}
