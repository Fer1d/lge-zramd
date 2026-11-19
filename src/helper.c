#define _GNU_SOURCE
#include "nzm_io.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <dirent.h>
#include <sys/sysmacros.h>
#ifdef NZM_HELPER_ADAPTER
#include NZM_HELPER_ADAPTER
#endif
static const char *opname(enum nzm_helper_op op) {
    switch(op) {
    case NZM_HELPER_PREPARE:return "preparefile";
    case NZM_HELPER_CREATE:return "create";
    case NZM_HELPER_QUERY:return "query";
    case NZM_HELPER_BIND:return "bind";
    default:return NULL;
    }
}
static int number(const char *s,uint64_t *v) {
    uint64_t n=0;
    if(!s || !*s || (s[0]=='0' && s[1])) return -EPROTO;
    for(;*s;s++) {
        unsigned d=(unsigned char)*s-'0';
        if(d>9 || n>(UINT64_MAX-d)/10) return -EPROTO;
        n=n*10+d;
    }
    *v=n;return 0;
}
int nzm_helper_parse(enum nzm_helper_op op,const struct nzm_helper_identity *id,
                     const char *s,size_t n,struct nzm_helper_result *out) {
    char buf[1024], canonical[1024],uuid[128]; char *tok[16],*p;
    uint64_t v[6]; int k; const char *name=opname(op);
    const char *keys[]={"dm_major=","dm_minor=","backing_major=","backing_minor=","inode=","extents="};
    if(!id || !s || !out || !name || !id->bytes || id->bytes>UINT64_C(4294967296) ||
       id->bytes%4096 || !n || n>=sizeof buf || s[n-1]!='\n' || memchr(s,0,n)) return -EPROTO;
    if(op==NZM_HELPER_PREPARE) {
        k=snprintf(canonical,sizeof canonical,"candidate prepared bytes=%"PRIu64" kernel_materialization_unverified=1\n",id->bytes);
        if(k<0 || (size_t)k!=n || memcmp(s,canonical,n)) return -EPROTO;
        memset(out,0,sizeof *out);return 0;
    }
    if(!id->inode || id->fs_dev>UINT32_MAX ||
       major((dev_t)id->fs_dev)!=id->backing_major ||
       minor((dev_t)id->fs_dev)!=id->backing_minor ||
       (op==NZM_HELPER_BIND && !id->require_dm)) return -EPROTO;
    memcpy(buf,s,n);buf[n-1]=0;p=buf;
    for(k=0;k<16;k++) {tok[k]=strsep(&p," ");if(!tok[k] || !*tok[k]) return -EPROTO;}
    if(p) return -EPROTO;
    for(k=0;k<6;k++) {
        size_t z=strlen(keys[k]);
        if(strncmp(tok[k+10],keys[k],z)||number(tok[k+10]+z,&v[k])) return -EPROTO;
    }
    if(v[0]<1||v[0]>4095||v[1]>1048575||v[2]>4095||v[3]>1048575||
       v[2]!=id->backing_major||v[3]!=id->backing_minor||v[4]!=id->inode||!v[5]||
       (id->require_dm && (v[0]!=id->dm_major||v[1]!=id->dm_minor))) return -EPROTO;
    snprintf(uuid,sizeof uuid,"EXTM-FILE-v1-%"PRIx64"-%"PRIx64"-%"PRIx64,id->fs_dev,id->inode,id->bytes);
    k=snprintf(canonical,sizeof canonical,
        "extm-helper-v2 operation=%s ready=1 removed=0 bound=%d name=extm-file uuid=%s target=extm-file target_version=1.0.0 bytes=%"PRIu64
        " dm_major=%"PRIu64" dm_minor=%"PRIu64" backing_major=%"PRIu64" backing_minor=%"PRIu64" inode=%"PRIu64" extents=%"PRIu64"\n",
        name,op==NZM_HELPER_BIND,uuid,id->bytes,v[0],v[1],v[2],v[3],v[4],v[5]);
    if(k<0 || (size_t)k!=n || memcmp(s,canonical,n)) return -EPROTO;
    out->dm_major=(unsigned)v[0];out->dm_minor=(unsigned)v[1];out->extents=v[5];out->bound=op==NZM_HELPER_BIND;return 0;
}
static int helper_open(void) {
    const struct nzm_meta dir={0,0,0755,0,NULL};
    const struct nzm_meta vendor_dir={0,2000,0755,0,NULL};
    const struct nzm_meta file={0,0,0755,1,"u:object_r:extm_file_helper_exec:s0"};
    int root,vendor,bin,fd,rc;
    root=open("/",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(root<0)return -errno;
    rc=nzm_io_check(root,S_IFDIR,&dir);if(rc){close(root);return rc;}
    vendor=nzm_io_dir_at(root,"vendor",&vendor_dir);close(root);if(vendor<0)return vendor;
    bin=nzm_io_dir_at(vendor,"bin",&vendor_dir);close(vendor);if(bin<0)return bin;
    fd=openat(bin,"extm-file-helper",O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);close(bin);
    if(fd<0)return -errno;
    rc=nzm_io_check(fd,S_IFREG,&file);if(rc){close(fd);return rc;}return fd;
}
/* Slots are reserved before fork: cleanup never allocates or loses a PID.
 * Active owners alone wait/signal; pending slots are reaped only under this
 * mutex on a later safe entry. No caller storage survives the call. */
#define HELPER_CHILD_SLOTS 16
#define HELPER_REAP_GRACE_MS 100
#define HELPER_REAP_POLLS 32
static pthread_mutex_t children_mu=PTHREAD_MUTEX_INITIALIZER;
static struct { pid_t pid; int used, pending; } children[HELPER_CHILD_SLOTS];
static int child_entry(int reserve,int *slot) {
    int k=pthread_mutex_lock(&children_mu),rc=0,free_slot=-1;
    if(k)return -k;
    for(int i=0;i<HELPER_CHILD_SLOTS;i++) {
        if(children[i].pending) {
            pid_t w=waitpid(children[i].pid,NULL,WNOHANG);
            if(w==children[i].pid || (w<0 && errno==ECHILD))
                memset(&children[i],0,sizeof children[i]);
            else rc=-EBUSY; /* Includes EINTR: one bounded probe per entry. */
        }
        if(!children[i].used && free_slot<0)free_slot=i;
    }
    if(!rc && reserve) {
        if(free_slot<0)rc=-EBUSY;
        else {children[free_slot].used=1;*slot=free_slot;}
    }
    if(pthread_mutex_unlock(&children_mu))abort();
    return rc;
}
static void child_publish(int slot,pid_t pid,int pending) {
    if(pthread_mutex_lock(&children_mu))abort();
    if(pid>0){children[slot].pid=pid;children[slot].pending=pending;}
    else memset(&children[slot],0,sizeof children[slot]);
    if(pthread_mutex_unlock(&children_mu))abort();
}
int nzm_helper_preflight(void) {
    struct nzm_spawn_fd_guard guard=NZM_SPAWN_FD_GUARD_INIT;
    int rc,fd;
#ifndef __ANDROID__
    int cancel_state;
    rc=pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,&cancel_state);if(rc)return -rc;
#endif
    rc=child_entry(0,NULL);
    if(!rc)rc=nzm_spawn_fd_enter(&guard);
    if(!rc){fd=helper_open();rc=fd<0?fd:(close(fd)?-errno:0);}
    if(guard.held)nzm_spawn_fd_leave(&guard);
#ifndef __ANDROID__
    if(pthread_setcancelstate(cancel_state,NULL))abort();
#endif
    return rc;
}
/* At most 256 input bytes, hex-escaped controls/quotes/backslashes: one log line. */
void nzm_helper_diagnostic(FILE *out,enum nzm_helper_op op,
                           const struct nzm_helper_run *run,int rc,int wb_attempted) {
    size_t limit=run->err_len<256?run->err_len:256;
    fprintf(out,"helper_operation=%s errno=%d helper_attempted=%d helper_reaped=%d "
            "wait_status=%d writeback_attempted=%d stderr_bytes=%zu stderr_truncated=%d stderr=\"",
            opname(op),rc<0?-rc:rc,run->attempted,run->reaped,run->wait_status,
            wb_attempted,run->err_len,run->err_len>limit);
    for(size_t i=0;i<limit;i++) {
        unsigned char c=(unsigned char)run->err[i];
        if(c>=32&&c<=126&&c!='"'&&c!='\\')fputc(c,out);
        else fprintf(out,"\\x%02x",c);
    }
    fputs("\"\n",out);
}
static int64_t millis(void) {
    struct timespec ts;if(clock_gettime(CLOCK_MONOTONIC,&ts)) return -1;
    return (int64_t)ts.tv_sec*1000+ts.tv_nsec/1000000;
}
static int capture(int fd,char *b,size_t *n,int *eof) {
    char scratch[512];ssize_t z=read(fd,scratch,sizeof scratch);
    if(z<0)return errno==EINTR||errno==EAGAIN ? 0 : -errno;
    if(!z){*eof=1;return 0;}
    if((size_t)z>NZM_HELPER_CAPTURE-*n)return -EOVERFLOW;
    memcpy(b+*n,scratch,(size_t)z);*n+=(size_t)z;b[*n]=0;return 0;
}
static int highfd(int fd) {
    int n;if(fd<0)return fd;
    if(fd>=3)return fd;
    n=fcntl(fd,F_DUPFD_CLOEXEC,3);if(n<0){int e=errno;close(fd);return -e;}
    close(fd);return n;
}
/* Parent-only Linux 4.9 compatible snapshot. The same process-wide FD gate
 * remains held from resource preparation through this enumeration and fork.
 * No stale list may be reused for a later fork, or replayed in the parent.
 * The scan descriptor is closed here and excluded from the child close list. */
