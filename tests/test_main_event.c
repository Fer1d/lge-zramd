#define _GNU_SOURCE
#define NZM_EVENT_IMPLEMENTATION
#include "event_adapter.h"
#include "nzm_core.h"
#include "uapi/linux/zram.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <inttypes.h>
#include <linux/fs.h>
#include <linux/magic.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/signalfd.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

/* Independent, process-per-case fixture. No test_core.c/success.inc inclusion.
 * Real: main/config/policy/boot/mkswap/VM/ledger parser+transaction/recovery,
 * request transport, pthread runtime, epoll/signalfd/timerfd, PSI, shared FD gate.
 * Injected: Android UID/GID/SELinux and mount provenance, helper outcomes,
 * block/sysfs/ioctl effects. All ordinary files and durable ledger operations
 * are real tempfile I/O. No device, host VM, or host pressure node is touched.
 * The producer never reads core or kernel-model state: only sync controls and
 * runtime APIs. The main thread owns setup/config; after launch only the real
 * worker owns fake I/O, until the runtime has joined it. Clocks remain REAL. */
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"CHECK %s:%d: %s errno=%d\n",__FILE__,__LINE__,#x,errno); abort(); } } while (0)
#define WEAR "/data/vendor/extm/state"
#define RUN "/dev/lge-zram-manager"
#define Z "/sys/block/zram0/"
#define DISK UINT64_C(67108864)
#define CHARGE UINT64_C(4096)
enum phase { PH_NONE, PH_CYCLE_DONE, PH_BEFORE, PH_OBSERVING, PH_PRECOMMIT,
             PH_INTENT_SYNC, PH_POSTCHARGE, PH_SUBMITTING, PH_EXEC, PH_FIRST_CYCLE, PH_CONFIG_CHANGED };
enum psi_mode { PSI_OFF, PSI_MISSING, PSI_DENIED, PSI_REAL_PRI };
struct scenario {
    const char *name, *backend;
    int event, legacy_kernel, exec_error;
    enum phase stop_at;
    enum psi_mode psi;
    int retire_while_blocked, policy_block, change_config;
};
struct controls {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    enum phase reached;
    int release;
    struct nzm_runtime *runtime;
    atomic_int active, initialized, finalized, producer_done, request_live;
    atomic_uint psi_acks, psi_closes, joins, runtime_closes, jobs, hup_reads;
    atomic_int psi_peer;
};
struct fixture {
    struct scenario scenario;
    char root[256];
    dev_t storage_dev;
    pthread_t eventloop, worker;
    struct controls ctl;
    struct nzm_runtime_executor real_executor;
    struct nzm_runtime_stats stats;
    unsigned helpers[4], swaps, execs, legacy_calls, status_calls;
    unsigned quota_writes, idle_writes, submitting_checks, config_opens;
    int in_execute, intent_sync_pending;
    int phase1, disk_backed_runtime, bind_during_init, boot_wait, wrong_label, fail_disksize;
    unsigned disksize_writes, completed_reads, sleeps;
    uint64_t installed;
    char last_event[1200];
};
static struct fixture f;
int nzm_entry(int, char **);
uid_t test_geteuid(void) { return 0; }

