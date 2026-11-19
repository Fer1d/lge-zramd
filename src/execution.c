#define _GNU_SOURCE
#include "nzm_internal.h"
#include "uapi/linux/zram.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <sys/xattr.h>
#include <unistd.h>
#ifdef NZM_CORE_ADAPTER
#include NZM_CORE_ADAPTER
#endif

/* The adapter owns policy/state. The frozen rb module only owns one synchronous
 * open/validate/ioctl/close. No retrying EXEC, cached fd, or kernel include tree.
 * The worker is the sole helper/fork owner and executes requests serially. Its
 * exclusively owned fd stays stable during fstat/xattr/ioctl. Gate ONLY fd table
 * mutations: holding the shared gate across blocking writeback would stall the
 * eventloop's PSI retirement/FD management and delay stop handling. */
static int request_open(void *context, const char *path, int flags) {
  (void)context;
  struct nzm_spawn_fd_guard guard=NZM_SPAWN_FD_GUARD_INIT;
  int rc=nzm_spawn_fd_enter(&guard);
  if(rc) {errno=-rc;return -1;}
  int fd=open(path,flags),saved_errno=errno;
  nzm_spawn_fd_leave(&guard);
  errno=saved_errno;
  return fd;
}
static int request_stat(void *context, int fd, struct stat *st) {
  (void)context;
  return fstat(fd,st);
}
static ssize_t request_label(void *context, int fd, const char *name,
                             void *value, size_t size) {
  (void)context;
  return fgetxattr(fd,name,value,size);
}
static int request_ioctl(void *context, int fd, unsigned long command, void *arg) {
  struct nzm_core *c=context;
  if(command==ZRAM_IOC_WB_EXEC) {
    uint64_t tick;
    int rc=nzm_core_control(c,NZM_BOUNDARY_SUBMITTING);
    if(rc) {errno=-rc;return -1;}
    rc=nzm_monotonic_now(&tick);
    if(rc) {errno=-rc;return -1;}
    /* Actual syscall boundary, not the earlier open or idle observation. */
    c->timer_since=c->last_attempt_tick=tick;
    c->timer_valid=c->last_attempt_valid=c->attempted=1;
  }
#ifdef __BIONIC__
  return ioctl(fd,(int)command,arg);
#else
  return ioctl(fd,command,arg);
#endif
}
static int request_close(void *context, int fd) {
  (void)context;
  struct nzm_spawn_fd_guard guard=NZM_SPAWN_FD_GUARD_INIT;
  int rc=nzm_spawn_fd_enter(&guard);
  if(rc) {errno=-rc;return -1;}
  rc=close(fd);
  int saved_errno=errno;
  nzm_spawn_fd_leave(&guard);
  errno=saved_errno;
  return rc; /* Linux close, including EINTR, is never retried. */
}
static const struct rb_syscalls request_syscalls={
  request_open,request_stat,request_label,request_ioctl,request_close
};