struct fd_snapshot { int *fds; size_t count, capacity; };
static int snapshot_fds(int keep,struct fd_snapshot *list) {
    DIR *d=opendir("/proc/self/fd");struct dirent *ent;int scan,rc=0,found=0;
    if(!d)return -errno;
    scan=dirfd(d);
    for(;;) {
        char *end;long n;
        errno=0;ent=readdir(d);
        if(!ent){rc=errno?-errno:0;break;}
        if(ent->d_name[0]=='.')continue;
        errno=0;n=strtol(ent->d_name,&end,10);
        if(errno||*end||n<0||n>INT_MAX){rc=-EIO;break;}
        if(n==keep){found=1;continue;}
        if(n<=2||n==scan)continue;
        if(list->count==list->capacity) {
            size_t cap=list->capacity?list->capacity*2:64;
            int *p;
            if(cap>1048576){rc=-E2BIG;break;}
            p=realloc(list->fds,cap*sizeof *p);
            if(!p){rc=-ENOMEM;break;}
            list->fds=p;list->capacity=cap;
        }
        list->fds[list->count++]=(int)n;
    }
    if(closedir(d)&&!rc)rc=-errno;
    return rc?rc:(found?0:-EBADF);
}
void nzm_helper_stop_init(struct nzm_helper_stop *stop) {
    atomic_init(&stop->requested,false);
}
void nzm_helper_stop_request(struct nzm_helper_stop *stop) {
    atomic_store_explicit(&stop->requested,true,memory_order_release);
}
static int stopped(const struct nzm_helper_stop *stop) {
    return stop && atomic_load_explicit(&stop->requested,memory_order_acquire);
}
static void close_owned(int *fd) {
    /* Linux close releases the descriptor even on EINTR; NEVER retry. */
    if(*fd>=0){(void)close(*fd);*fd=-1;}
}
int nzm_helper_call_with_mask(enum nzm_helper_op op,const struct nzm_helper_identity *id,
                    unsigned timeout,const struct nzm_helper_stop *stop,
                    const sigset_t *exec_mask,struct nzm_helper_run *run,
                    struct nzm_helper_result *result) {
    int fd=-1,nullfd=-1,po[2]={-1,-1},pe[2]={-1,-1},rc=0,eof[2]={0,0},status=0;
    int masked=0,owned=0,k,slot=-1;
#ifndef __ANDROID__
    int cancel_state;
#endif
    pid_t pid=-1,w;int64_t start,now;char bytes[32],execpath[64];const char *name=opname(op);
    char *argv[]={"/vendor/bin/extm-file-helper",(char *)name,bytes,NULL};
    char *env[]={"PATH=/vendor/bin:/system/bin","LANG=C",NULL};
    sigset_t all_signals,parent_mask,child_mask;struct sigaction disposition;
    struct fd_snapshot list={0};
    struct nzm_spawn_fd_guard guard=NZM_SPAWN_FD_GUARD_INIT;
    if(!run || !result || !id || !name || !timeout || timeout>300000 || !id->bytes ||
       id->bytes>UINT64_C(4294967296)||id->bytes%4096) return -EINVAL;
    memset(run,0,sizeof *run);memset(result,0,sizeof *result);
    if(op!=NZM_HELPER_PREPARE && (!id->inode || id->fs_dev>UINT32_MAX ||
       major((dev_t)id->fs_dev)!=id->backing_major || minor((dev_t)id->fs_dev)!=id->backing_minor ||
       (op==NZM_HELPER_BIND && (!id->require_dm || !id->dm_major)))) return -EINVAL;
    if(stopped(stop))return -ECANCELED;
    start=millis();if(start<0)return -errno;
#ifndef __ANDROID__
    k=pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,&cancel_state);if(k)return -k;
