#define _GNU_SOURCE
#include "nzm_internal.h"
#include "demand_exec.h"
#include "log.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <sys/xattr.h>
#include <unistd.h>

_Static_assert(sizeof(struct zram_wb_exec)==160,"exec ABI");
_Static_assert(sizeof(struct nzm_demand_exec_wire)==224,"policy ABI");
_Static_assert(offsetof(struct nzm_demand_exec_wire,demand_sequence)==160,"policy offset");

static int real_guard(void *p, struct nzm_core *c) {
  (void)p;
  int paused=0,rc=nzm_core_control(c,NZM_BOUNDARY_SUBMITTING);
  if(!rc)rc=nzm_pause_verified(c,&paused);
  if(!rc && paused)rc=-EPERM;
  if(!rc)rc=nzm_managed(c);
  if(!rc)rc=nzm_absent(c->wearfd,"ledger.new");
  if(!rc)rc=nzm_absent(c->wearfd,"ledger.txn");
  if(!rc)rc=nzm_intent_check(c);
  return rc;
}
static int real_status(void *p, struct nzm_core *c, struct rb_status_result *s) {
  (void)p;
  char b[64],canonical[64];unsigned ma,mi;int n=0;
  int rc=nzm_rawread(Z "dev",b,sizeof b);
  if(rc)return rc;
  if(sscanf(b,"%u:%u%n",&ma,&mi,&n)!=2 || strcmp(b+n,"\n") || ma>4095 || mi>1048575)return -EPROTO;
  snprintf(canonical,sizeof canonical,"%u:%u\n",ma,mi);
  if(strcmp(b,canonical) || !makedev(ma,mi))return -EPROTO;
  if(c->execution.transport.expected_rdev && c->execution.transport.expected_rdev!=makedev(ma,mi))return -ESTALE;
  c->execution.transport.expected_rdev=makedev(ma,mi);
  *s=rb_status(&c->execution.transport);
  if(s->transport.close_errno)return -s->transport.close_errno;
  return s->known && s->selection==RB_BACKEND_REQUEST ? 0 : -EPROTO;
}
static int real_demand(void *p, struct nzm_demand_view *v) {
  (void)p;
  struct nzm_spawn_fd_guard g=NZM_SPAWN_FD_GUARD_INIT;
  int rc=nzm_spawn_fd_enter(&g);
  if(rc)return rc;
  int fd=open(Z "pool_demand",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
  int e=errno;nzm_spawn_fd_leave(&g);
  if(fd<0)return -e;
  rc=nzm_demand_read(fd,v);
  int gr=nzm_spawn_fd_enter(&g);
  if(gr)return gr; /* FD table guard failure is fatal, never retry IO. */
  if(close(fd) && !rc)rc=-errno;
  nzm_spawn_fd_leave(&g);
  return rc;
}
static int real_quota(void *p,uint64_t limit) {
  (void)p;
  /* Zero first, including when enable writes/readback fail. */
  int rc=nzm_putnum(Z "writeback_limit",0);
  int r=nzm_putnum(Z "writeback_limit_enable",1);
  if(!rc)rc=r;
  if(!rc && limit)rc=nzm_putnum(Z "writeback_limit",limit);
  uint64_t enabled=0,got=UINT64_MAX;
  r=nzm_number(Z "writeback_limit_enable",&enabled);if(!rc)rc=r;
  r=nzm_number(Z "writeback_limit",&got);if(!rc)rc=r;
  if(!rc && (enabled!=1 || got!=limit))rc=-EIO;
  return rc;
}
static int real_intent(void *p,struct nzm_core *c,const struct nzm_plan *plan) {
  (void)p;return nzm_intent_create(c,plan);
}
static int real_exec(void *p,struct nzm_core *c,struct nzm_demand_exec_wire *w) {
  (void)p;
  struct nzm_spawn_fd_guard g=NZM_SPAWN_FD_GUARD_INIT;
  int rc=nzm_spawn_fd_enter(&g);
  if(rc)return rc;
  int fd=open(RB_DEVICE_PATH,O_RDWR|O_CLOEXEC|O_NOFOLLOW),e=errno;
  nzm_spawn_fd_leave(&g);
  if(fd<0)return -e;
  struct stat st;char label[128];
  if(fstat(fd,&st))rc=-errno;
  if(!rc && (!S_ISBLK(st.st_mode) || st.st_rdev!=c->execution.transport.expected_rdev))rc=-ESTALE;
  if(!rc) {
    ssize_t n=fgetxattr(fd,"security.selinux",label,sizeof label);
    size_t want=strlen(RB_DEVICE_LABEL);
    if(n<0)rc=-errno;
    else if(!((n==(ssize_t)want || (n==(ssize_t)want+1 && !label[want])) && !memcmp(label,RB_DEVICE_LABEL,want)))rc=-EPERM;
  }
  if(!rc)rc=real_guard(NULL,c);
  if(!rc) {
    c->attempted=1;
#ifdef __BIONIC__
    if(ioctl(fd,(int)NZM_DEMAND_EXEC_IOCTL,w))rc=-errno;
#else
    if(ioctl(fd,NZM_DEMAND_EXEC_IOCTL,w))rc=-errno;
#endif
  }
  int gr=nzm_spawn_fd_enter(&g);
  if(gr)return gr;
  if(close(fd) && !rc)rc=-errno;
  nzm_spawn_fd_leave(&g);
  return rc;
}
static int real_retire(void *p,struct nzm_core *c) {
  (void)p;return nzm_intent_retire(c);
}
static int real_now(void *p,uint64_t *now) {
  (void)p;return nzm_memory_now(now);
}
static const struct nzm_demand_exec_ops real_ops={
  .guard=real_guard,.status=real_status,.demand=real_demand,.quota=real_quota,
  .intent=real_intent,.exec=real_exec,.retire=real_retire,.now=real_now
};

static int bound(const struct rb_status_result *s,const struct nzm_demand_view *v) {
  if(!s->known || s->selection!=RB_BACKEND_REQUEST || !s->ready ||
     !s->identity.instance || !s->identity.generation ||
     (s->page_size!=4096 && s->page_size!=16384 && s->page_size!=65536) ||
     !s->disksize_bytes || s->disksize_bytes%s->page_size)return -EPROTO;
  if(v->instance!=s->identity.instance || v->generation!=s->identity.generation)return -ESTALE;
  if(!v->sequence || v->enabled!=1 || v->initialized!=1 || v->backing!=1 || v->active!=1)return -EAGAIN;
  return 0;
}
static int recheck(const struct nzm_demand_exec_ops *o,void *p,struct nzm_core *c,
                   const struct rb_status_result *s,const struct nzm_demand_view *v) {
  struct rb_status_result now={0};struct nzm_demand_view d={0};
  int rc=o->guard(p,c);
  if(!rc)rc=o->status(p,c,&now);
  if(!rc)rc=o->demand(p,&d);
  if(!rc)rc=bound(&now,&d);
  if(!rc && (d.sequence!=v->sequence || now.identity.instance!=s->identity.instance ||
      now.identity.generation!=s->identity.generation || now.page_size!=s->page_size ||
      now.disksize_bytes!=s->disksize_bytes || d.min_age_seconds!=v->min_age_seconds))rc=-ESTALE;
  return rc;
}
static int exact(const struct nzm_demand_exec_wire *in,const struct nzm_demand_exec_wire *out,uint64_t pages) {
  struct nzm_demand_exec_wire a=*in,b=*out;
  memset((char *)&a.exec+offsetof(struct zram_wb_exec,scanned_pages),0,
         sizeof a.exec-offsetof(struct zram_wb_exec,scanned_pages));
  memset((char *)&b.exec+offsetof(struct zram_wb_exec,scanned_pages),0,
         sizeof b.exec-offsetof(struct zram_wb_exec,scanned_pages));
  a.next_index=b.next_index=0;
  const struct zram_wb_exec *e=&out->exec;
  return !memcmp(&a,&b,sizeof a) && !e->reserved_out &&
    e->scanned_pages<=in->max_scanned_pages && e->selected_pages<=e->scanned_pages &&
    e->selected_pages<=in->exec.max_attempt_pages && e->submitted_pages<=e->selected_pages &&
    e->installed_pages<=e->submitted_pages && e->installed_pages<=in->exec.max_installed_pages &&
    e->invalidated_pages<=e->selected_pages-e->installed_pages && out->next_index<pages &&
    e->stop_reason>=1 && e->stop_reason<=11 && e->op_errno<=0 && e->op_errno>=-4095 &&
    e->error_stage<=ZRAM_WB_STAGE_IO && (e->op_errno || !e->error_stage) &&
    (e->stop_reason!=ZRAM_WB_STOP_ERROR || e->op_errno<0) &&
    (e->stop_reason!=ZRAM_WB_STOP_STALE || (e->op_errno==-ESTALE &&
      !e->scanned_pages && !e->selected_pages && !e->submitted_pages && !e->installed_pages && !e->invalidated_pages)) &&
    (e->stop_reason!=ZRAM_WB_STOP_NOT_READY || (e->op_errno<0 &&
      !e->scanned_pages && !e->selected_pages && !e->submitted_pages && !e->installed_pages && !e->invalidated_pages));
}

int nzm_core_demand_exec(struct nzm_core *c,struct nzm_demand_exec_state *state,
                         const struct nzm_demand_exec_ops *ops,void *p) {
  const struct nzm_demand_exec_ops *o=ops ? ops : &real_ops;
  int rc=-EINVAL,dirty=0;
  uint64_t request_id=0,now=0;
  struct rb_status_result s={0};struct nzm_demand_view v={0};
  if(state) {
    state->attempted=state->known=state->settled=0;
    state->known_submitted_kib=state->pending_kib=0;
    state->granted_pages=0;state->budget_deadline_ns=0;
    memset(&state->reply,0,sizeof state->reply);
  }
  if(c) {
    memset(&c->execution.result,0,sizeof c->execution.result);
    memset(&c->last_plan,0,sizeof c->last_plan);c->reserved=c->attempted=0;
  }
  if(!o->quota)return -EINVAL;
  int zero=o->quota(p,0);
  if(zero) {rc=zero;goto done;}
  if(!c || !state || !o->guard || !o->status || !o->demand ||
     !o->intent || !o->exec || !o->retire || !o->now)goto done;
  if(state->halted || c->execution.halted || c->memory_budget.halted || c->intent_owned) {
    rc=-EPERM;goto done;
  }
  if(!c->boot_completed || !c->config.enable_writeback || !c->config.demand_enabled) {
    rc=-EAGAIN;goto done;
  }
  if((rc=o->guard(p,c)) || (rc=o->status(p,c,&s)) || (rc=o->demand(p,&v)) ||
     (rc=bound(&s,&v)))goto done;
  if(state->expected_sequence && state->expected_sequence!=v.sequence) {rc=-EAGAIN;goto done;}
  if(!c->execution.next_request_id) {rc=-EOVERFLOW;goto done;}
  if(!c->config.daily_mib || c->config.daily_mib>UINT64_MAX/1024 ||
     !c->config.demand_batch_mib || c->config.demand_batch_mib>UINT64_MAX/1024 ||
     !c->config.demand_scan_limit || c->config.demand_scan_limit==UINT64_MAX ||
     !v.min_age_seconds || v.min_age_seconds>UINT64_MAX/1000000000) {rc=-EINVAL;goto done;}
  if((rc=o->now(p,&now)))goto done;
  uint64_t daily=c->config.daily_mib*1024,grant=c->config.demand_batch_mib*1024;
  uint64_t available=0,page_kib=s.page_size/1024;
  if((rc=nzm_memory_available(&c->memory_budget,now,daily,&available)) ||
     (rc=nzm_memory_due(&c->memory_budget,now,daily,page_kib,page_kib,
                        &state->budget_deadline_ns)))goto done;
  if(grant>available)grant=available;
  grant/=page_kib;
  if(grant>c->config.demand_scan_limit)grant=c->config.demand_scan_limit;
  uint64_t diskpages=s.disksize_bytes/s.page_size;
  if(grant>diskpages)grant=diskpages;
  if(!grant) {rc=-EDQUOT;goto done;}
  struct nzm_plan plan={.pages=grant,.reserve_kib=grant*page_kib,
                       .limit_4k=grant*(s.page_size/4096)};
  if((rc=recheck(o,p,c,&s,&v)))goto done;
  if((rc=nzm_memory_reserve(&c->memory_budget,now,plan.reserve_kib,daily)))goto done;
  c->execution.backend=RB_BACKEND_REQUEST;c->execution.selected=1;
  c->execution.identity=s.identity;c->last_plan=plan;c->reserved=1;
  state->granted_pages=grant;
  request_id=c->execution.next_request_id;
  dirty=1; /* Even failed creation may leave durable/partial intent evidence. */
  rc=o->intent(p,c,&plan);
  c->execution.next_request_id=request_id==UINT64_MAX ? 0 : request_id+1;
  if(rc)goto done;
  nzm_log("demand reserve request_id=%llu grant_pages=%llu available_kib=%llu",
      (unsigned long long)request_id,(unsigned long long)grant,(unsigned long long)available);
  if((rc=recheck(o,p,c,&s,&v)) || (rc=o->quota(p,plan.limit_4k)) ||
     (rc=recheck(o,p,c,&s,&v)))goto done;
  struct nzm_demand_exec_wire w={0};
  w.exec.size=sizeof w;w.exec.version=1;w.exec.request_id=request_id;
  w.exec.expected_instance=s.identity.instance;w.exec.expected_generation=s.identity.generation;
  w.exec.max_attempt_pages=w.exec.max_installed_pages=grant;
  w.demand_sequence=v.sequence;w.max_scanned_pages=c->config.demand_scan_limit;
  w.min_age_ns=v.min_age_seconds*UINT64_C(1000000000);
  w.start_index=state->next_index%diskpages;
  struct nzm_demand_exec_wire input=w;
  state->attempted=1;c->execution.result.no_replay=1;
  rc=o->exec(p,c,&w); /* Exactly once, including EINTR/EFAULT. */
  zero=o->quota(p,0); /* Close authorization before postcondition work. */
  if(rc)goto done;
  if(!exact(&input,&w,diskpages)) {rc=-EPROTO;goto done;}
  state->known=1;state->reply=w;
  c->execution.result.known=1;c->execution.result.op_errno=w.exec.op_errno;
  c->execution.result.counters=(struct rb_request_counters){w.exec.scanned_pages,
    w.exec.selected_pages,w.exec.submitted_pages,w.exec.installed_pages,w.exec.invalidated_pages};
  if(zero) {rc=zero;goto done;}
  /* EXEC can deactivate demand; postcheck authority/lifecycle, not old demand. */
  struct rb_status_result post={0};
  if((rc=o->guard(p,c)) || (rc=o->status(p,c,&post)))goto done;
  if(!post.known || !post.ready || post.selection!=RB_BACKEND_REQUEST ||
     post.identity.instance!=s.identity.instance ||
     post.identity.generation!=s.identity.generation || post.page_size!=s.page_size ||
     post.disksize_bytes!=s.disksize_bytes) {rc=-ESTALE;goto done;}
  uint64_t completion=0;
  if((rc=o->now(p,&completion)))goto done;
  if(completion<now) {rc=-ESTALE;goto done;}
  if(w.exec.submitted_pages>UINT64_MAX/page_kib) {rc=-EOVERFLOW;goto done;}
  uint64_t submitted=w.exec.submitted_pages*page_kib;
  if((rc=nzm_memory_settle_known(&c->memory_budget,completion,submitted)))goto done;
  state->settled=1;
  if((rc=nzm_memory_due(&c->memory_budget,completion,daily,page_kib,page_kib,
                        &state->budget_deadline_ns)))goto done;
  /* A valid op error accounts submissions but never retires evidence. */
  if(w.exec.op_errno) {rc=w.exec.op_errno;goto done;}
  if((rc=o->retire(p,c)))goto done;
  state->next_index=w.next_index;dirty=0;
 done:
  /* Retain the full outstanding reservation on every uncertain path. The last
   * validated time is sufficient: unknown reservations never age or refund. */
  if(c && dirty && c->memory_budget.pending) {
    int retained=nzm_memory_retain_unknown(&c->memory_budget,c->memory_budget.last_ns);
    if(retained && !rc)rc=retained;
  }
  int close_error=o->quota(p,0);
  if(close_error) {zero=close_error;rc=close_error;}
  if(state && (dirty || zero))state->halted=1;
  if(c && (dirty || zero))c->execution.halted=1;
  if(state && c) {
    state->known_submitted_kib=c->memory_budget.known_submitted_kib;
    state->pending_kib=c->memory_budget.pending_kib;
    const struct zram_wb_exec *e=&state->reply.exec;
    nzm_log("demand outcome boot=%s request_id=%llu sequence=%llu approved_pages=%llu attempted=%d known=%d scanned=%llu submitted=%llu installed=%llu settled=%d known_submitted_kib=%llu pending_kib=%llu errno=%d stop=%u quota_closed=%d halted=%d",
        c->boot,(unsigned long long)request_id,(unsigned long long)v.sequence,
        (unsigned long long)state->granted_pages,state->attempted,state->known,
        (unsigned long long)e->scanned_pages,(unsigned long long)e->submitted_pages,
        (unsigned long long)e->installed_pages,state->settled,
        (unsigned long long)state->known_submitted_kib,(unsigned long long)state->pending_kib,
        -rc,e->stop_reason,!zero,state->halted);
  }
  return rc;
}

int nzm_demand_execute(struct nzm_core *c,const struct nzm_demand_view *view) {
  if(!c)return -EINVAL;
  /* Ephemeral view/cursor state never resets the core-owned budget or ID. */
  struct nzm_demand_exec_state state={.expected_sequence=view ? view->sequence : 0};
  return nzm_core_demand_exec(c,&state,NULL,NULL);
}