const char *nzm_execution_name(const struct nzm_core *c) {
  if(!c->execution.selected)return "unselected";
  switch(c->execution.backend) {
  case RB_BACKEND_REQUEST:return "ioctl";
  case RB_BACKEND_LEGACY:return "legacy";
  default:return "unavailable";
  }
}
static int actual_rdev(dev_t *out) {
  char text[64],canonical[64];
  unsigned ma,mi;int n=0;
  int rc=nzm_rawread(Z "dev",text,sizeof text);
  if(rc)return rc;
  if(sscanf(text,"%u:%u%n",&ma,&mi,&n)!=2 || strcmp(text+n,"\n") ||
     ma>4095 || mi>1048575)return -EINVAL;
  snprintf(canonical,sizeof canonical,"%u:%u\n",ma,mi);
  if(strcmp(text,canonical))return -EINVAL;
  *out=makedev(ma,mi);
  return *out ? 0 : -ENODEV;
}
static int status_once(struct nzm_core *c) {
  c->execution.status=rb_status(&c->execution.transport);
  const struct rb_status_result *s=&c->execution.status;
  if(s->transport.close_errno)return -s->transport.close_errno;
  if(!s->known)return -(s->transport.error_no ? s->transport.error_no : EPROTO);
  if(s->selection!=RB_BACKEND_REQUEST)return -ENOTSUP;
  return 0;
}
int nzm_execution_select(struct nzm_core *c) {
  struct nzm_execution *e=&c->execution;
  if(e->halted)return -EPERM;
  if(e->selected)return e->backend==RB_BACKEND_UNAVAILABLE ? -EPERM : 0;
  e->selected=1; /* A failed selection is never retried or reinterpreted. */
  if(c->config.request_backend==NZM_REQUEST_LEGACY) {
    e->backend=RB_BACKEND_LEGACY;
  } else {
    if(c->config.request_backend!=NZM_REQUEST_AUTO &&
       c->config.request_backend!=NZM_REQUEST_IOCTL)return -EINVAL;
    int rc=actual_rdev(&e->transport.expected_rdev);
    if(rc)return rc;
    e->transport.syscalls=&request_syscalls;
    e->transport.syscall_context=c;
    rc=status_once(c);
    const struct rb_status_result *s=&e->status;
    if(rc==-ENOTTY && !s->transport.close_errno &&
       s->selection==RB_BACKEND_LEGACY && c->config.request_backend==NZM_REQUEST_AUTO) {
      e->backend=RB_BACKEND_LEGACY;
    } else {
      if(rc)return rc;
      long page=sysconf(_SC_PAGESIZE);
      uint64_t disk;
      if(page!=4096 && page!=16384 && page!=65536)return -ENOTSUP;
      if(!s->ready)return -ENODEV;
      if(s->page_size!=(uint32_t)page)return -EPROTO;
      if((rc=nzm_number(Z "disksize",&disk)))return rc;
      if(!disk || disk%((uint64_t)page) || s->disksize_bytes!=disk)return -ESTALE;
      e->identity=s->identity;
      e->page_size=s->page_size;
      e->disksize_bytes=disk;
      /* Request numbering belongs to the controller object's lifetime. */
      e->backend=RB_BACKEND_REQUEST;
    }
  }
  fprintf(stderr,"request_backend=%s enabled_backend=%s selection_scope=boot_once\n",
      nzm_request_backend_name(c->config.request_backend),nzm_execution_name(c));
  return 0;
}
static int status_bound(struct nzm_core *c) {
  struct nzm_execution *e=&c->execution;
  dev_t rdev;
  memset(&e->status,0,sizeof e->status);
  int rc=actual_rdev(&rdev);
  if(rc)return rc;
  if(rdev!=e->transport.expected_rdev)return -ESTALE;
  rc=status_once(c);
  if(rc)return rc; /* Permission/node/label/bad reply is NOT sample noise. */
  const struct rb_status_result *s=&e->status;
  if(s->identity.instance!=e->identity.instance ||
     s->identity.generation!=e->identity.generation ||
     s->disksize_bytes!=e->disksize_bytes || s->page_size!=e->page_size)return -ESTALE;
  if(!s->ready)return -ENODEV;
  return 0;
}
/* Legacy-only paired-kernel contract: ten fields, with lowratio_pages last.
 * Field count is NOT proof of that semantic; an unrelated ten-field ABI is
 * unsupported even though it cannot be distinguished from this text alone.
 * Do not infer layouts or use this parser for demand STATUS/pool statistics. */