#endif
    rc=child_entry(1,&slot);if(rc)goto done;
    rc=nzm_spawn_fd_enter(&guard);if(rc)goto done;
    if(sigaction(SIGCHLD,NULL,&disposition)){rc=-errno;goto done;}
    if(disposition.sa_handler!=SIG_DFL || (disposition.sa_flags&SA_NOCLDWAIT)) {
        rc=-EINVAL;goto done;
    }
    if(sigaction(SIGTERM,NULL,&disposition)){rc=-errno;goto done;}
    if(disposition.sa_handler==SIG_IGN){rc=-EINVAL;goto done;}
    sigfillset(&all_signals);
    if(exec_mask)child_mask=*exec_mask;else sigemptyset(&child_mask);
    if(sigismember(&child_mask,SIGTERM)!=0){rc=-EINVAL;goto done;}
    snprintf(bytes,sizeof bytes,"%"PRIu64,id->bytes);
    fd=helper_open();if(fd<0){rc=fd;goto done;}
    fd=highfd(fd);if(fd<0){rc=fd;goto done;}
    nullfd=open("/dev/null",O_RDONLY|O_CLOEXEC);
    if(nullfd<0){rc=-errno;goto done;}
    nullfd=highfd(nullfd);if(nullfd<0){rc=nullfd;goto done;}
    if(pipe2(po,O_CLOEXEC)){rc=-errno;goto done;}
    for(int i=0;i<2;i++){po[i]=highfd(po[i]);if(po[i]<0){rc=po[i];goto done;}}
    if(pipe2(pe,O_CLOEXEC)){rc=-errno;goto done;}
    for(int i=0;i<2;i++){pe[i]=highfd(pe[i]);if(pe[i]<0){rc=pe[i];goto done;}}
    if(fcntl(po[0],F_SETFL,O_NONBLOCK)<0 || fcntl(pe[0],F_SETFL,O_NONBLOCK)<0){rc=-errno;goto done;}
    /* Do not call Android/glibc fexecve here: implementations may format a
     * proc path or probe/fallback in libc. Prepare the sole exec target now.
     * execve is async-signal-safe. This proc handle resolves the STILL OPEN,
     * verified inode, not /vendor/bin again. FD_CLOEXEC removes it on success.
     * No fallback, no retry, and absolutely no execution after closing fd. */
    k=snprintf(execpath,sizeof execpath,"/proc/self/fd/%d",fd);
    if(k<0 || (size_t)k>=sizeof execpath){rc=-EOVERFLOW;goto done;}
    k=pthread_sigmask(SIG_SETMASK,&all_signals,&parent_mask);if(k){rc=-k;goto done;}
    masked=1;
    rc=snapshot_fds(fd,&list);if(rc)goto done;
    now=millis();if(now<0){rc=-EIO;goto done;}
    if(stopped(stop)){rc=-ECANCELED;goto done;}
    if(now-start>=timeout){rc=-ETIMEDOUT;goto done;}
    /* An earlier concurrent owner may have become pending during setup.
     * Serialize the final pending check, fork and PID publication as one step. */
    k=pthread_mutex_lock(&children_mu);if(k){rc=-k;goto done;}
    for(int i=0;i<HELPER_CHILD_SLOTS;i++)
        if(children[i].pending){rc=-EBUSY;break;}
    if(rc){if(pthread_mutex_unlock(&children_mu))abort();goto done;}
    pid=fork();
    if(pid!=0) {
        if(pid<0)rc=-errno;
        else children[slot].pid=pid;
        if(pthread_mutex_unlock(&children_mu))abort();
        if(pid<0)goto done;
    }
    if(!pid) {
        /* Child-only region: no allocator, stdio, directory API, pthread API,
         * logging, formatting, descriptor creation or mutex operation. */
        if(dup2(nullfd,0)<0 || dup2(po[1],1)<0 || dup2(pe[1],2)<0)_exit(126);
        for(size_t i=0;i<list.count;i++)
            if(close(list.fds[i]) && errno!=EINTR)_exit(126);
        if(sigprocmask(SIG_SETMASK,&child_mask,NULL))_exit(126);
        execve(execpath,argv,env);_exit(127);
    }
    run->attempted=1;owned=1;
    close_owned(&fd);close_owned(&nullfd);close_owned(&po[1]);close_owned(&pe[1]);
    free(list.fds);list.fds=NULL;
    k=pthread_sigmask(SIG_SETMASK,&parent_mask,NULL);masked=0;if(k)rc=-k;
    nzm_spawn_fd_leave(&guard);
    if(rc)goto reap;
    while(!run->reaped || !eof[0] || !eof[1]) {
        struct pollfd pf[2]={{eof[0]?-1:po[0],POLLIN,0},{eof[1]?-1:pe[0],POLLIN,0}};
        if(stopped(stop)){rc=-ECANCELED;break;}
        now=millis();if(now<0){rc=-EIO;break;}
        if(now-start>=timeout){rc=-ETIMEDOUT;break;}
        if(poll(pf,2,10)<0 && errno!=EINTR){rc=-errno;break;}
        if(!eof[0] && (rc=capture(po[0],run->out,&run->out_len,&eof[0])))break;
        if(!eof[1] && (rc=capture(pe[0],run->err,&run->err_len,&eof[1])))break;
        if(!run->reaped) {
            w=waitpid(pid,&status,WNOHANG);
            if(w==pid){run->reaped=1;owned=0;}
            else if(w<0&&errno!=EINTR){rc=-errno;if(errno==ECHILD)owned=0;break;}
        }
    }