static void owner(void) {
    if (atomic_load(&f.ctl.active)) {
        CHECK(atomic_load(&f.ctl.initialized));
        CHECK(pthread_equal(pthread_self(),f.worker));
    }
}
static void path(char *out, size_t size, const char *leaf) {
    int n=snprintf(out,size,"%s%s",f.root,leaf);
    CHECK(n>=0 && (size_t)n<size);
}
static void fdpath(int fd, char *out, size_t size) {
    char link[64];
    CHECK(snprintf(link,sizeof link,"/proc/self/fd/%d",fd)>0);
    ssize_t n=readlink(link,out,size-1);
    CHECK(n>=0 && (size_t)n<size-1); out[n]=0;
}
static const char *leafpath(int fd, char *buf, size_t size) {
    fdpath(fd,buf,size);
    CHECK(!strncmp(buf,f.root,strlen(f.root)));
    return buf+strlen(f.root);
}
static void put(const char *leaf, const char *text) {
    owner(); char p[1024]; path(p,sizeof p,leaf);
    int fd=open(p,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0600); CHECK(fd>=0);
    CHECK(write(fd,text,strlen(text))==(ssize_t)strlen(text)); CHECK(!close(fd));
}
static void dir(const char *leaf, mode_t mode) {
    char p[1024]; path(p,sizeof p,leaf); CHECK(!mkdir(p,mode)); CHECK(!chmod(p,mode));
}
static int exists(const char *leaf) {
    owner(); char p[1024]; path(p,sizeof p,leaf);
    int rc=access(p,F_OK); CHECK(!rc || errno==ENOENT); return !rc;
}
static uint64_t number(const char *leaf) {
    owner(); char p[1024],text[80]; path(p,sizeof p,leaf);
    int fd=open(p,O_RDONLY|O_CLOEXEC); CHECK(fd>=0);
    ssize_t n=read(fd,text,sizeof text-1); CHECK(n>0); text[n]=0; CHECK(!close(fd));
    return strtoull(text,NULL,10);
}
static uint64_t ledger(void) {
    owner(); char p[1024],text[8192]; path(p,sizeof p,WEAR "/ledger");
    int fd=open(p,O_RDONLY|O_CLOEXEC); CHECK(fd>=0);
    ssize_t n=read(fd,text,sizeof text); CHECK(n>0); CHECK(!close(fd));
    struct nzm_ledger l={0}; CHECK(!nzm_ledger_parse(text,(size_t)n,(uint64_t)time(NULL),&l));
    CHECK(l.generation==l.count);
    uint64_t charge=l.cumulative; nzm_ledger_free(&l);
    CHECK(!exists(WEAR "/ledger.txn") && !exists(WEAR "/ledger.new"));
    return charge;
}
static void project(const char *p, struct stat *st) {
    st->st_uid=st->st_gid=0;
    if (!strcmp(p,"/data")) st->st_uid=st->st_gid=1000;
    if (!strcmp(p,"/mnt/vendor/persist")) st->st_gid=1000;
    if (!strcmp(p,"/dev/block/zram0")) { st->st_mode=S_IFBLK|0600; st->st_rdev=makedev(0,1); }
    if (!strcmp(p,"/dev/block/sda26") || !strcmp(p,"/dev/block/sda5") ||
        !strncmp(p,"/dev/block/bootdevice/by-name/",30)) {
        st->st_mode=S_IFBLK|0600; st->st_rdev=f.storage_dev;
    }
}
int event_open(const char *p, int flags, ...) {
    owner(); mode_t mode=0;
    if (flags&O_CREAT) { va_list ap; va_start(ap,flags); mode=(mode_t)va_arg(ap,int); va_end(ap); }
    char mapped[1024],target[1024];
    if (!strncmp(p,"/proc/self/fdinfo/",18)) {
        int fd=atoi(p+18); const char *leaf=leafpath(fd,target,sizeof target);
        CHECK(!strcmp(leaf,"/data") || !strcmp(leaf,"/data/vendor") ||
              !strcmp(leaf,"/mnt/vendor/persist"));
        p=!strcmp(leaf,"/data") ? "/proc/self/data-fdinfo" :
          !strcmp(leaf,"/data/vendor") ? "/proc/self/vendor-fdinfo" : "/proc/self/persist-fdinfo";
    }
    CHECK(p[0]=='/');
    if (f.phase1) {
        CHECK(strncmp(p,"/mnt/vendor/persist",19));
        CHECK(strncmp(p,WEAR "/ledger",strlen(WEAR "/ledger")));
        CHECK(strcmp(p,"/data/vendor/extm/file") && strcmp(p,"/data/vendor/extm/owner") &&
              strncmp(p,"/proc/pressure",14));
        if (flags&(O_RDWR|O_WRONLY|O_CREAT))
            CHECK(!strcmp(p,Z "disksize") || !strcmp(p,"/dev/block/zram0") ||
                  !strncmp(p,"/proc/sys/vm/",13));
    }
    path(mapped,sizeof mapped,p);
    return open(mapped,flags,mode);
}
int event_openat(int d, const char *p, int flags, ...) {
    owner(); CHECK(p[0]!='/'); mode_t mode=0;
    if (flags&O_CREAT) { va_list ap; va_start(ap,flags); mode=(mode_t)va_arg(ap,int); va_end(ap); }
    if (!strcmp(p,"lge-zram.conf")) f.config_opens++;
    if (f.phase1) {
        char target[1024];
        const char *leaf=leafpath(d,target,sizeof target);
        CHECK(strcmp(p,"persist") && strncmp(leaf,"/mnt/vendor/persist",19));
        if (!strcmp(leaf,"/data/vendor/extm"))
            CHECK(!strcmp(p,"state")); /* minimal startup only, never physical backend */
        if (!strcmp(leaf,WEAR)) CHECK(strncmp(p,"ledger",6));
        if (flags&(O_RDWR|O_WRONLY|O_CREAT)) {
            CHECK(!strcmp(leaf,WEAR));
            CHECK(!strcmp(p,"boot-state.txn") || !strcmp(p,"boot-state.new"));
        }
    }
    return openat(d,p,flags,mode);
}
int event_fstat(int fd, struct stat *st) {
    owner(); int rc=fstat(fd,st); if (rc) return rc;
    char p[1024]; project(leafpath(fd,p,sizeof p),st); return 0;
}
int event_fstatfs(int fd, struct statfs *fs) {
    (void)fd; (void)fs; CHECK(0); return -1; /* No runtime mount is consulted. */
}
static int checkpoint(const char *name) {
    char p[1024]; path(p,sizeof p,WEAR);
    int fd=open(p,O_RDONLY|O_DIRECTORY|O_CLOEXEC); CHECK(fd>=0);
    struct nzm_boot_state state; nzm_boot_init(&state);
    int rc=nzm_boot_load(&state,fd);
    CHECK(!close(fd));
    if(rc==-ENOENT) return 0;
    CHECK(!rc); rc=nzm_boot_absent(&state,name); CHECK(!rc || rc==-EEXIST);
    return rc==-EEXIST;
}
static int pathstat(const char *p, struct stat *st, int nofollow) {
    owner(); char mapped[1024]; path(mapped,sizeof mapped,p);
    int rc=nofollow ? lstat(mapped,st) : stat(mapped,st);
    if (!rc) project(p,st);
    return rc;
}
int event_stat(const char *p, struct stat *st) { return pathstat(p,st,0); }
int event_lstat(const char *p, struct stat *st) { return pathstat(p,st,1); }
ssize_t event_xattr(int fd, const char *name, void *buf, size_t size) {
    owner(); CHECK(!strcmp(name,"security.selinux")); char p[1024],label[128];
    const char *leaf=leafpath(fd,p,sizeof p), *type="lge_zram_runtime_file";
    if (!strcmp(leaf,"/vendor/etc/lge-zram.conf")) type="vendor_configs_file";
    else if (!strcmp(leaf,"/data/vendor")) type="vendor_data_file";
    else if (!strcmp(leaf,"/dev/block/zram0")) type=f.wrong_label ? "untrusted_block_device" : "lge_zram0_device";
    else if (!strncmp(leaf,WEAR,strlen(WEAR))) {
        type="extm_wear_dir";
        if (strstr(leaf,"/authority")) type="extm_wear_authority";
        else if (strstr(leaf,"/ledger") || strstr(leaf,"/exec.intent") || strstr(leaf,"/boot-state")) type="extm_wear_ledger";
        else if (strstr(leaf,"/pause")) type="extm_wear_pause";
    } else if (!strncmp(leaf,"/data/vendor/extm",strlen("/data/vendor/extm"))) {
        type="extm_root_data";
        if (strstr(leaf,"/owner")) type="extm_owner_data";
        else if (strstr(leaf,"/file")) type="extm_backing_data";
    }
    int n=snprintf(label,sizeof label,"u:object_r:%s:s0",type);
    CHECK(n>0 && (size_t)n<sizeof label);
    if ((size_t)n+1>size) { errno=ERANGE; return -1; }
    memcpy(buf,label,(size_t)n+1); return n+1;
}
static void publish(enum phase p, int wait) {
    CHECK(!pthread_mutex_lock(&f.ctl.mutex));
    f.ctl.release=0; f.ctl.reached=p; CHECK(!pthread_cond_broadcast(&f.ctl.cond));
    while (wait && !f.ctl.release) CHECK(!pthread_cond_wait(&f.ctl.cond,&f.ctl.mutex));
    CHECK(!pthread_mutex_unlock(&f.ctl.mutex));
}
ssize_t event_write(int fd, const void *buf, size_t size) {
    owner(); char p[1024]; const char *leaf=leafpath(fd,p,sizeof p);
    if (!strcmp(leaf,Z "disksize")) {
        f.disksize_writes++;
        if (f.phase1) CHECK(checkpoint("ram-only-attempted"));
        if (f.fail_disksize) { errno=EIO; return -1; }
        if (f.bind_during_init) put(Z "backing_dev","/dev/block/dm-0\n");
        CHECK(!ftruncate(fd,0)); CHECK(lseek(fd,0,SEEK_SET)==0);
        CHECK(write(fd,"67108864\n",9)==9); return (ssize_t)size;
    }
    if (!strcmp(leaf,Z "idle")) f.idle_writes++;
    if (!strncmp(leaf,Z "writeback_limit",strlen(Z "writeback_limit"))) f.quota_writes++;
    if (!strcmp(leaf,Z "writeback")) {
        CHECK(ledger()==CHARGE); CHECK(!exists(WEAR "/exec.intent")); f.legacy_calls++;
        put(Z "bd_stat","1024 0 1024\n");
    }
    if (!strncmp(leaf,"/proc/sys/vm/",13) || !strncmp(leaf,Z,strlen(Z))) {
        CHECK(!ftruncate(fd,0)); CHECK(lseek(fd,0,SEEK_SET)==0);
    }
    return write(fd,buf,size);
}
int event_close(int fd) { owner(); return close(fd); }
int event_fsync(int fd) {
    owner(); char p[1024]; const char *leaf=leafpath(fd,p,sizeof p);
    int rc=fsync(fd);
    if (!rc && f.scenario.stop_at==PH_INTENT_SYNC) {
        if (!strcmp(leaf,WEAR "/exec.intent")) f.intent_sync_pending=1;
        else if (!strcmp(leaf,WEAR) && f.intent_sync_pending) {
            f.intent_sync_pending=0; CHECK(ledger()==0); publish(PH_INTENT_SYNC,1);
        }
    }
    return rc;
}
int event_space(int fd, struct statvfs *st) {
    owner(); CHECK(fcntl(fd,F_GETFD)>=0); memset(st,0,sizeof *st);
    st->f_frsize=4096; st->f_bavail=1048576; return 0;
}
int event_swapon(const char *name, int flags) {
    owner(); CHECK(!strcmp(name,"/dev/block/zram0") && !flags);
    char p[1024],header[4096]; path(p,sizeof p,name);
    int fd=open(p,O_RDONLY|O_CLOEXEC); CHECK(fd>=0);
    CHECK(read(fd,header,sizeof header)==sizeof header); CHECK(!close(fd));
    uint32_t version,last; memcpy(&version,header+1024,4); memcpy(&last,header+1028,4);
    CHECK(version==1 && last==16383 && !memcmp(header+4096-10,"SWAPSPACE2",10));
    f.swaps++; put("/proc/swaps","Filename Type Size Used Priority\n/dev/block/zram0 partition 65532 0 -2\n");
    return 0;
}
int event_ioctl(int fd, unsigned long op, ...) {
    owner(); va_list ap; va_start(ap,op); void *arg=va_arg(ap,void *); va_end(ap);
    if (op==BLKGETSIZE64) { *(uint64_t *)arg=DISK; return 0; }
    CHECK(!f.phase1);
    CHECK(fcntl(fd,F_GETFD)&FD_CLOEXEC); CHECK(fcntl(fd,F_GETFL)&O_NOFOLLOW);
    if (op==ZRAM_IOC_WB_STATUS) {
        f.status_calls++; CHECK((fcntl(fd,F_GETFL)&O_ACCMODE)==O_RDONLY);
        if (f.scenario.legacy_kernel) { errno=ENOTTY; return -1; }
        struct zram_wb_status *s=arg; CHECK(s->size==sizeof *s && s->version==1);
        s->capabilities=ZRAM_WB_CAP_STATUS|ZRAM_WB_CAP_EXEC|ZRAM_WB_CAP_FILTERED_IDLE|ZRAM_WB_CAP_DUAL_LIMIT;
        s->instance=11; s->generation=7; s->disksize_bytes=DISK; s->page_size=4096;
        s->state_flags=ZRAM_WB_STATE_INITIALIZED|ZRAM_WB_STATE_BACKING;
        s->pages_stored=DISK/4096; s->lowratio_pages=s->pages_stored;
        s->bd_count_pages=f.installed; s->bd_writes_pages=f.installed;
        return 0;
    }
    CHECK(op==ZRAM_IOC_WB_EXEC && (fcntl(fd,F_GETFL)&O_ACCMODE)==O_RDWR);
    struct zram_wb_exec *r=arg;
    CHECK(r->size==sizeof *r && r->version==1 && r->mode==ZRAM_WB_MODE_FILTERED_IDLE);
    CHECK(r->expected_instance==11 && r->expected_generation==7 && r->request_id==1);
    CHECK(r->max_attempt_pages==1024 && r->max_installed_pages==1024 && r->idle_count==1);
    CHECK(ledger()==CHARGE && exists(WEAR "/exec.intent"));
    f.execs++; atomic_store(&f.ctl.request_live,1);
    if (f.scenario.stop_at==PH_EXEC) publish(PH_EXEC,1);
    if (f.scenario.exec_error) {
        /* Queued reasons while the real worker is blocked cannot cause replay.
         * Error handling, not a concurrent SIGTERM, must stop this daemon. */
        CHECK(!pthread_mutex_lock(&f.ctl.mutex));
        while (!atomic_load(&f.ctl.psi_acks)) CHECK(!pthread_cond_wait(&f.ctl.cond,&f.ctl.mutex));
        CHECK(!pthread_mutex_unlock(&f.ctl.mutex));
    }
    CHECK(fcntl(fd,F_GETFD)>=0); /* Nobody closes worker request to unblock it. */
    f.installed=1024; put(Z "bd_stat","1024 0 1024\n");
    atomic_store(&f.ctl.request_live,0);
    if (f.scenario.exec_error) { errno=f.scenario.exec_error; return -1; }
    r->scanned_pages=r->selected_pages=r->submitted_pages=r->installed_pages=1024;
    r->stop_reason=ZRAM_WB_STOP_INSTALLED_LIMIT; return 0;
}
int event_helper(enum nzm_helper_op op, const struct nzm_helper_identity *id,
                 unsigned timeout, const struct nzm_helper_stop *stop, const sigset_t *mask,
                 struct nzm_helper_run *run, struct nzm_helper_result *result) {
    owner(); CHECK(!f.phase1 && timeout==120000 && !stop && id->bytes==16*1048576ULL);
    if (f.scenario.event) {
        CHECK(mask && sigismember(mask,SIGTERM)==0 && sigismember(mask,SIGUSR1)==1);
        sigset_t current; CHECK(!pthread_sigmask(SIG_SETMASK,NULL,&current));
        CHECK(sigismember(&current,SIGTERM)==1);
    } else CHECK(!mask);
    f.helpers[op]++; memset(run,0,sizeof *run); memset(result,0,sizeof *result);
    run->attempted=run->reaped=1;
    if (op==NZM_HELPER_PREPARE) {
        char p[1024]; path(p,sizeof p,"/data/vendor/extm/file");
        int fd=open(p,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600); CHECK(fd>=0);
        CHECK(!ftruncate(fd,(off_t)id->bytes)); CHECK(!close(fd)); return 0;
    }
    CHECK(id->inode && id->fs_dev); result->dm_major=253; result->extents=1;
    if (op==NZM_HELPER_CREATE) {
        CHECK(!id->require_dm); dir("/sys/block/dm-0",0755); dir("/sys/block/dm-0/dm",0755);
        put("/sys/block/dm-0/dm/name","extm-file\n"); put("/sys/block/dm-0/dev","253:0\n");
    } else {
        CHECK(id->require_dm && id->dm_major==253 && !id->dm_minor);
        if (op==NZM_HELPER_BIND) { result->bound=1; put(Z "backing_dev","/dev/block/dm-0\n"); }
    }
    return 0;
}
int __system_property_get(const char *name, char *out) {
    owner(); CHECK(!strcmp(name,"sys.boot_completed"));
    f.completed_reads++;
    strcpy(out,f.boot_wait && !f.sleeps ? "0" : "1");
    return 1;
}
int __system_property_set(const char *name, const char *value) {
    (void)name; (void)value; CHECK(0); return -1;
}
int event_sleep(const struct timespec *delay, struct timespec *left) {
    owner(); (void)left;
    if (f.phase1) {
        CHECK(delay->tv_sec==5 && !delay->tv_nsec && f.boot_wait && !f.sleeps);
        CHECK(checkpoint("vm-complete-boot") && !checkpoint("vm-complete-completed"));
        f.sleeps++;
        put("/proc/sys/vm/min_free_kbytes","700\n");
        return 0;
    }
    CHECK(!f.scenario.event && delay->tv_sec==2);
    CHECK(f.execs+f.legacy_calls==1); errno=EINTR; return -1; /* synchronous loop bounded explicitly */
}
int event_control_check(const struct nzm_runtime_control *ctl, enum nzm_runtime_boundary b) {
    owner(); enum phase p=PH_NONE;
    if (f.in_execute) {
        if (b==NZM_BOUNDARY_OBSERVING && !f.idle_writes) p=PH_BEFORE;
        if (b==NZM_BOUNDARY_PRECOMMIT) p=PH_PRECOMMIT;
        if (b==NZM_BOUNDARY_POSTPRECHARGE) p=PH_POSTCHARGE;
        if (b==NZM_BOUNDARY_SUBMITTING && ++f.submitting_checks==2) p=PH_SUBMITTING;
    }
    if (p!=PH_NONE && p==f.scenario.stop_at) publish(p,1);
    return nzm_runtime_control_check(ctl,b);
}
int event_control_wait(const struct nzm_runtime_control *ctl, enum nzm_runtime_boundary b,
                       const struct timespec *deadline) {
    owner(); if (f.scenario.stop_at==PH_OBSERVING && f.in_execute) publish(PH_OBSERVING,0);
    return nzm_runtime_control_wait_until(ctl,b,deadline);
}
static int owner_init(void *unused, const struct nzm_runtime_control *ctl) {
    (void)unused; f.worker=pthread_self(); atomic_store(&f.ctl.initialized,1);
    return f.real_executor.initialize(f.real_executor.owner_context,ctl);
}
static void owner_execute(void *unused, const struct nzm_runtime_job *job,
                          const struct nzm_runtime_control *ctl, struct nzm_runtime_result *result) {
    (void)unused; owner(); f.in_execute=1;
    if (f.scenario.change_config && job->sequence==1) {
        /* Controlled disk edit by the sole I/O owner, not the producer. The
         * new 1 MiB budget/backend/runtime settings must NOT affect this run. */
        put("/vendor/etc/lge-zram.conf","runtime_event_enabled=0\nrequest_backend=legacy\ndaily_mib=1\n");
        publish(PH_CONFIG_CHANGED,1);
    }
    f.real_executor.execute(f.real_executor.owner_context,job,ctl,result);
    f.in_execute=0; unsigned jobs=atomic_fetch_add(&f.ctl.jobs,1)+1;
    if (f.scenario.policy_block) {
        CHECK(!f.execs && !f.legacy_calls && !f.idle_writes && ledger()==0);
        if (jobs==1) { publish(PH_FIRST_CYCLE,1); return; }
        CHECK(job->reasons&NZM_REASON_PSI);
    }
    if ((f.scenario.psi==PSI_MISSING || f.scenario.psi==PSI_DENIED) && jobs==1) return;
    if (f.scenario.stop_at==PH_CYCLE_DONE) publish(PH_CYCLE_DONE,1);
}
static void owner_finalize(void *unused, const struct nzm_runtime_control *ctl) {
    (void)unused; owner(); CHECK(!atomic_load(&f.ctl.request_live));
    f.real_executor.finalize(f.real_executor.owner_context,ctl);
    atomic_store(&f.ctl.finalized,1);
}
static int psi_open(void *unused, int flags) {
    (void)unused; CHECK(pthread_equal(pthread_self(),f.eventloop));
    CHECK(flags==(O_RDWR|O_NONBLOCK|O_CLOEXEC));
    if (f.scenario.psi==PSI_MISSING) return -ENOENT;
    if (f.scenario.psi==PSI_DENIED) return -EACCES;
    int pair[2]; CHECK(!socketpair(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0,pair));
    atomic_store(&f.ctl.psi_peer,pair[1]); return pair[0];
}
static ssize_t psi_write(void *unused, int fd, const void *buf, size_t len) {
    (void)unused; CHECK(fcntl(fd,F_GETFD)&FD_CLOEXEC);
    CHECK(len==strlen("some 500000 10000000\n") && !memcmp(buf,"some 500000 10000000\n",len));
    return (ssize_t)len;
}
static int psi_ack(void *unused, int fd) {
    (void)unused; char byte; CHECK(recv(fd,&byte,1,MSG_OOB|MSG_DONTWAIT)==1);
    atomic_fetch_add(&f.ctl.psi_acks,1);
    CHECK(!pthread_mutex_lock(&f.ctl.mutex)); CHECK(!pthread_cond_broadcast(&f.ctl.cond));
    CHECK(!pthread_mutex_unlock(&f.ctl.mutex)); return 0;
}
static int psi_close(void *unused, int fd) {
    (void)unused; CHECK(pthread_equal(pthread_self(),f.eventloop));
    CHECK(!close(fd)); atomic_fetch_add(&f.ctl.psi_closes,1); return 0;
}
int event_runtime_create(struct nzm_runtime **out, const struct nzm_runtime_options *options,
                         const struct nzm_runtime_executor *executor) {
    CHECK(!f.phase1 && pthread_equal(pthread_self(),f.eventloop));
    struct nzm_psi_adapter adapter={NULL,psi_open,psi_write,psi_ack,psi_close};
    struct nzm_runtime_options injected=*options; injected.psi_adapter=&adapter;
    f.real_executor=*executor;
    struct nzm_runtime_executor wrapped={NULL,owner_init,owner_execute,owner_finalize};
    int rc=nzm_runtime_create(out,&injected,&wrapped);
    if (!rc) {
        CHECK(!pthread_mutex_lock(&f.ctl.mutex)); f.ctl.runtime=*out;
        atomic_store(&f.ctl.active,1); CHECK(!pthread_cond_broadcast(&f.ctl.cond));
        CHECK(!pthread_mutex_unlock(&f.ctl.mutex));
    }
    return rc;
}
int event_runtime_stats(struct nzm_runtime *runtime, struct nzm_runtime_stats *out) {
    int rc=nzm_runtime_get_stats(runtime,out); if (!rc) f.stats=*out; return rc;
}
int event_runtime_destroy(struct nzm_runtime *runtime) {
    /* No producer may hold or call the runtime once it is freed. This does not
     * delay worker join/FD teardown: run has already proved those separately. */
    CHECK(!pthread_mutex_lock(&f.ctl.mutex));
    while (!atomic_load(&f.ctl.producer_done)) CHECK(!pthread_cond_wait(&f.ctl.cond,&f.ctl.mutex));
    CHECK(!pthread_mutex_unlock(&f.ctl.mutex));
    int rc=nzm_runtime_destroy(runtime); if (!rc) atomic_store(&f.ctl.active,0); return rc;
}
/* Link observers only: the real runtime source and the actual pthread/close
 * calls are unchanged. Catch premature join and runtime-FD close, not merely
 * a return value that happens to look successful. */
