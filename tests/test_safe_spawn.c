/* Host-only syscall fixture adapters. Production paths/metadata remain fixed.
 * All helpers are real ELF execs; no thread is added to a daemon. */
#define _GNU_SOURCE
#include "nzm_io.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/xattr.h>

static int rootfd,binfd;static dev_t vdev,bdev;static ino_t vino,bino;
static pid_t parent_pid;
static int fail_null,fail_pipe,fail_scan,fail_read_dir,fail_alloc,fail_dup,fail_fork;
static int swap_inode,deny_exec,steal_reap,probe_gate,pipe_calls;
static int hide_wait,inject_wait_eintr,inject_wait_echild,fail_kill;
static pid_t deferred_pid;
static _Thread_local pid_t child_pid;
static _Thread_local int child_reaped;
static atomic_int forks_seen,kills_seen,started_captures,waits_seen,execs_ok;
static atomic_bool churn_stop;
static atomic_ulong churn_count;
static pthread_mutex_t probe_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t probe_cv=PTHREAD_COND_INITIALIZER;
static int probe_ready,probe_go,probe_attempt,probe_acquired;

static void parent_only(void){assert(getpid()==parent_pid);}
static int fixture_stat(int fd,struct stat *st) {
    parent_only();int rc=fstat(fd,st);
    if(!rc) {
        st->st_uid=st->st_gid=0;
        if((st->st_dev==vdev&&st->st_ino==vino)||(st->st_dev==bdev&&st->st_ino==bino))st->st_gid=2000;
    }
    return rc;
}
static ssize_t fixture_xattr(int fd,const char *key,void *buf,size_t cap) {
    parent_only();(void)fd;const char label[]="u:object_r:extm_file_helper_exec:s0";
    assert(!strcmp(key,"security.selinux")&&cap>=sizeof label);
    memcpy(buf,label,sizeof label);return sizeof label;
}
#define fstat fixture_stat
#define fgetxattr fixture_xattr
#include "../src/io.c"
#undef fstat
#undef fgetxattr