reap:
    if(owned) {
        int killed=0;int64_t grace=millis();
        /* SIGKILL does not promise prompt exit (e.g. uninterruptible I/O).
         * Every wait is exact-PID and nonblocking, even after EINTR. The poll
         * ceiling also bounds clock failure/stall and repeated interruptions. */
        for(int i=0;i<HELPER_REAP_POLLS;i++) {
            w=waitpid(pid,&status,WNOHANG);
            if(w==pid){run->reaped=1;owned=0;break;}
            if(w<0 && errno!=EINTR) {
                if(!rc)rc=-errno;
                if(errno==ECHILD)owned=0;
                break;
            }
            if(w==0 && !killed) {
                /* Sole-reaper contract pins this PID until our exact reap. */
                if(kill(pid,SIGKILL)<0 && !rc)rc=-errno;
                killed=1;
            }
            now=millis();
            if(grace<0 || now<0 || now-grace>=HELPER_REAP_GRACE_MS)break;
            struct timespec pause={0,5000000};
            (void)nanosleep(&pause,NULL); /* Never retry interrupted sleep. */
        }
    }
    run->wait_status=status;
    if(!rc && (!run->reaped||!WIFEXITED(status)||WEXITSTATUS(status)))rc=-ECHILD;
    if(!rc)rc=nzm_helper_parse(op,id,run->out,run->out_len,result);
