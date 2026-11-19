#define _GNU_SOURCE
#include "demand_exec.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

struct fixture {
  struct nzm_core c;
  struct nzm_demand_exec_state state;
  int calls,fail_at,exec_error,malformed,paused,changed,intent,retired,execs;
  int clocks,clock_error,op_error,post_zero_error,retire_error,backend_changed;
  int old_txn,old_intent;
  uint64_t quota,page,request,submitted_pages,installed_pages,start_ns,completion_ns;
};
static int step(struct fixture *f) {return ++f->calls==f->fail_at ? -EIO : 0;}
static int guard(void *p,struct nzm_core *c) {
  struct fixture *f=p;int rc=step(f);
  assert(c==&f->c);
  return rc ? rc : f->paused ? -EPERM : (f->old_txn || f->old_intent) ? -EEXIST : 0;
}
static int status(void *p,struct nzm_core *c,struct rb_status_result *s) {
  (void)c;struct fixture *f=p;int rc=step(f);if(rc)return rc;
  *s=(struct rb_status_result){.known=1,.ready=1,.selection=RB_BACKEND_REQUEST,
    .identity={11,22},.page_size=(uint32_t)f->page,.disksize_bytes=UINT64_C(1)<<30};
  if(f->changed && f->execs)s->identity.generation++;
  if(f->backend_changed && f->execs)s->selection=RB_BACKEND_LEGACY;
  return 0;
}
static int demand(void *p,struct nzm_demand_view *v) {
  struct fixture *f=p;int rc=step(f);if(rc)return rc;
  *v=(struct nzm_demand_view){.sequence=7,.instance=11,.generation=22,
    .enabled=1,.initialized=1,.backing=1,.active=1,.min_age_seconds=60};return 0;
}
static int quota(void *p,uint64_t q) {
  struct fixture *f=p;
  if(q)assert(f->c.memory_budget.pending && f->intent);
  f->quota=q;int rc=step(f);
  return rc ? rc : (!q && f->execs && f->post_zero_error) ? -EIO : 0;
}
static int intent(void *p,struct nzm_core *c,const struct nzm_plan *plan) {
  struct fixture *f=p;assert(!f->quota && plan->pages && c->memory_budget.pending);
  assert(c->memory_budget.pending_kib==plan->reserve_kib);
  assert(c->execution.next_request_id==f->request);
  f->intent=1;c->intent_owned=1;return step(f);
}
static int execute(void *p,struct nzm_core *c,struct nzm_demand_exec_wire *w) {
  struct fixture *f=p;assert(c->memory_budget.pending && f->intent && f->quota);
  f->execs++;
  assert(w->exec.size==224 && w->exec.version==1 && !w->exec.mode &&
    !w->exec.page_index && !w->exec.idle_count && w->demand_sequence==7);
  assert(w->exec.request_id==f->request);
  assert(w->exec.expected_instance==11 && w->exec.expected_generation==22);
  assert(w->min_age_ns==UINT64_C(60000000000));
  assert(w->exec.max_attempt_pages==w->exec.max_installed_pages);
  assert(w->exec.max_attempt_pages*f->page/4096==f->quota);
  int rc=step(f);if(rc)return rc;if(f->exec_error)return f->exec_error;
  w->exec.scanned_pages=w->exec.selected_pages=w->exec.submitted_pages=f->submitted_pages;
  w->exec.installed_pages=f->installed_pages;
  if(w->exec.submitted_pages==UINT64_MAX)
    w->exec.scanned_pages=w->exec.selected_pages=w->exec.submitted_pages=w->exec.installed_pages=w->exec.max_attempt_pages;
  w->exec.stop_reason=f->op_error ? ZRAM_WB_STOP_ERROR : ZRAM_WB_STOP_COMPLETE;
  w->exec.op_errno=f->op_error;w->next_index=1;
  if(f->malformed)w->exec.installed_pages=w->exec.max_installed_pages+1;
  return 0;
}
static int now(void *p,uint64_t *ns) {
  struct fixture *f=p;int rc=step(f);if(rc)return rc;
  f->clocks++;
  if(f->c.execution.result.known) {
    assert(!f->quota && f->intent);
    if(f->clock_error)return f->clock_error;
    *ns=f->completion_ns;
  } else *ns=f->start_ns;
  return 0;
}
static int retire(void *p,struct nzm_core *c) {
  struct fixture *f=p;
  assert(!f->quota && c->execution.result.known && !c->memory_budget.pending);
  assert(c->memory_budget.known_submitted_kib>=f->submitted_pages*f->page/1024 || f->submitted_pages==UINT64_MAX);
  int rc=step(f);if(rc)return rc;if(f->retire_error)return f->retire_error;
  f->retired++;f->intent=0;c->intent_owned=0;return 0;
}
static const struct nzm_demand_exec_ops ops={
  .guard=guard,.status=status,.demand=demand,.quota=quota,.intent=intent,
  .exec=execute,.retire=retire,.now=now
};
static void init(struct fixture *f,uint64_t page) {
  memset(f,0,sizeof *f);f->page=page;f->request=1;
  f->start_ns=9999999;f->completion_ns=f->start_ns+NZM_MEMORY_BUCKET_NS;
  assert(!nzm_memory_init(&f->c.memory_budget,0));
  /* Simulate four KiB of previous confirmed submission. */
  assert(!nzm_memory_reserve(&f->c.memory_budget,0,4,1024));
  assert(!nzm_memory_settle_known(&f->c.memory_budget,0,4));
  f->submitted_pages=f->installed_pages=1;f->c.execution.next_request_id=1;
  f->c.boot_completed=1;f->c.config.enable_writeback=f->c.config.demand_enabled=1;
  f->c.config.daily_mib=1;f->c.config.demand_batch_mib=2;f->c.config.demand_scan_limit=1000;
}
static int run(struct fixture *f) {return nzm_core_demand_exec(&f->c,&f->state,&ops,f);}
static void unknown(struct fixture *f) {
  assert(f->intent && f->state.halted && f->c.execution.halted);
  assert(f->c.memory_budget.halted && f->c.memory_budget.unknown_kib==1020);
  assert(f->state.pending_kib==1020 && !f->state.settled && !f->retired && !f->quota);
  int before=f->execs;assert(run(f)<0 && f->execs==before);
}
int main(void) {
  _Static_assert(sizeof(struct nzm_demand_exec_wire)==224,"ABI size");
  assert(_IOC_NR(NZM_DEMAND_EXEC_IOCTL)==0x22 && _IOC_SIZE(NZM_DEMAND_EXEC_IOCTL)==224);
  struct fixture f;
  for(uint64_t page=4096;page<=65536;page*=4) {
    init(&f,page);assert(!run(&f));assert(!f.quota && f.retired && f.state.known && f.state.settled);
    assert(f.state.granted_pages==1020/(page/1024) && f.state.next_index==1);
    assert(f.c.execution.next_request_id==2 && f.c.memory_budget.known_submitted_kib==4+page/1024);
  }
  init(&f,4096);assert(!run(&f));int total=f.calls;
  for(int i=1;i<=total;i++) {
    init(&f,4096);f.fail_at=i;assert(run(&f)<0);assert(!f.quota && f.execs<=1);
    if(f.state.halted) {int before=f.execs;assert(run(&f)<0);assert(f.execs==before);}
  }
  const int ambiguous[]={-EFAULT,-EINTR,-EIO};
  for(size_t i=0;i<sizeof ambiguous/sizeof ambiguous[0];i++) {
    init(&f,4096);f.exec_error=ambiguous[i];assert(run(&f)==ambiguous[i]);unknown(&f);
  }
  init(&f,4096);f.malformed=1;assert(run(&f)==-EPROTO && !f.state.known);unknown(&f);
  init(&f,4096);f.changed=1;assert(run(&f)==-ESTALE && f.state.known);unknown(&f);
  init(&f,4096);f.paused=1;assert(run(&f)==-EPERM && !f.execs && !f.intent && !f.quota);
  for(int old=0;old<2;old++) {
    init(&f,4096);f.old_txn=!old;f.old_intent=old;
    assert(run(&f)==-EEXIST && !f.execs && !f.intent && !f.c.memory_budget.pending);
  }
  init(&f,4096);f.state.expected_sequence=8;assert(run(&f)==-EAGAIN && !f.intent);
  init(&f,4096);f.c.config.demand_scan_limit=2;assert(!run(&f) && f.state.granted_pages==2);
  init(&f,4096);f.request=f.c.execution.next_request_id=UINT64_MAX;assert(!run(&f));
  assert(!f.c.execution.next_request_id);
  f.execs=0;assert(run(&f)==-EOVERFLOW && !f.execs);
  init(&f,4096);f.c.execution.next_request_id=0;assert(run(&f)==-EOVERFLOW && !f.intent);
  init(&f,4096);f.state.known=f.state.attempted=1;f.state.reply.exec.installed_pages=99;
  f.paused=1;assert(run(&f)==-EPERM && !f.state.known && !f.state.attempted && !f.state.reply.exec.installed_pages);
  /* Submitted rather than installed pages charge at fresh completion time. */
  init(&f,4096);f.installed_pages=0;assert(!run(&f));
  assert(f.state.settled && f.state.known_submitted_kib==8 && !f.state.pending_kib);
  assert(f.c.memory_budget.buckets[f.c.memory_budget.count-1].index==1);
  init(&f,4096);f.submitted_pages=f.installed_pages=0;assert(!run(&f));
  assert(f.state.settled && f.state.known_submitted_kib==4 && f.retired);
  init(&f,4096);f.submitted_pages=UINT64_MAX;assert(!run(&f));
  assert(f.state.known_submitted_kib==1024);
  init(&f,4096);f.op_error=-EIO;assert(run(&f)==-EIO);
  assert(f.state.settled && f.state.known_submitted_kib==8 && !f.state.pending_kib);
  assert(f.intent && !f.retired && f.state.halted && !f.c.memory_budget.unknown_kib);
  init(&f,4096);f.post_zero_error=1;assert(run(&f)==-EIO);unknown(&f);
  init(&f,4096);f.clock_error=-EIO;assert(run(&f)==-EIO);unknown(&f);
  init(&f,4096);f.completion_ns=f.start_ns-1;assert(run(&f)==-ESTALE);unknown(&f);
  init(&f,4096);f.backend_changed=1;assert(run(&f)==-ESTALE);unknown(&f);
  init(&f,4096);f.retire_error=-EIO;assert(run(&f)==-EIO && f.state.settled && f.intent && f.state.halted);
  /* New per-call state does not reset core accounting or consumed IDs. */
  init(&f,4096);assert(!run(&f));
  memset(&f.state,0,sizeof f.state);f.start_ns=f.completion_ns;f.completion_ns++;
  f.request=2;assert(!run(&f));assert(f.c.execution.next_request_id==3);
  assert(f.c.memory_budget.known_submitted_kib==12 && f.c.memory_budget.approved_kib==2040);
  /* Exhaustion waits until enough expired KiB fit one actual device page. */
  init(&f,4096);assert(!nzm_memory_reserve(&f.c.memory_budget,0,1020,1024));
  assert(!nzm_memory_settle_known(&f.c.memory_budget,0,1020));
  assert(run(&f)==-EDQUOT && !f.intent && !f.execs);
  assert(f.state.budget_deadline_ns==NZM_MEMORY_WINDOW_NS+NZM_MEMORY_BUCKET_NS);
  f.start_ns=f.state.budget_deadline_ns;f.completion_ns=f.start_ns+1;
  assert(!run(&f));
  /* Four KiB expiring first is not enough for a 16-KiB page: the returned
   * deadline walks forward to the second bucket instead of busy-looping. */
  init(&f,16384);
  assert(!nzm_memory_reserve(&f.c.memory_budget,NZM_MEMORY_BUCKET_NS,1020,1024));
  assert(!nzm_memory_settle_known(&f.c.memory_budget,NZM_MEMORY_BUCKET_NS,1020));
  f.start_ns=NZM_MEMORY_BUCKET_NS+1;
  assert(run(&f)==-EDQUOT);
  assert(f.state.budget_deadline_ns==NZM_MEMORY_WINDOW_NS+2*NZM_MEMORY_BUCKET_NS);
  f.start_ns=NZM_MEMORY_WINDOW_NS+NZM_MEMORY_BUCKET_NS;
  assert(run(&f)==-EDQUOT);
  assert(f.state.budget_deadline_ns==NZM_MEMORY_WINDOW_NS+2*NZM_MEMORY_BUCKET_NS);
  f.start_ns=f.state.budget_deadline_ns;f.completion_ns=f.start_ns+1;
  assert(!run(&f));
  /* Reinitializing a daemon object resets accounting; no persistent wear IO. */
  struct nzm_memory_budget fresh;
  assert(!nzm_memory_init(&fresh,0) && !fresh.approved_kib && !fresh.pending_kib);
  puts("PASS demand execution: memory reserve/intent/exec/closure/settle, faults, IDs, expiry, no replay");
  return 0;
}
