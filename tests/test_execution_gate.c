/* Dedicated threaded adapter fixture. NEVER run the serial core fixture globals
 * concurrently. Real shared spawn_fd.c + frozen rb; syscall-only substitution. */
#define _GNU_SOURCE
#include "nzm_internal.h"
#include "uapi/linux/zram.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/sysmacros.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

static _Thread_local int gate_depth;
static int tracked_enter(struct nzm_spawn_fd_guard *guard) {
  assert(!gate_depth);
  int rc=nzm_spawn_fd_enter(guard);
  if(!rc)gate_depth=1;
  return rc;
}
static void tracked_leave(struct nzm_spawn_fd_guard *guard) {
  assert(gate_depth==1);gate_depth=0;nzm_spawn_fd_leave(guard);
  errno=EUCLEAN; /* pthread cleanup must not overwrite syscall error reporting. */
}
struct fixture {
  struct nzm_core core;
  struct nzm_plan plan;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  int entered, release, retired, stopped, completed;
  int fail_open, fail_exec, fail_close, fail_label;
  unsigned opens, closes, statuses, execs;
  int rc;
};
static struct fixture *current; /* Published before threads; immutable pointer. */
static int gated_open(const char *path, int flags, ...) {
  struct fixture *f=current;
  assert(gate_depth==1 && !strcmp(path,RB_DEVICE_PATH));
  assert((flags&(O_CLOEXEC|O_NOFOLLOW))==(O_CLOEXEC|O_NOFOLLOW));
  f->opens++;
  if(f->fail_open) {errno=EACCES;return -1;}
  return memfd_create("request-gate-fixture",MFD_CLOEXEC);
}
static int ungated_stat(int fd, struct stat *st) {
  assert(!gate_depth && fcntl(fd,F_GETFD)>=0);
  memset(st,0,sizeof *st);st->st_mode=S_IFBLK|0600;st->st_rdev=makedev(254,3);
  return 0;
}
static ssize_t ungated_label(int fd, const char *name, void *value, size_t size) {
  assert(!gate_depth && fcntl(fd,F_GETFD)>=0 && !strcmp(name,"security.selinux"));
  if(current->fail_label) {errno=ENODATA;return -1;}
  assert(size>=sizeof RB_DEVICE_LABEL);memcpy(value,RB_DEVICE_LABEL,sizeof RB_DEVICE_LABEL);
  return sizeof RB_DEVICE_LABEL;
}
static int gated_close(int fd) {
  assert(gate_depth==1);current->closes++;
  int rc=close(fd);assert(!rc);
  if(current->fail_close) {errno=EINTR;return -1;}
  return 0;
}
static int ungated_ioctl(int fd, unsigned long command, void *arg) {
  struct fixture *f=current;
  assert(!gate_depth && fcntl(fd,F_GETFD)>=0);
  if(command==ZRAM_IOC_WB_STATUS) {
    f->statuses++;
    struct zram_wb_status *s=arg;
    s->capabilities=ZRAM_WB_CAP_EXEC|ZRAM_WB_CAP_STATUS|ZRAM_WB_CAP_FILTERED_IDLE|ZRAM_WB_CAP_DUAL_LIMIT;
    s->instance=11;s->generation=7;s->page_size=4096;s->disksize_bytes=67108864;
    s->state_flags=ZRAM_WB_STATE_INITIALIZED|ZRAM_WB_STATE_BACKING;
    return 0;
  }
  assert(command==ZRAM_IOC_WB_EXEC);f->execs++;
  assert(!pthread_mutex_lock(&f->mutex));
  f->entered=1;assert(!pthread_cond_broadcast(&f->cond));
  while(!f->release)assert(!pthread_cond_wait(&f->cond,&f->mutex));
  assert(!pthread_mutex_unlock(&f->mutex));
  if(f->fail_exec) {errno=EFAULT;return -1;}
  struct zram_wb_exec *r=arg;
  r->scanned_pages=r->selected_pages=r->submitted_pages=r->installed_pages=1;
  r->stop_reason=ZRAM_WB_STOP_INSTALLED_LIMIT;
  return 0;
}
#define open gated_open
#define close gated_close
#define fstat ungated_stat
#define fgetxattr ungated_label
#define ioctl ungated_ioctl
#define nzm_spawn_fd_enter tracked_enter
#define nzm_spawn_fd_leave tracked_leave
#include "../src/execution.c"
#undef nzm_spawn_fd_leave
#undef nzm_spawn_fd_enter
#undef ioctl
#undef fgetxattr
#undef fstat
#undef close
#undef open

/* No filesystem effects in this focused test. Whole-core main integration is a
 * separate suite. Every core access below belongs to the worker until join. */