int __real_close(int);
int __wrap_close(int fd) {
    if (atomic_load(&f.ctl.active)) {
        char p[128],link[64]; snprintf(link,sizeof link,"/proc/self/fd/%d",fd);
        ssize_t n=readlink(link,p,sizeof p-1);
        if (n>=0) {
            p[n]=0;
            if (!strcmp(p,"anon_inode:[eventfd]") || !strcmp(p,"anon_inode:[timerfd]") ||
                !strcmp(p,"anon_inode:[signalfd]") || !strcmp(p,"anon_inode:[eventpoll]")) {
                CHECK(atomic_load(&f.ctl.finalized)); CHECK(atomic_load(&f.ctl.joins)==1);
                atomic_fetch_add(&f.ctl.runtime_closes,1);
            }
        }
    }
    return __real_close(fd);
}
ssize_t __real_read(int, void *, size_t);
ssize_t __wrap_read(int fd, void *buf, size_t size) {
    ssize_t n=__real_read(fd,buf,size);
    if (n>0 && atomic_load(&f.ctl.active) && pthread_equal(pthread_self(),f.eventloop)) {
        char p[128],link[64]; snprintf(link,sizeof link,"/proc/self/fd/%d",fd);
        ssize_t count=readlink(link,p,sizeof p-1);
        if (count>=0) {
            p[count]=0;
            if (!strcmp(p,"anon_inode:[signalfd]")) {
                CHECK(n%(ssize_t)sizeof(struct signalfd_siginfo)==0);
                struct signalfd_siginfo *info=buf;
                for (size_t i=0;i<(size_t)n/sizeof *info;i++)
                    if (info[i].ssi_signo==SIGHUP) atomic_fetch_add(&f.ctl.hup_reads,1);
            }
        }
    }
    return n;
}
int __real_pthread_join(pthread_t, void **);
int __wrap_pthread_join(pthread_t thread, void **result) {
    if (atomic_load(&f.ctl.active) && pthread_equal(pthread_self(),f.eventloop)) {
        CHECK(atomic_load(&f.ctl.finalized)); CHECK(!atomic_load(&f.ctl.request_live));
        atomic_fetch_add(&f.ctl.joins,1);
    }
    return __real_pthread_join(thread,result);
}
static void short_sleep(void) { struct timespec t={0,1000000}; CHECK(!nanosleep(&t,NULL)); }
static void wait_phase(enum phase phase) {
    struct timespec end; CHECK(!clock_gettime(CLOCK_REALTIME,&end)); end.tv_sec+=10;
    CHECK(!pthread_mutex_lock(&f.ctl.mutex));
    while (f.ctl.reached!=phase) CHECK(!pthread_cond_timedwait(&f.ctl.cond,&f.ctl.mutex,&end));
    CHECK(!pthread_mutex_unlock(&f.ctl.mutex));
}
static void release_worker(void) {
    CHECK(!pthread_mutex_lock(&f.ctl.mutex)); f.ctl.release=1;
    CHECK(!pthread_cond_broadcast(&f.ctl.cond)); CHECK(!pthread_mutex_unlock(&f.ctl.mutex));
}
static void wait_count(atomic_uint *value, unsigned minimum) {
    for (unsigned i=0;i<5000 && atomic_load(value)<minimum;i++) short_sleep();
    CHECK(atomic_load(value)>=minimum);
}
static void *producer(void *unused) {
    (void)unused; sigset_t mask; CHECK(!pthread_sigmask(SIG_SETMASK,NULL,&mask));
    CHECK(sigismember(&mask,SIGTERM)==1 && sigismember(&mask,SIGHUP)==1);
    if (f.scenario.change_config) {
        wait_phase(PH_CONFIG_CHANGED); CHECK(!pthread_kill(f.eventloop,SIGHUP));
        wait_count(&f.ctl.hup_reads,1); release_worker();
    }
    if (f.scenario.policy_block) {
        wait_phase(PH_FIRST_CYCLE);
        int peer=atomic_load(&f.ctl.psi_peer); CHECK(peer>=0);
        CHECK(send(peer,"P",1,MSG_OOB|MSG_NOSIGNAL)==1);
        wait_count(&f.ctl.psi_acks,1); release_worker();
    }
    wait_phase(f.scenario.stop_at);
    if (f.scenario.exec_error) {
        int peer=atomic_load(&f.ctl.psi_peer); CHECK(peer>=0);
        CHECK(send(peer,"E",1,MSG_OOB|MSG_NOSIGNAL)==1);
        CHECK(!pthread_mutex_lock(&f.ctl.mutex));
        atomic_store(&f.ctl.producer_done,1); f.ctl.release=1;
        CHECK(!pthread_cond_broadcast(&f.ctl.cond)); CHECK(!pthread_mutex_unlock(&f.ctl.mutex));
        return NULL;
    }
    if (f.scenario.retire_while_blocked) {
        CHECK(atomic_load(&f.ctl.request_live));
        struct nzm_spawn_fd_guard guard=NZM_SPAWN_FD_GUARD_INIT;
        CHECK(!nzm_spawn_fd_enter(&guard));
        int peer=atomic_exchange(&f.ctl.psi_peer,-1); CHECK(peer>=0); CHECK(!close(peer));
        nzm_spawn_fd_leave(&guard);
        wait_count(&f.ctl.psi_closes,1); /* Real EPOLLHUP -> shared gate -> retire. */
    }
    CHECK(!pthread_kill(f.eventloop,SIGTERM));
    int stopped=0;
    for (unsigned i=0;i<5000;i++) {
        int rc=nzm_runtime_notify(f.ctl.runtime,NZM_REASON_PSI);
        if (rc==-ESHUTDOWN) { stopped=1; break; }
        CHECK(!rc); short_sleep();
    }
    CHECK(stopped);
    if (f.scenario.stop_at==PH_EXEC) {
        CHECK(atomic_load(&f.ctl.request_live)); CHECK(!atomic_load(&f.ctl.finalized));
        CHECK(!atomic_load(&f.ctl.joins) && !atomic_load(&f.ctl.runtime_closes));
        struct nzm_runtime_stats s; CHECK(nzm_runtime_get_stats(f.ctl.runtime,&s)==-EBUSY);
    }
    CHECK(!pthread_mutex_lock(&f.ctl.mutex));
    atomic_store(&f.ctl.producer_done,1); f.ctl.release=1;
    CHECK(!pthread_cond_broadcast(&f.ctl.cond)); CHECK(!pthread_mutex_unlock(&f.ctl.mutex));
    return NULL;
}
static void setup(const struct scenario *s) {
    memset(&f,0,sizeof f); f.scenario=*s; f.eventloop=pthread_self();
    CHECK(!pthread_mutex_init(&f.ctl.mutex,NULL)); CHECK(!pthread_cond_init(&f.ctl.cond,NULL));
    atomic_init(&f.ctl.psi_peer,-1);
    strcpy(f.root,"/tmp/nzm-main-event-XXXXXX"); CHECK(mkdtemp(f.root));
    struct stat st; CHECK(!stat(f.root,&st)); f.storage_dev=st.st_dev;
    const char *dirs[]={"/dev","/dev/block","/dev/block/bootdevice","/dev/block/bootdevice/by-name",
        "/proc","/proc/self","/proc/sys","/proc/sys/kernel","/proc/sys/kernel/random","/proc/sys/vm",
        "/mnt","/mnt/vendor","/vendor","/vendor/etc","/sys","/sys/block","/sys/block/zram0",
        "/sys/class","/sys/class/block","/sys/class/block/sda26","/sys/class/block/sda5",
        "/sys/class/backlight","/sys/class/backlight/panel0-backlight",
        "/sys/class/power_supply","/sys/class/power_supply/battery"};
    for (size_t i=0;i<sizeof dirs/sizeof *dirs;i++) dir(dirs[i],0755);
    dir("/data",0771); dir("/data/vendor",0771); dir("/data/vendor/extm",0700);
    dir("/mnt/vendor/persist",0771); dir(WEAR,0700);
    put("/data/vendor/extm/owner","extm-dm-owner-v1\n");
    put(WEAR "/authority","extm-wear-v1\n"); put(WEAR "/ledger","wear-v1 0 0 0\n");
    put("/proc/sys/kernel/random/boot_id","01234567-89ab-cdef-0123-456789abcdef\n");
    const char *vm[]={"swappiness","page-cluster","watermark_scale_factor","extra_free_kbytes",
        "min_free_kbytes","overcommit_memory","laptop_mode"};
    for (size_t i=0;i<sizeof vm/sizeof *vm;i++) {
        char p[128]; snprintf(p,sizeof p,"/proc/sys/vm/%s",vm[i]); put(p,i==4?"100\n":"1\n");
    }
    put(Z "disksize","0\n"); put(Z "dev","0:1\n"); put(Z "backing_dev","none\n");
    put(Z "mm_stat","67108864 0 0 0 0 0 0 0 0 16384\n");
    put(Z "bd_stat","0 0 0\n"); put(Z "io_stat","0 0 0 0\n");
    put(Z "idle",""); put(Z "writeback",""); put(Z "writeback_limit","0\n");
    put(Z "writeback_limit_enable","0\n"); put("/dev/block/zram0","");
    put("/proc/swaps","Filename Type Size Used Priority\n");
    put("/proc/meminfo",s->policy_block ? "MemAvailable: 1 kB\n" : "MemAvailable: 524288 kB\n");
    put("/sys/class/backlight/panel0-backlight/bl_power","4\n");
    put("/sys/class/power_supply/battery/status","Charging\n");
    /* Real recovery mounted()/mount_record() parsing, synthetic kernel evidence. */
    put("/dev/block/sda26",""); put("/dev/block/sda5","");
    char p[1024],q[1024];
    path(p,sizeof p,"/dev/block/sda26"); path(q,sizeof q,"/dev/block/bootdevice/by-name/userdata"); CHECK(!link(p,q));
    path(p,sizeof p,"/dev/block/sda5"); path(q,sizeof q,"/dev/block/bootdevice/by-name/persist"); CHECK(!link(p,q));
    char text[2048]; snprintf(text,sizeof text,"%u:%u\n",major(f.storage_dev),minor(f.storage_dev));
    put("/sys/class/block/sda26/dev",text); put("/sys/class/block/sda5/dev",text);
    put("/sys/class/block/sda26/partition","26\n"); put("/sys/class/block/sda5/partition","5\n");
    put("/proc/self/vendor-fdinfo","mnt_id:\t32\n");
    put("/proc/self/data-fdinfo","mnt_id:\t32\n"); put("/proc/self/persist-fdinfo","mnt_id:\t33\n");
    snprintf(text,sizeof text,"32 1 %u:%u / /data rw - f2fs /dev/block/bootdevice/by-name/userdata rw\n"
        "33 1 %u:%u / /mnt/vendor/persist rw - ext4 /dev/block/bootdevice/by-name/persist rw\n",
        major(f.storage_dev),minor(f.storage_dev),major(f.storage_dev),minor(f.storage_dev));
    put("/proc/self/mountinfo",text);
    snprintf(text,sizeof text,"runtime_event_enabled=%d\npsi_enabled=%d\nrequest_backend=%s\n"
        "backing_mib=16\nidle_observation_seconds=0\nwatermark_charging_batch_mib=4\n"
        "watermark_charging_idle_observation_seconds=%d\nwatermark_battery_idle_observation_seconds=60\n"
        "watermark_charging_min_interval_seconds=60\nwatermark_battery_min_interval_seconds=120\n"
        "runtime_active_sample_seconds=1\nruntime_idle_sample_seconds=%d\nruntime_max_resync_seconds=%d\n",
        s->event,s->psi!=PSI_OFF,s->backend,s->stop_at==PH_OBSERVING?60:0,
        s->policy_block?10:1,s->policy_block?10:1);
    put("/vendor/etc/lge-zram.conf",text); path(p,sizeof p,"/vendor/etc/lge-zram.conf"); CHECK(!chmod(p,0644));
}
static int remove_one(const char *p, const struct stat *st, int type, struct FTW *walk) {
    (void)st; (void)walk; return type==FTW_DP ? rmdir(p) : unlink(p);
}
static void run_case(const struct scenario *s) {
    setup(s); sigset_t initial,original,blocked,after;
    FILE *log=tmpfile(); CHECK(log); fflush(stderr);
    int saved_stderr=dup(STDERR_FILENO); CHECK(saved_stderr>=0);
    CHECK(dup2(fileno(log),STDERR_FILENO)>=0);
    CHECK(!pthread_sigmask(SIG_SETMASK,NULL,&initial)); original=initial;
    CHECK(!sigaddset(&original,SIGUSR1)); CHECK(!sigdelset(&original,SIGTERM));
    CHECK(!sigdelset(&original,SIGINT)); CHECK(!sigdelset(&original,SIGHUP));
    blocked=original; CHECK(!sigaddset(&blocked,SIGTERM)); CHECK(!sigaddset(&blocked,SIGINT)); CHECK(!sigaddset(&blocked,SIGHUP));
    pthread_t thread;
    if (s->event) {
        CHECK(!pthread_sigmask(SIG_SETMASK,&blocked,NULL));
        CHECK(!pthread_create(&thread,NULL,producer,NULL));
    }
    CHECK(!pthread_sigmask(SIG_SETMASK,&original,NULL));
    /* Explicit legacy-v1 Stage 3 module regression, not production fallback. */
    struct nzm_core c;
    nzm_core_init(&c);
    CHECK(!nzm_core_config(&c));
    /* Explicit legacy module fixture, not values applied by live parsing. */
    CHECK(!c.config.runtime_event_enabled && !c.config.psi_enabled);
    c.config.runtime_event_enabled=s->event;
    c.config.psi_enabled=s->psi!=PSI_OFF;
    c.config.idle_observation_seconds=0;
    c.config.watermark_charging_batch_mib=4;
    c.config.watermark_charging_idle_observation_seconds=s->stop_at==PH_OBSERVING?60:0;
    c.config.watermark_battery_idle_observation_seconds=60;
    c.config.watermark_charging_min_interval_seconds=60;
    c.config.watermark_battery_min_interval_seconds=120;
    c.config.runtime_active_sample_seconds=1;
    c.config.runtime_idle_sample_seconds=s->policy_block?10:1;
    c.config.runtime_max_resync_seconds=s->policy_block?10:1;
    int code=c.config.runtime_event_enabled ? nzm_event_daemon(&c.config,0) : nzm_core_daemon(&c);
    nzm_core_close(&c);
    int rc=!!code;
    CHECK(!pthread_sigmask(SIG_SETMASK,NULL,&after));
    for (int sig=1;sig<NSIG;sig++) CHECK(sigismember(&original,sig)==sigismember(&after,sig));
    if (s->event) CHECK(!pthread_join(thread,NULL));
    fflush(stderr); CHECK(dup2(saved_stderr,STDERR_FILENO)>=0); CHECK(!close(saved_stderr));
    rewind(log); char line[2048];
    while(fgets(line,sizeof line,log)) {
        if (code) fputs(line,stderr);
        if(!strncmp(line,"stage=",6)) {
            CHECK(strlen(line)<sizeof f.last_event); strcpy(f.last_event,line);
        }
    }
    CHECK(!fclose(log));
    CHECK(rc==(s->exec_error || !s->event ? 1:0));
    CHECK(f.helpers[NZM_HELPER_PREPARE]==1 && f.helpers[NZM_HELPER_CREATE]==1 && f.helpers[NZM_HELPER_BIND]==1);
    CHECK(f.swaps==1 && number("/proc/sys/vm/min_free_kbytes")==200);
    CHECK(checkpoint("vm-complete-completed") && !exists(RUN));
    CHECK(f.config_opens==1);
    int early=s->stop_at==PH_BEFORE || s->stop_at==PH_OBSERVING || s->stop_at==PH_PRECOMMIT ||
        s->stop_at==PH_INTENT_SYNC || s->policy_block;
    int retained=s->stop_at==PH_POSTCHARGE || s->stop_at==PH_SUBMITTING;
    int legacy=!strcmp(s->backend,"legacy") || s->legacy_kernel;
    CHECK(ledger()==(early ? 0:CHARGE));
    CHECK(f.execs+f.legacy_calls==(unsigned)(!early && !retained));
    CHECK(f.execs==(unsigned)(!early && !retained && !legacy));
    CHECK(exists(WEAR "/exec.intent")==(!legacy && (retained || s->exec_error || s->stop_at==PH_INTENT_SYNC)));
    CHECK(exists(WEAR "/pause")==!!s->exec_error);
    CHECK(!f.execs || !f.quota_writes);
    if (s->event) {
        CHECK(f.stats.worker_joined && atomic_load(&f.ctl.joins)==1);
        CHECK(atomic_load(&f.ctl.runtime_closes)==4 && atomic_load(&f.ctl.finalized));
        if (s->psi==PSI_MISSING || s->psi==PSI_DENIED) {
            CHECK(f.stats.timer_only && f.stats.psi_fallbacks==1 && f.stats.timer_events>=1);
            CHECK(f.stats.psi_error==(s->psi==PSI_MISSING ? -ENOENT : -EACCES));
            CHECK(f.stats.jobs_completed==2 && f.stats.last_reasons&NZM_REASON_FALLBACK);
        }
        if (s->policy_block) {
            CHECK(f.stats.psi_events==1 && atomic_load(&f.ctl.psi_acks)==1);
            CHECK(f.stats.jobs_completed==2 && !f.stats.timer_events);
            CHECK(strstr(f.last_event,"stage=waiting_safety_or_policy "));
        }
        if (s->retire_while_blocked) CHECK(f.stats.psi_fallbacks==1 && f.stats.psi_error==-EIO);
        if (s->change_config) CHECK(f.stats.hup_unsupported==1 && atomic_load(&f.ctl.hup_reads)==1);
    }
    if (s->exec_error) {
        CHECK(strstr(f.last_event,"stage=request_unknown_no_replay_no_refund "));
        CHECK(strstr(f.last_event,"attempted=1 ") && strstr(f.last_event,"no_replay=1 "));
        CHECK(f.stats.jobs_completed==1 && f.stats.psi_events==1 && f.stats.coalesced>=1);
        CHECK(f.stats.last_result_code==-EFAULT && atomic_load(&f.ctl.psi_acks)==1);
    }
    if (s->stop_at==PH_SUBMITTING || s->stop_at==PH_POSTCHARGE)
        CHECK(strstr(f.last_event,"stage=stopped_retain_reservation ") && strstr(f.last_event,"attempted=0 "));
    int peer=atomic_load(&f.ctl.psi_peer); if (peer>=0) CHECK(!close(peer));
    printf("PASS main-event %s exit=%d exec=%u legacy=%u ledger=%" PRIu64 " jobs=%" PRIu64 "\n",
        s->name,rc,f.execs,f.legacy_calls,ledger(),f.stats.jobs_completed);
    CHECK(!pthread_sigmask(SIG_SETMASK,&initial,NULL));
    CHECK(!pthread_cond_destroy(&f.ctl.cond)); CHECK(!pthread_mutex_destroy(&f.ctl.mutex));
    CHECK(!nftw(f.root,remove_one,32,FTW_DEPTH|FTW_PHYS));
}
static void phase1_case(const char *name) {
    const struct scenario s={.backend="ioctl",.event=1,.psi=PSI_REAL_PRI};
    setup(&s); f.phase1=1;
    /* RAM-only fixtures disable physical-backend acquisition. Authority and
     * pending fences remain independent of this optional backend setting. */
    char cfgpath[1024]; path(cfgpath,sizeof cfgpath,"/vendor/etc/lge-zram.conf");
    FILE *cfg=fopen(cfgpath,"a"); CHECK(cfg);
    CHECK(fputs("enable_writeback=0\n",cfg)>=0 && !fclose(cfg));
    char p[1024]; path(p,sizeof p,"/data/vendor/extm/owner"); CHECK(!unlink(p));
    /* Data-only startup requires extm/state, not an owner or backing file.
     * Former persist authority is unrelated and must remain byte-for-byte. */
    dir("/mnt/vendor/persist/lge-zram-wear",0700);
    put("/mnt/vendor/persist/lge-zram-wear/authority","old persist untouched\n");
    const char *pause="native-v1 persistent pause\n";
    int boot_only=!strcmp(name,"boot-then-resume");
    int paused=boot_only || !strcmp(name,"paused-wait");
    if (paused) put(WEAR "/pause",pause);
    if (boot_only) put(WEAR "/exec.intent","preserve unexamined intent\n");
    if (!strcmp(name,"bad-pause")) put(WEAR "/pause","native-v1 persistent pause\nextra\n");
    if (!strcmp(name,"pause-symlink")) {
        path(p,sizeof p,WEAR "/pause"); CHECK(!symlink("authority",p));
    }
    if (!strcmp(name,"bad-authority")) {
        put(WEAR "/authority","foreign\n"); put(WEAR "/pause",pause);
    }
    const char *pending = !strcmp(name,"pending") ? "ledger.txn" :
        !strcmp(name,"pending-new") ? "ledger.new" :
        !strcmp(name,"pending-intent") ? "exec.intent" : NULL;
    if (pending) {
        char leaf[128]; snprintf(leaf,sizeof leaf,WEAR "/%s",pending);
        put(leaf,"preserve unresolved evidence\n");
    }
    if (!strcmp(name,"missing-ledger")) {
        path(p,sizeof p,WEAR "/ledger"); CHECK(!unlink(p));
    }
    if (!strcmp(name,"bad-ledger")) put(WEAR "/ledger","malformed\n");
    if (!strcmp(name,"exhausted-ledger"))
        put(WEAR "/ledger","wear-v1 1 0 18446744073709551615\n0 18446744073709551615\n");
    if (!strcmp(name,"bad-data-mount")) put("/proc/self/mountinfo","untrusted\n");
    if (!strcmp(name,"wrong-vendor-mount-id")) put("/proc/self/vendor-fdinfo","mnt_id:\t34\n");
    if (!strcmp(name,"backed")) put(Z "backing_dev","/dev/block/dm-0\n");
    if (!strcmp(name,"active-foreign"))
        put("/proc/swaps","Filename Type Size Used Priority\n/dev/block/zram0 partition 65532 0 -2\n");
    if (!strcmp(name,"old-marker")) {
        /* Malformed persistent startup state is never an ownership proof. */
        put(WEAR "/boot-state","old boot\n");
    }
    if (!strcmp(name,"wrong-device")) put(Z "dev","1:2\n");
    if (!strcmp(name,"existing-dm")) {
        dir("/sys/block/dm-0",0755); dir("/sys/block/dm-0/dm",0755);
        put("/sys/block/dm-0/dm/name","extm-file\n");
    }
    f.wrong_label=!strcmp(name,"wrong-label");
    f.boot_wait=!strcmp(name,"paused-wait");
    f.bind_during_init=!strcmp(name,"bind-race");
    f.fail_disksize=!strcmp(name,"attempt-not-replayed");
    int success=paused || pending || !strcmp(name,"ordinary") ||
        !strcmp(name,"missing-ledger") || !strcmp(name,"bad-ledger") ||
        !strcmp(name,"exhausted-ledger") || !strcmp(name,"no-runtime-directory");
    char old_ledger[128]; size_t old_ledger_len=0;
    path(p,sizeof p,WEAR "/ledger");
    int old_ledger_fd=open(p,O_RDONLY);
    if (old_ledger_fd>=0) {
        ssize_t n=read(old_ledger_fd,old_ledger,sizeof old_ledger); CHECK(n>0);
        old_ledger_len=(size_t)n; CHECK(!close(old_ledger_fd));
    } else CHECK(errno==ENOENT);
    char *boot[]={"native-zram-manager","boot","--confirmed",NULL};
    char *daemon[]={"native-zram-manager","daemon","--confirmed","--boot-completed",NULL};
    if (boot_only) {
        CHECK(!nzm_entry(3,boot));
        CHECK(f.swaps==1 && f.disksize_writes==1 && !f.completed_reads);
        CHECK(checkpoint("vm-complete-boot") && !checkpoint("vm-complete-completed"));
        put("/proc/sys/vm/min_free_kbytes","700\n");
    }
    CHECK(nzm_entry(f.boot_wait ? 3 : 4,daemon)==!success);
    if (f.fail_disksize) {
        CHECK(!number(Z "disksize"));
        f.fail_disksize=0;
        CHECK(nzm_entry(3,boot)==1); /* still cold, but the attempt fence wins */
        CHECK(f.disksize_writes==1 && checkpoint("ram-only-attempted"));
    }
    CHECK(f.swaps==(unsigned)success);
    if(strcmp(name,"old-marker")) {
        CHECK(checkpoint("ram-only")==success);
        CHECK(checkpoint("vm-complete-completed")==success);
    }
    CHECK(!exists(RUN));
    CHECK(number("/proc/sys/vm/min_free_kbytes")== (success ? 200 : 100));
    CHECK(f.completed_reads==(unsigned)(f.boot_wait ? 2 : 0));
    CHECK(!f.execs && !f.status_calls && !f.legacy_calls && !f.idle_writes && !f.quota_writes);
    CHECK(exists(WEAR) && !exists("/data/vendor/extm/file") &&
          !exists("/data/vendor/extm/owner") && !f.ctl.runtime);
    if (!success) {
        char zeros[16]; path(p,sizeof p,"/dev/block/zram0");
        int fd=open(p,O_RDONLY); CHECK(fd>=0 && read(fd,zeros,sizeof zeros)==0 && !close(fd));
    }
    if (!strcmp(name,"ordinary")) CHECK(ledger()==0 && !exists(WEAR "/pause"));
    if (boot_only) CHECK(exists(WEAR "/exec.intent"));
    if (pending) {
        char leaf[128]; snprintf(leaf,sizeof leaf,WEAR "/%s",pending);
        path(p,sizeof p,leaf); char text[64];
        int fd=open(p,O_RDONLY); CHECK(fd>=0);
        ssize_t n=read(fd,text,sizeof text-1); CHECK(n>0); text[n]=0;
        CHECK(!close(fd) && !strcmp(text,"preserve unresolved evidence\n"));
    }
    path(p,sizeof p,WEAR "/ledger");
    int fd=open(p,O_RDONLY);
    if (old_ledger_len) {
        char text[128]; CHECK(fd>=0);
        CHECK(read(fd,text,sizeof text)==(ssize_t)old_ledger_len);
        CHECK(!memcmp(text,old_ledger,old_ledger_len) && !close(fd));
    } else CHECK(fd<0 && errno==ENOENT);
    CHECK(!exists(WEAR "/ledger.v1") && !exists(WEAR "/ledger.bak"));
    path(p,sizeof p,"/mnt/vendor/persist/lge-zram-wear/authority");
    fd=open(p,O_RDONLY); CHECK(fd>=0);
    char untouched[64]; ssize_t oldn=read(fd,untouched,sizeof untouched);
    CHECK(oldn==(ssize_t)strlen("old persist untouched\n") &&
          !memcmp(untouched,"old persist untouched\n",(size_t)oldn) && !close(fd));
    CHECK(!pthread_cond_destroy(&f.ctl.cond)); CHECK(!pthread_mutex_destroy(&f.ctl.mutex));
    CHECK(!nftw(f.root,remove_one,32,FTW_DEPTH|FTW_PHYS));
    printf("PASS phase1 %s swaps=%u disksize_writes=%u\n",name,f.swaps,f.disksize_writes);
}
int main(int argc, char **argv) {
    if (argc>=2 && !strcmp(argv[1],"--phase1")) {
        const char *cases[]={"boot-then-resume","paused-wait","ordinary","bad-pause",
            "pause-symlink","bad-authority","pending","pending-new","pending-intent",
            "missing-ledger","bad-ledger","exhausted-ledger","bad-data-mount","wrong-vendor-mount-id","backed",
            "active-foreign","old-marker","wrong-device","wrong-label","existing-dm","no-runtime-directory",
            "bind-race","attempt-not-replayed"};
        for (size_t i=0;i<sizeof cases/sizeof *cases;i++) phase1_case(cases[i]);
        return 0;
    }
    const struct scenario cases[]={
        {.name="cold-boot-ioctl-stop0",.backend="ioctl",.event=1,.stop_at=PH_CYCLE_DONE},
        {.name="cold-boot-explicit-legacy",.backend="legacy",.event=1,.stop_at=PH_CYCLE_DONE},
        {.name="cold-boot-auto-enotty-legacy",.backend="auto",.event=1,.legacy_kernel=1,.stop_at=PH_CYCLE_DONE},
        {.name="runtime-off-ioctl",.backend="ioctl",.stop_at=PH_NONE},
        {.name="runtime-off-legacy",.backend="legacy",.stop_at=PH_NONE},
        {.name="psi-missing-timer",.backend="ioctl",.event=1,.psi=PSI_MISSING,.stop_at=PH_CYCLE_DONE},
        {.name="psi-denied-timer",.backend="ioctl",.event=1,.psi=PSI_DENIED,.stop_at=PH_CYCLE_DONE},
        {.name="stop-before-observation",.backend="ioctl",.event=1,.stop_at=PH_BEFORE},
        {.name="stop-during-observation",.backend="ioctl",.event=1,.stop_at=PH_OBSERVING},
        {.name="stop-precommit",.backend="ioctl",.event=1,.stop_at=PH_PRECOMMIT},
        {.name="stop-intent-fsync-before-ledger",.backend="ioctl",.event=1,.stop_at=PH_INTENT_SYNC},
        {.name="stop-postprecharge",.backend="ioctl",.event=1,.stop_at=PH_POSTCHARGE},
        {.name="stop-actual-submitting",.backend="ioctl",.event=1,.stop_at=PH_SUBMITTING},
        {.name="blocked-exec-psi-retire-stop",.backend="ioctl",.event=1,.stop_at=PH_EXEC,.psi=PSI_REAL_PRI,.retire_while_blocked=1},
        {.name="efault-once-no-replay",.backend="ioctl",.event=1,.exec_error=EFAULT,.stop_at=PH_EXEC,.psi=PSI_REAL_PRI},
        {.name="real-pri-keeps-safety-policy",.backend="ioctl",.event=1,.stop_at=PH_CYCLE_DONE,.psi=PSI_REAL_PRI,.policy_block=1},
        {.name="hup-disk-config-not-reloaded",.backend="ioctl",.event=1,.stop_at=PH_CYCLE_DONE,.change_config=1},
    };
    unsigned ran=0;
    for (size_t i=0;i<sizeof cases/sizeof *cases;i++) {
        if (argc==2 && strcmp(argv[1],cases[i].name)) continue;
        fflush(NULL); pid_t pid=fork(); CHECK(pid>=0);
        if (!pid) { alarm(20); run_case(&cases[i]); fflush(NULL); _exit(0); }
        int status; CHECK(waitpid(pid,&status,0)==pid);
        if (!WIFEXITED(status) || WEXITSTATUS(status)) {
            fprintf(stderr,"FAIL main-event %s status=%d\n",cases[i].name,status); return 1;
        }
        ran++;
    }
    CHECK(ran>0); printf("main-event: %u independent real-main cases PASS; injected helper/kernel boundaries, no device execution\n",ran);
    return 0;
}