static int fixture_open(const char *path,int flags,...) {
    parent_only();
    if(!strcmp(path,"/")) {
        assert(flags==(O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
        return fcntl(rootfd,F_DUPFD_CLOEXEC,3);
    }
    assert(!strcmp(path,"/dev/null")&&flags==(O_RDONLY|O_CLOEXEC));
    if(fail_null){errno=EACCES;return -1;}
    /* EOF pipe instead of opening any host/device node. */
    int p[2];if(pipe2(p,O_CLOEXEC))return -1;
    assert(!close(p[1]));return p[0];
}
static int fixture_pipe(int p[2],int flags) {
    parent_only();assert(flags==O_CLOEXEC);
    if(fail_pipe && ++pipe_calls==fail_pipe){errno=EMFILE;return -1;}
    return pipe2(p,flags);
}
static int fixture_fcntl(int fd,int op,...) {
    parent_only();va_list ap;va_start(ap,op);int arg=va_arg(ap,int);va_end(ap);
    if(fail_dup&&op==F_DUPFD_CLOEXEC){errno=EMFILE;return -1;}
    return fcntl(fd,op,arg);
}
static DIR *fixture_opendir(const char *s) {
    parent_only();assert(!strcmp(s,"/proc/self/fd"));
    if(fail_scan){errno=EACCES;return NULL;}return opendir(s);
}
static struct dirent *fixture_readdir(DIR *d) {
    parent_only();if(fail_read_dir){errno=EIO;return NULL;}return readdir(d);
}
static void *fixture_realloc(void *p,size_t n) {
    parent_only();if(fail_alloc){errno=ENOMEM;return NULL;}return realloc(p,n);
}
static pid_t fixture_fork(void) {
    parent_only();
    if(probe_gate) {
        assert(!pthread_mutex_lock(&probe_mu));probe_go=1;
        assert(!pthread_cond_broadcast(&probe_cv));
        while(!probe_attempt)assert(!pthread_cond_wait(&probe_cv,&probe_mu));
        assert(!pthread_mutex_unlock(&probe_mu));
        struct timespec delay={0,20000000};nanosleep(&delay,NULL);
        assert(!pthread_mutex_lock(&probe_mu));assert(!probe_acquired);
        assert(!pthread_mutex_unlock(&probe_mu));
    }
    if(swap_inode) {
        /* Swap the path AFTER the validated inode and close-list are pinned. */
        assert(!renameat(binfd,"extm-file-helper",binfd,"saved-helper"));
        assert(!renameat(binfd,"impostor",binfd,"extm-file-helper"));
    }
    if(deny_exec)assert(!fchmodat(binfd,"extm-file-helper",0644,0));
    if(fail_fork){errno=EAGAIN;return -1;}
    pid_t p=fork();
    if(p>0){child_pid=p;child_reaped=0;if(hide_wait)deferred_pid=p;atomic_fetch_add(&forks_seen,1);}
    return p;
}
static pid_t fixture_waitpid(pid_t p,int *st,int flags) {
    parent_only();assert(p>0&&flags==WNOHANG);
    assert(p==deferred_pid || (p==child_pid&&!child_reaped));
    atomic_fetch_add(&waits_seen,1);
    if(inject_wait_echild){errno=ECHILD;return -1;}
    if(inject_wait_eintr){errno=EINTR;return -1;}
    if(hide_wait)return 0;
    if(steal_reap) {
        pid_t w;do{w=waitpid(p,st,0);}while(w<0&&errno==EINTR);
        assert(w==p);child_reaped=1;errno=ECHILD;return -1;
    }
    pid_t w=waitpid(p,st,flags);if(w==p){if(p==child_pid)child_reaped=1;if(p==deferred_pid)deferred_pid=0;}return w;
}
static int fixture_kill(pid_t p,int sig) {
    parent_only();assert(p>0&&p==child_pid&&!child_reaped&&sig==SIGKILL);
    atomic_fetch_add(&kills_seen,1);if(fail_kill){errno=EPERM;return -1;}return kill(p,sig);
}
static ssize_t fixture_read(int fd,void *buf,size_t n) {
    parent_only();ssize_t z=read(fd,buf,n);
    if(z==7&&!memcmp(buf,"started",7))atomic_fetch_add(&started_captures,1);
    return z;
}
#define open fixture_open
#define pipe2 fixture_pipe
#define fcntl fixture_fcntl
#define opendir fixture_opendir
#define readdir fixture_readdir
#define realloc fixture_realloc
#define fork fixture_fork
#define waitpid fixture_waitpid
#define kill fixture_kill
#define read fixture_read
#include "../src/helper.c"
#undef open
#undef pipe2
#undef fcntl
#undef opendir
#undef readdir
#undef realloc
#undef fork
#undef waitpid
#undef kill
#undef read

static void delay_ms(unsigned ms) {
    struct timespec ts={ms/1000,(long)(ms%1000)*1000000};
    while(nanosleep(&ts,&ts))assert(errno==EINTR);
}
static int fd_count(void) {
    struct nzm_spawn_fd_guard g=NZM_SPAWN_FD_GUARD_INIT;
    assert(!nzm_spawn_fd_enter(&g));
    DIR *d=opendir("/proc/self/fd");struct dirent *e;int count=0;assert(d);
    while((e=readdir(d)))if(e->d_name[0]!='.')count++;
    assert(!closedir(d));nzm_spawn_fd_leave(&g);return count;
}
static void copy_elf(const char *source,const char *name) {
    int in=open(source,O_RDONLY|O_CLOEXEC),out=openat(binfd,name,O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0755);
    char buf[8192];ssize_t n;assert(in>=0&&out>=0);assert(!fchmod(out,0755));
    while((n=read(in,buf,sizeof buf))>0)assert(write(out,buf,(size_t)n)==n);
    assert(!n);assert(!close(in));assert(!close(out));
}
static void mask_equal(const sigset_t *a,const sigset_t *b) {
    for(int i=1;i<NSIG;i++)assert(sigismember(a,i)==sigismember(b,i));
}
static int call(unsigned bytes,unsigned ms,const struct nzm_helper_stop *stop,struct nzm_helper_run *run) {
    struct nzm_helper_identity id={.bytes=bytes};struct nzm_helper_result result;
    sigset_t before,after;int cancel;
    assert(!pthread_sigmask(SIG_SETMASK,NULL,&before));
    int rc=nzm_helper_call_controlled(NZM_HELPER_PREPARE,&id,ms,stop,run,&result);
    assert(!pthread_sigmask(SIG_SETMASK,NULL,&after));mask_equal(&before,&after);
    assert(!pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,&cancel));
    assert(cancel==PTHREAD_CANCEL_ENABLE);assert(!pthread_setcancelstate(cancel,NULL));
    assert(run->out_len<=NZM_HELPER_CAPTURE&&run->err_len<=NZM_HELPER_CAPTURE);
    if(!rc){assert(run->attempted&&run->reaped&&run->err_len==10&&!memcmp(run->err,"fixture-ok",10));atomic_fetch_add(&execs_ok,1);}
    return rc;
}
static void *churn(void *unused) {
    (void)unused;
    while(!atomic_load(&churn_stop)) {
        struct nzm_spawn_fd_guard g=NZM_SPAWN_FD_GUARD_INIT;int p[2],s[2];struct stat st;
        assert(!nzm_spawn_fd_enter(&g));
        /* Deliberately NOT CLOEXEC: exec must rely on the actual close-list. */
        int file=openat(rootfd,"sentinel",O_RDONLY),copy=dup(file);assert(file>=3&&copy>=3);
        int sparse=fcntl(file,F_DUPFD,4096);assert(sparse>=4096);
        assert(!pipe(p));assert(!socketpair(AF_UNIX,SOCK_STREAM,0,s));
        int ep=epoll_create1(0),ev=eventfd(0,0);sigset_t mask;sigemptyset(&mask);
        int sf=signalfd(-1,&mask,0);assert(ep>=3&&ev>=3&&sf>=3);
        assert(dup2(p[0],copy)==copy);assert(!close(file));
        file=openat(rootfd,"sentinel",O_RDONLY);assert(file>=3);
        nzm_spawn_fd_leave(&g);
        sched_yield(); /* real lifecycle spans other threads' fork snapshots */
        assert(!fstat(file,&st)&&S_ISREG(st.st_mode));
        assert(!fstat(sparse,&st)&&S_ISREG(st.st_mode));
        assert(!fstat(copy,&st)&&S_ISFIFO(st.st_mode));
        assert(!nzm_spawn_fd_enter(&g));
        int fds[]={file,copy,sparse,p[0],p[1],s[0],s[1],ep,ev,sf};
        for(size_t i=0;i<sizeof fds/sizeof *fds;i++)assert(!close(fds[i]));
        nzm_spawn_fd_leave(&g);atomic_fetch_add(&churn_count,1);
        delay_ms(1);
    }
    return NULL;
}
static void *owner_stress(void *unused) {
    (void)unused;
    for(int i=0;i<24;i++){struct nzm_helper_run run;assert(!call(4096,5000,NULL,&run));}
    return NULL;
}
static void *probe_mutator(void *unused) {
    (void)unused;struct nzm_spawn_fd_guard g=NZM_SPAWN_FD_GUARD_INIT;
    assert(!pthread_mutex_lock(&probe_mu));probe_ready=1;assert(!pthread_cond_broadcast(&probe_cv));
    while(!probe_go)assert(!pthread_cond_wait(&probe_cv,&probe_mu));
    probe_attempt=1;assert(!pthread_cond_broadcast(&probe_cv));assert(!pthread_mutex_unlock(&probe_mu));
    assert(!nzm_spawn_fd_enter(&g));
    assert(!pthread_mutex_lock(&probe_mu));probe_acquired=1;assert(!pthread_mutex_unlock(&probe_mu));
    int fd=openat(rootfd,"sentinel",O_RDONLY);assert(fd>=3);assert(!close(fd));
    nzm_spawn_fd_leave(&g);return NULL;
}
struct job { struct nzm_helper_stop stop;struct nzm_helper_run run;int rc;unsigned bytes; };
static void *owner_once(void *arg) {
    struct job *j=arg;j->rc=call(j->bytes,5000,&j->stop,&j->run);return NULL;
}
static void fault_tests(void) {
    struct nzm_helper_run run;int baseline=fd_count();
    int *faults[]={&fail_null,&fail_pipe,&fail_scan,&fail_read_dir,&fail_alloc,&fail_fork};
    const int errors[]={-EACCES,-EMFILE,-EACCES,-EIO,-ENOMEM,-EAGAIN};
    for(size_t i=0;i<sizeof faults/sizeof *faults;i++) {
        *faults[i]=1;pipe_calls=0;
        assert(call(4096,5000,NULL,&run)==errors[i]&&!run.attempted);
        *faults[i]=0;assert(fd_count()==baseline);assert(!call(4096,5000,NULL,&run));
    }
    fail_pipe=2;pipe_calls=0;assert(call(4096,5000,NULL,&run)==-EMFILE&&!run.attempted);fail_pipe=0;
    /* highfd failure with missing stdin: prove consumed low fd is not replayed. */
    int saved=fcntl(0,F_DUPFD_CLOEXEC,3);assert(saved>=3);assert(!close(0));fail_dup=1;
    assert(call(4096,5000,NULL,&run)==-EMFILE&&!run.attempted);fail_dup=0;
    assert(dup2(saved,0)==0);assert(!close(saved));assert(fd_count()==baseline);
    deny_exec=1;assert(call(4096,5000,NULL,&run)==-ECHILD&&run.reaped);
    assert(WIFEXITED(run.wait_status)&&WEXITSTATUS(run.wait_status)==127);deny_exec=0;
    assert(!fchmodat(binfd,"extm-file-helper",0755,0));
    int kills=atomic_load(&kills_seen);steal_reap=1;
    assert(call(4096,5000,NULL,&run)==-ECHILD&&!run.reaped);steal_reap=0;
    assert(atomic_load(&kills_seen)==kills);assert(fd_count()==baseline);
    struct sigaction saved_action,action={0};sigemptyset(&action.sa_mask);action.sa_handler=SIG_IGN;
    assert(!sigaction(SIGCHLD,&action,&saved_action));
    assert(call(4096,5000,NULL,&run)==-EINVAL&&!run.attempted);assert(!sigaction(SIGCHLD,&saved_action,NULL));
    action.sa_handler=SIG_DFL;action.sa_flags=SA_NOCLDWAIT;
    assert(!sigaction(SIGCHLD,&action,&saved_action));
    assert(call(4096,5000,NULL,&run)==-EINVAL&&!run.attempted);assert(!sigaction(SIGCHLD,&saved_action,NULL));
    action.sa_handler=SIG_IGN;action.sa_flags=0;
    assert(!sigaction(SIGTERM,&action,&saved_action));
    assert(call(4096,5000,NULL,&run)==-EINVAL&&!run.attempted);assert(!sigaction(SIGTERM,&saved_action,NULL));
    assert(fd_count()==baseline);
}
static void real_errors(void) {
    struct nzm_helper_run run;int baseline=fd_count();
    assert(call(8192,150,NULL,&run)==-ETIMEDOUT&&run.reaped);
    assert(WIFSIGNALED(run.wait_status)&&WTERMSIG(run.wait_status)==SIGKILL);
    assert(call(12288,5000,NULL,&run)==-EOVERFLOW&&run.reaped);
    assert(call(16384,5000,NULL,&run)==-EOVERFLOW&&run.reaped);
    assert(call(20480,5000,NULL,&run)==-EPROTO&&run.reaped);
    assert(call(24576,5000,NULL,&run)==-ECHILD&&run.reaped&&WIFEXITED(run.wait_status)&&WEXITSTATUS(run.wait_status)==42);
    assert(call(28672,5000,NULL,&run)==-ECHILD&&run.reaped&&WIFSIGNALED(run.wait_status)&&WTERMSIG(run.wait_status)==SIGTERM);
    assert(fd_count()==baseline);
}
static void stop_tests(void) {
    struct job j={.bytes=8192};pthread_t owner;int held[2],status;
    nzm_helper_stop_init(&j.stop);nzm_helper_stop_request(&j.stop);
    int forks=atomic_load(&forks_seen),kills=atomic_load(&kills_seen);
    assert(call(4096,5000,&j.stop,&j.run)==-ECANCELED&&!j.run.attempted);
    assert(atomic_load(&forks_seen)==forks&&atomic_load(&kills_seen)==kills);
    /* Another child is deliberately alive. No module operation may reap/kill it. */
    assert(!pipe(held));pid_t unrelated=fork();assert(unrelated>=0);
    if(!unrelated){char c;close(held[1]);_exit(read(held[0],&c,1)==1?0:1);}
    close(held[0]);
    nzm_helper_stop_init(&j.stop);int started=atomic_load(&started_captures);
    assert(!pthread_create(&owner,NULL,owner_once,&j));
    for(int i=0;i<500&&atomic_load(&started_captures)==started;i++)delay_ms(10);
    assert(atomic_load(&started_captures)>started);
    nzm_helper_stop_request(&j.stop);assert(!pthread_join(owner,NULL));
    assert(j.rc==-ECANCELED&&j.run.attempted&&j.run.reaped);
    assert(WIFSIGNALED(j.run.wait_status)&&WTERMSIG(j.run.wait_status)==SIGKILL);
    assert(waitpid(unrelated,&status,WNOHANG)==0);
    kills=atomic_load(&kills_seen);nzm_helper_stop_request(&j.stop);assert(atomic_load(&kills_seen)==kills);
    assert(write(held[1],"x",1)==1);close(held[1]);
    assert(waitpid(unrelated,&status,0)==unrelated&&WIFEXITED(status)&&!WEXITSTATUS(status));
    /* Completion races: flag-only stop cannot signal a PID after owner return. */
    for(int i=0;i<24;i++) {
        j.bytes=4096;nzm_helper_stop_init(&j.stop);assert(!pthread_create(&owner,NULL,owner_once,&j));
        if(i%2)delay_ms(1);
        nzm_helper_stop_request(&j.stop);assert(!pthread_join(owner,NULL));
        assert(j.rc==0||j.rc==-ECANCELED);
        assert(!j.run.attempted||j.run.reaped);
        kills=atomic_load(&kills_seen);nzm_helper_stop_request(&j.stop);assert(atomic_load(&kills_seen)==kills);
    }
}
/* Fault adapters hide real exit readiness, modeling a SIGKILL-pending child
 * without requiring uninterruptible host I/O or any device operations. */
static void pending_tests(void) {
    struct nzm_helper_run run;int baseline=fd_count(),forks,kills;
    int slots[HELPER_CHILD_SLOTS];
    for(int i=0;i<HELPER_CHILD_SLOTS;i++)assert(!child_entry(1,&slots[i]));
    forks=atomic_load(&forks_seen);
    assert(call(4096,100,NULL,&run)==-EBUSY&&!run.attempted);
    assert(atomic_load(&forks_seen)==forks);
    for(int i=0;i<HELPER_CHILD_SLOTS;i++)child_publish(slots[i],0,0);
    /* Persistent EINTR must not spin forever or signal without a zero probe. */
    hide_wait=1;inject_wait_eintr=1;kills=atomic_load(&kills_seen);
    int64_t start=millis();
    assert(call(8192,40,NULL,&run)==-ETIMEDOUT&&run.attempted&&!run.reaped);
    assert(millis()-start<1500&&atomic_load(&kills_seen)==kills);
    inject_wait_eintr=0;
    assert(call(4096,100,NULL,&run)==-EBUSY&&!run.attempted);
    assert(!kill(deferred_pid,SIGKILL));hide_wait=0;
    for(int i=0;i<200&&nzm_helper_preflight()==-EBUSY;i++)delay_ms(5);
    assert(!deferred_pid);assert(!call(4096,5000,NULL,&run));
    /* The original overflow beats cleanup's kill failure; ownership survives. */
    hide_wait=1;fail_kill=1;start=millis();
    assert(call(12288,5000,NULL,&run)==-EOVERFLOW&&run.attempted&&!run.reaped);
    assert(millis()-start<1500);fail_kill=0;
    assert(!kill(deferred_pid,SIGKILL));hide_wait=0;
    for(int i=0;i<200&&nzm_helper_preflight()==-EBUSY;i++)delay_ms(5);
    assert(!deferred_pid);
    /* Worker exits, but only process-owned PID state is retained, not job
     * storage or TLS. A different thread (here main) performs the exact reap. */
    struct job j={.bytes=8192};pthread_t owner;
    nzm_helper_stop_init(&j.stop);hide_wait=1;
    int started=atomic_load(&started_captures);
    assert(!pthread_create(&owner,NULL,owner_once,&j));
    for(int i=0;i<500&&atomic_load(&started_captures)==started;i++)delay_ms(10);
    assert(atomic_load(&started_captures)>started);
    nzm_helper_stop_request(&j.stop);assert(!pthread_join(owner,NULL));
    assert(j.rc==-ECANCELED&&j.run.attempted&&!j.run.reaped);
    forks=atomic_load(&forks_seen);kills=atomic_load(&kills_seen);
    assert(call(4096,100,NULL,&run)==-EBUSY&&!run.attempted);
    assert(nzm_helper_preflight()==-EBUSY);
    assert(atomic_load(&forks_seen)==forks&&atomic_load(&kills_seen)==kills);
    hide_wait=0;
    for(int i=0;i<200&&nzm_helper_preflight()==-EBUSY;i++)delay_ms(5);
    assert(!deferred_pid);assert(!call(4096,5000,NULL,&run));
    /* ECHILD at deferred entry releases ownership, never signals that PID. */
    hide_wait=1;
    assert(call(8192,40,NULL,&run)==-ETIMEDOUT&&!run.reaped);
    hide_wait=0;int status;assert(waitpid(deferred_pid,&status,0)==deferred_pid);
    inject_wait_echild=1;kills=atomic_load(&kills_seen);
    assert(!nzm_helper_preflight());inject_wait_echild=0;deferred_pid=0;
    assert(atomic_load(&kills_seen)==kills);
    assert(!call(4096,5000,NULL,&run));assert(fd_count()==baseline);
}
int main(int argc,char **argv) {
    assert(argc==3);parent_pid=getpid();char temp[]="/tmp/nzm-safe-spawn-XXXXXX";
    assert(mkdtemp(temp));rootfd=open(temp,O_RDONLY|O_DIRECTORY|O_CLOEXEC);assert(rootfd>=3);
    assert(!fchmod(rootfd,0755));assert(!mkdirat(rootfd,"vendor",0755));
    int vendor=openat(rootfd,"vendor",O_RDONLY|O_DIRECTORY|O_CLOEXEC);assert(vendor>=3);assert(!fchmod(vendor,0755));
    assert(!mkdirat(vendor,"bin",0755));binfd=openat(vendor,"bin",O_RDONLY|O_DIRECTORY|O_CLOEXEC);assert(binfd>=3);assert(!fchmod(binfd,0755));
    struct stat st;assert(!fstat(vendor,&st));vdev=st.st_dev;vino=st.st_ino;
    assert(!fstat(binfd,&st));bdev=st.st_dev;bino=st.st_ino;
    copy_elf(argv[1],"extm-file-helper");copy_elf(argv[2],"impostor");
    int sentinel=openat(rootfd,"sentinel",O_CREAT|O_EXCL|O_RDONLY|O_CLOEXEC,0600);assert(sentinel>=3);close(sentinel);
    assert(!setenv("NZM_SECRET_MUST_NOT_LEAK","secret",1));
    sigset_t blocked,original;sigemptyset(&blocked);sigaddset(&blocked,SIGTERM);sigaddset(&blocked,SIGINT);
    assert(!pthread_sigmask(SIG_BLOCK,&blocked,&original));
    struct nzm_helper_run run;int baseline=fd_count();assert(!nzm_helper_preflight());
    assert(!call(4096,5000,NULL,&run));
    { /* Explicit startup mask keeps unrelated blocks, not eventloop SIGTERM. */
        sigset_t execmask,before,after;struct nzm_helper_identity id={.bytes=32768};
        struct nzm_helper_result result;sigemptyset(&execmask);sigaddset(&execmask,SIGUSR1);
        assert(!pthread_sigmask(SIG_SETMASK,NULL,&before));
        assert(!nzm_helper_call_with_mask(NZM_HELPER_PREPARE,&id,5000,NULL,&execmask,&run,&result));
        assert(run.reaped&&run.err_len==10&&!memcmp(run.err,"fixture-ok",10));
        sigaddset(&execmask,SIGTERM);
        assert(nzm_helper_call_with_mask(NZM_HELPER_PREPARE,&id,5000,NULL,&execmask,&run,&result)==-EINVAL&&!run.attempted);
        assert(!pthread_sigmask(SIG_SETMASK,NULL,&after));mask_equal(&before,&after);
    }
    swap_inode=1;assert(!call(4096,5000,NULL,&run));swap_inode=0;
    assert(!renameat(binfd,"extm-file-helper",binfd,"impostor"));
    assert(!renameat(binfd,"saved-helper",binfd,"extm-file-helper"));
    /* Real exec for all eight missing-stdio combinations, plus sparse leak. */
    for(int mask=0;mask<8;mask++) {
        pid_t p=fork();assert(p>=0);
        if(!p) {
            parent_pid=getpid();int leak=fcntl(rootfd,F_DUPFD,8192);assert(leak>=8192);
            for(int i=0;i<3;i++)if(mask&(1<<i))close(i);
            _exit(call(4096,5000,NULL,&run)?1:0);
        }
        int status;assert(waitpid(p,&status,0)==p&&WIFEXITED(status)&&!WEXITSTATUS(status));
    }
    fault_tests();real_errors();stop_tests();pending_tests();
    pthread_t mutator;probe_gate=1;assert(!pthread_create(&mutator,NULL,probe_mutator,NULL));
    assert(!pthread_mutex_lock(&probe_mu));while(!probe_ready)assert(!pthread_cond_wait(&probe_cv,&probe_mu));
    assert(!pthread_mutex_unlock(&probe_mu));assert(!call(4096,5000,NULL,&run));
    assert(!pthread_join(mutator,NULL));assert(probe_acquired);probe_gate=0;
    pthread_t mutators[4],owners[3];
    for(int i=0;i<4;i++)assert(!pthread_create(&mutators[i],NULL,churn,NULL));
    while(atomic_load(&churn_count)<8)delay_ms(1);
    for(int i=0;i<3;i++)assert(!pthread_create(&owners[i],NULL,owner_stress,NULL));
    for(int i=0;i<3;i++)assert(!pthread_join(owners[i],NULL));
    atomic_store(&churn_stop,true);
    for(int i=0;i<4;i++)assert(!pthread_join(mutators[i],NULL));
    assert(atomic_load(&churn_count)>8);assert(fd_count()==baseline);
    for(int i=0;i<HELPER_CHILD_SLOTS;i++)assert(!children[i].used);
    assert(!pthread_sigmask(SIG_SETMASK,&original,NULL));
    printf("safe-spawn PASS: stdio=8 real_exec_ok=%d forks=%d owner_waits=%d owned_kills=%d fd_lifecycles=%lu\n",
        atomic_load(&execs_ok),atomic_load(&forks_seen),atomic_load(&waits_seen),atomic_load(&kills_seen),atomic_load(&churn_count));
    assert(!unlinkat(binfd,"extm-file-helper",0));assert(!unlinkat(binfd,"impostor",0));close(binfd);
    assert(!unlinkat(vendor,"bin",AT_REMOVEDIR));close(vendor);assert(!unlinkat(rootfd,"vendor",AT_REMOVEDIR));
    assert(!unlinkat(rootfd,"sentinel",0));close(rootfd);assert(!rmdir(temp));
    return 0;
}