int nzm_rawread(const char *path, char *text, size_t cap) {
  assert(!strcmp(path,Z "dev") && cap>=7);strcpy(text,"254:3\n");return 0;
}
int nzm_number(const char *path, uint64_t *value) {
  assert(!strcmp(path,Z "disksize"));*value=67108864;return 0;
}
int nzm_fields(const char *path, uint64_t *values, size_t count) {
  (void)path;(void)values;(void)count;abort();
}
int nzm_monotonic_now(uint64_t *tick) {assert(!gate_depth);*tick=100;return 0;}
int nzm_boundary(struct nzm_core *c) {(void)c;assert(!gate_depth);return 0;}
int nzm_core_control(struct nzm_core *c,enum nzm_runtime_boundary boundary) {(void)c;(void)boundary;return 0;}
int nzm_intent_retire(struct nzm_core *c) {(void)c;return 0;}
int nzm_stop(struct nzm_core *c, const char *stage, int rc) {
  c->stage=stage;c->execution.halted=1;return rc;
}
int nzm_diag(struct nzm_core *c, const char *stage, int rc) {c->stage=stage;return rc;}
static void *worker(void *arg) {
  struct fixture *f=arg;
  nzm_config_default(&f->core.config);
  f->core.execution.next_request_id=1; /* Fixture bypasses nzm_core_init(). */
  f->rc=nzm_execution_select(&f->core);
  if(!f->rc) {
    f->core.reserved=f->core.intent_owned=1;
    f->rc=nzm_execution_execute(&f->core,&f->plan);
  }
  assert(!pthread_mutex_lock(&f->mutex));
  f->completed=1;assert(!pthread_cond_broadcast(&f->cond));
  assert(!pthread_mutex_unlock(&f->mutex));return NULL;
}
static void *eventloop(void *arg) {
  struct fixture *f=arg;
  struct nzm_spawn_fd_guard guard=NZM_SPAWN_FD_GUARD_INIT;
  assert(!nzm_spawn_fd_enter(&guard));
  int ep=epoll_create1(EPOLL_CLOEXEC),stop=eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK);
  int psi=eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK);
  assert(ep>=0 && stop>=0 && psi>=0);
  struct epoll_event event={.events=EPOLLIN,.data.fd=stop};
  assert(!epoll_ctl(ep,EPOLL_CTL_ADD,stop,&event));
  nzm_spawn_fd_leave(&guard);
  assert(!pthread_mutex_lock(&f->mutex));
  while(!f->entered)assert(!pthread_cond_wait(&f->cond,&f->mutex));
  assert(!pthread_mutex_unlock(&f->mutex));
  /* Model PSI failure retirement BEFORE processing stop. This must not wait
   * for the unrelated, worker-owned EXEC to finish. Never close its fd. */
  assert(!nzm_spawn_fd_enter(&guard));assert(!close(psi));
  int replacement=eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK);assert(replacement>=0);
  assert(!close(replacement));nzm_spawn_fd_leave(&guard);
  uint64_t one=1,value=0;
  assert(write(stop,&one,sizeof one)==sizeof one);
  assert(epoll_wait(ep,&event,1,1000)==1 && event.data.fd==stop);
  assert(read(stop,&value,sizeof value)==sizeof value && value==1);
  assert(!pthread_mutex_lock(&f->mutex));
  assert(f->entered && !f->release && !f->completed);
  f->retired=f->stopped=1;assert(!pthread_cond_broadcast(&f->cond));
  /* Responsive stop is NOT completed teardown. Worker can still be blocked. */
  while(!f->completed)assert(!pthread_cond_wait(&f->cond,&f->mutex));
  assert(!pthread_mutex_unlock(&f->mutex));
  assert(!nzm_spawn_fd_enter(&guard));assert(!close(stop) && !close(ep));
  nzm_spawn_fd_leave(&guard);return NULL;
}
static void test_blocked(int copyout_fault) {
  struct fixture f={.plan={.pages=1,.reserve_kib=4,.limit_4k=1},.fail_exec=copyout_fault};
  assert(!pthread_mutex_init(&f.mutex,NULL));assert(!pthread_cond_init(&f.cond,NULL));
  current=&f;pthread_t w,e;
  assert(!pthread_create(&e,NULL,eventloop,&f));assert(!pthread_create(&w,NULL,worker,&f));
  assert(!pthread_mutex_lock(&f.mutex));
  struct timespec deadline;assert(!clock_gettime(CLOCK_REALTIME,&deadline));deadline.tv_sec+=5;
  while(!f.stopped)assert(!pthread_cond_timedwait(&f.cond,&f.mutex,&deadline));
  assert(f.retired && !f.completed && !f.release);
  f.release=1;assert(!pthread_cond_broadcast(&f.cond));
  assert(!pthread_mutex_unlock(&f.mutex));
  assert(!pthread_join(w,NULL) && !pthread_join(e,NULL));
  assert(f.statuses==1 && f.execs==1 && f.opens==2 && f.closes==2);
  assert(f.core.reserved && f.core.attempted && f.core.execution.result.no_replay);
  assert(f.rc==(copyout_fault?-EFAULT:0));
  if(copyout_fault)assert(f.core.execution.halted && !f.core.execution.result.known);
  else assert(f.core.execution.result.known);
  assert(!pthread_cond_destroy(&f.cond) && !pthread_mutex_destroy(&f.mutex));
}
static void test_errno(void) {
  for(int failure=0;failure<3;failure++) {
    struct fixture f={0};current=&f;nzm_config_default(&f.core.config);
    f.fail_open=failure==0;f.fail_label=failure==1;f.fail_close=failure==2;
    int rc=nzm_execution_select(&f.core);
    assert(rc==-(failure==0?EACCES:failure==1?ENODATA:EINTR));
    assert(f.opens==1 && f.closes==(unsigned)(failure!=0));
    assert(!f.execs && f.statuses==(unsigned)(failure==2));
    assert(!gate_depth); /* All rejection/close paths release the shared gate. */
  }
}
int main(void) {
  test_errno();test_blocked(0);test_blocked(1);
  puts("PASS execution FD gate: open/close only, errno retained, status/stat/xattr/EXEC ungated, PSI retirement + stop responsive while EXEC blocked, no early worker teardown");
  return 0;
}