done:
    if(masked){k=pthread_sigmask(SIG_SETMASK,&parent_mask,NULL);if(k&&!rc)rc=-k;}
    free(list.fds);
    if(!guard.held && (fd>=0||nullfd>=0||po[0]>=0||po[1]>=0||pe[0]>=0||pe[1]>=0)) {
        /* An internal pthread failure cannot justify ungated descriptor reuse. */
        if(nzm_spawn_fd_enter(&guard))abort();
    }
    close_owned(&fd);close_owned(&nullfd);
    for(int i=0;i<2;i++){close_owned(&po[i]);close_owned(&pe[i]);}
    if(guard.held)nzm_spawn_fd_leave(&guard);
    if(slot>=0)child_publish(slot,owned?pid:0,owned);
#ifndef __ANDROID__
    if(pthread_setcancelstate(cancel_state,NULL))abort();
#endif
    return rc;
}
int nzm_helper_call_controlled(enum nzm_helper_op op,const struct nzm_helper_identity *id,
                    unsigned timeout,const struct nzm_helper_stop *stop,
                    struct nzm_helper_run *run,struct nzm_helper_result *result) {
    return nzm_helper_call_with_mask(op,id,timeout,stop,NULL,run,result);
}
int nzm_helper_call(enum nzm_helper_op op,const struct nzm_helper_identity *id,
                    unsigned timeout,struct nzm_helper_run *run,struct nzm_helper_result *result) {
    return nzm_helper_call_controlled(op,id,timeout,NULL,run,result);
}