static int legacy_mm_stat(uint64_t mm[10]) {
  char b[2048], *s, *end;
  uint64_t fields[10];
  size_t count=0;
  int rc=nzm_rawread(Z "mm_stat",b,sizeof b);
  if(rc)return rc;
  s=b;
  for(;;) {
    while(*s==' ' || *s=='\t')s++;
    if(!*s || *s=='\n')break;
    end=s;
    while(*end>='0' && *end<='9')end++;
    if(end==s || (*end && *end!=' ' && *end!='\t' && *end!='\n'))return -EINVAL;
    char ch=*end;
    *end=0;
    uint64_t value;
    rc=nzm_u64(s,&value);
    *end=ch;
    if(rc)return rc; /* Validate overflow even in unsupported extra fields. */
    if(count<10)fields[count]=value;
    count++;
    s=end;
  }
  while(*s==' ' || *s=='\t' || *s=='\n')s++;
  if(*s || !count)return -EINVAL;
  if(count!=10) {
    fprintf(stderr,"legacy mm_stat unsupported fields=%zu: requires paired-kernel ten-field lowratio_pages contract; no layout inference or fallback\n",count);
    return -EOPNOTSUPP;
  }
  memcpy(mm,fields,sizeof fields);
  return 0;
}
int nzm_execution_sample(struct nzm_core *c, uint64_t mm[10], uint64_t bd[3],
                         uint64_t *disk, uint64_t *page) {
  c->execution.sample_inconsistent=0;
  if(c->execution.backend!=RB_BACKEND_REQUEST) {
    int rc;
    if((rc=legacy_mm_stat(mm)) || (rc=nzm_fields(Z "bd_stat",bd,3)) ||
       (rc=nzm_number(Z "disksize",disk)))return rc;
    *page=(uint64_t)sysconf(_SC_PAGESIZE);
    return 0;
  }
  /* STATUS statistics are individually sampled, never coherent. Retry only
   * inconsistent numeric samples, at most three readonly calls. Lifecycle
   * changes are confirmed read-only, NEVER adopted or cleared by later samples.
   * Validation failures from open/fstat/label/transport are returned verbatim. */
  int lifecycle_error=0;
  for(unsigned attempt=0;attempt<3;attempt++) {
    int rc=status_bound(c);
    if(rc) {
      if(c->execution.status.known &&
         c->execution.status.transport.failure_stage==RB_FAILURE_NONE &&
         (rc==-ESTALE || rc==-ENODEV)) {
        if(!lifecycle_error)lifecycle_error=rc;
        continue;
      }
      return rc;
    }
    if(lifecycle_error)continue;
    const struct rb_sampled_statistics *s=&c->execution.status.sampled;
    *page=c->execution.page_size;*disk=c->execution.disksize_bytes;
    uint64_t units=*page/4096;
    if(s->pages_stored>UINT64_MAX/(*page) || s->bd_count_pages>UINT64_MAX/units ||
       s->bd_reads_pages>UINT64_MAX/units || s->bd_writes_pages>UINT64_MAX/units)continue;
    memset(mm,0,10*sizeof *mm);
    mm[0]=s->pages_stored*(*page);mm[1]=s->compr_data_bytes;mm[2]=s->mem_used_bytes;
    mm[5]=s->same_pages;mm[8]=s->huge_pages;mm[9]=s->lowratio_pages;
    bd[0]=s->bd_count_pages*units;bd[1]=s->bd_reads_pages*units;bd[2]=s->bd_writes_pages*units;
    struct nzm_plan probe;
    enum nzm_watermark_state state=NZM_ARMED;
    rc=nzm_plan_writeback(&c->config,*page,*disk,mm,bd,1,&state,&probe);
    if(!rc || rc==-EAGAIN)return 0;
    if(rc!=-EINVAL && rc!=-ERANGE)return rc;
  }
  if(lifecycle_error)return lifecycle_error;
  /* Individually sampled numeric fields can remain contradictory across three
   * reads. No wire/metadata/identity error reaches here: wait readonly this
   * cycle, without intent, reservation or persistent pause. */
  c->execution.sample_inconsistent=1;
  return -EAGAIN;
}
int nzm_execution_precheck(struct nzm_core *c, const struct nzm_plan *plan) {
  if(c->execution.halted || !c->execution.selected)return -EPERM;
  if(c->execution.backend==RB_BACKEND_LEGACY)return 0;
  if(c->execution.backend!=RB_BACKEND_REQUEST)return -EPERM;
  if(!c->execution.next_request_id)return -EOVERFLOW;
  int rc=status_bound(c);
  if(rc)return rc;
  uint64_t page=c->execution.page_size;
  if(!plan->pages || plan->pages>UINT64_MAX/(page/1024) ||
     plan->reserve_kib!=plan->pages*(page/1024) ||
     plan->limit_4k!=plan->pages*(page/4096))return -EINVAL;
  const struct rb_status_result *s=&c->execution.status;
  if((s->state_flags&ZRAM_WB_STATE_GLOBAL_LIMIT) &&
     s->sampled.wb_limit_remaining_4k<plan->limit_4k)return -EAGAIN;
  return 0;
}
int nzm_execution_execute(struct nzm_core *c, const struct nzm_plan *plan) {
  struct nzm_execution *e=&c->execution;
  if(e->halted || e->backend!=RB_BACKEND_REQUEST || !c->reserved || !c->intent_owned ||
     c->attempted || e->result.no_replay || !e->next_request_id)
    return nzm_stop(c,"request_execution_contract",-EPERM);
  int rc=nzm_boundary(c);
  if(rc)return nzm_stop(c,"pre_attempt_boundary",rc);
  if((rc=nzm_core_control(c,NZM_BOUNDARY_SUBMITTING)))return rc;
  struct rb_exec_request request={
    .expected=e->identity,.request_id=e->next_request_id,
    .max_attempt_pages=plan->pages,.max_installed_pages=plan->pages,
    .filtered_idle_count=1
  };
  e->next_request_id=e->next_request_id==UINT64_MAX ? 0 : e->next_request_id+1;
  e->result=rb_exec(&e->transport,&request); /* Exactly ONE call; no gate over I/O. */
  const struct rb_exec_result *r=&e->result;
  /* A stop observed in our ioctl wrapper before actual entry is not a kernel
   * failure. The frozen transport is conservative about callback entry; keep
   * its no_replay flag and intent, but do not claim an actual attempted EXEC. */
  if(c->cooperative_stop && !c->attempted && r->transport.error_no==ECANCELED)
    return -ECANCELED;
  if(!r->known)return nzm_stop(c,r->no_replay ? "request_unknown_no_replay_no_refund" :
      "request_not_called_retained_no_refund",-(r->transport.error_no ? r->transport.error_no : EPROTO));
  c->no_progress=r->counters.installed_pages==0;
  c->watermark_resume=c->watermark;
  if(c->no_progress) {
    c->blocked_plan=*plan;c->no_progress_since=c->last_attempt_tick;
    c->watermark=NZM_NO_PROGRESS;
  }
  /* Known negative errno is not transport failure and may include work. All
   * such results conservatively pause/retain. Never replay or refund any part. */
  if(r->op_errno)return nzm_stop(c,r->counters.submitted_pages || r->counters.installed_pages ?
      "request_known_partial_no_refund" : "request_known_error_no_refund",r->op_errno);
  if(r->transport.close_errno)return nzm_stop(c,"request_known_close_error_no_refund",-r->transport.close_errno);
  if((rc=nzm_boundary(c)))return nzm_stop(c,"post_attempt_identity",rc);
  if((rc=nzm_intent_retire(c)))return nzm_stop(c,"intent_retire_uncertain",rc);
  return nzm_diag(c,c->no_progress ? "waiting_request_no_progress_process_only" :
      "request_installed_no_refund",0);
}
