#define _GNU_SOURCE
#include "demand_exec.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fixture {
  struct nzm_core c;
  struct nzm_demand_exec_state state;
  int calls,fail_at,exec_error,malformed,paused,changed,committed,intent,retired,execs;
  uint64_t quota,used,highwater,validated,page;
  size_t records;
  int preflight_error,commit_error,preflighted;
};
static int step(struct fixture *f) {return ++f->calls==f->fail_at ? -EIO : 0;}
static int guard(void *p,struct nzm_core *c) {
  (void)c;struct fixture *f=p;int rc=step(f);return rc ? rc : f->paused ? -EPERM : 0;
}
static int status(void *p,struct nzm_core *c,struct rb_status_result *s) {
  (void)c;struct fixture *f=p;int rc=step(f);if(rc)return rc;
  *s=(struct rb_status_result){.known=1,.ready=1,.selection=RB_BACKEND_REQUEST,
    .identity={11,22},.page_size=(uint32_t)f->page,.disksize_bytes=UINT64_C(1)<<30};
  if(f->changed && f->execs)s->identity.generation++;
  return 0;
}
static int demand(void *p,struct nzm_demand_view *v) {
  struct fixture *f=p;int rc=step(f);if(rc)return rc;
  *v=(struct nzm_demand_view){.sequence=7,.instance=11,.generation=22,
    .enabled=1,.initialized=1,.backing=1,.active=1,.min_age_seconds=60};return 0;
}
static int ledger(void *p,struct nzm_core *c,struct nzm_ledger *l) {
  (void)c;struct fixture *f=p;int rc=step(f);if(rc)return rc;
  size_t count=f->records ? f->records : 1;
  *l=(struct nzm_ledger){.generation=count,.count=count,.highwater=f->highwater,
    .validated_at=f->validated,.cumulative=f->records ? count : f->used,.used_kib=0};
  l->records=calloc(count,sizeof *l->records);assert(l->records);
  for(size_t i=0;i<count;i++)
    l->records[i]=(struct nzm_reservation){f->records ? 1 : f->highwater,f->records ? 1 : f->used};
  return 0;
}
static int quota(void *p,uint64_t q) {
  struct fixture *f=p;
  if(q)assert(f->committed && f->intent);
  f->quota=q;return step(f);
}
static int preflight(void *p,struct nzm_core *c,size_t n) {
  (void)c;struct fixture *f=p;assert(!f->intent && !f->quota && n && n<=NZM_LEDGER_MAX_BYTES);
  int rc=step(f);if(rc)return rc;
  if(f->preflight_error)return f->preflight_error;
  f->preflighted=1;return 0;
}
static int intent(void *p,struct nzm_core *c,const struct nzm_plan *plan) {
  struct fixture *f=p;assert(f->preflighted && !f->quota && !f->committed && plan->pages);
  f->intent=1;c->intent_owned=1;return step(f);
}
static int commit(void *p,struct nzm_core *c,const char *s,size_t n) {
  (void)c;struct fixture *f=p;assert(f->intent && !f->quota);
  struct nzm_ledger l={0};assert(!nzm_ledger_parse(s,n,f->highwater,&l));
  assert(l.highwater==f->highwater && l.cumulative>f->used);
  nzm_ledger_free(&l);int rc=step(f);if(!rc)rc=f->commit_error;
  if(!rc)f->committed=1;
  return rc;
}
static int execute(void *p,struct nzm_core *c,struct nzm_demand_exec_wire *w) {
  (void)c;struct fixture *f=p;assert(f->committed && f->intent && f->quota);
  assert(++f->execs==1);
  assert(w->exec.size==224 && w->exec.version==1 && !w->exec.mode &&
    !w->exec.page_index && !w->exec.idle_count && w->demand_sequence==7);
  assert(w->exec.expected_instance==11 && w->exec.expected_generation==22);
  assert(w->min_age_ns==UINT64_C(60000000000));
  assert(w->exec.max_attempt_pages==w->exec.max_installed_pages);
  assert(w->exec.max_attempt_pages*f->page/4096==f->quota);
  int rc=step(f);if(rc)return rc;if(f->exec_error)return f->exec_error;
  w->exec.scanned_pages=w->exec.selected_pages=w->exec.submitted_pages=w->exec.installed_pages=1;
  w->exec.stop_reason=ZRAM_WB_STOP_COMPLETE;w->next_index=1;
  if(f->malformed)w->exec.installed_pages=w->exec.max_installed_pages+1;
  return 0;
}
static int retire(void *p,struct nzm_core *c) {
  struct fixture *f=p;assert(!f->quota && f->execs==1 && c->execution.result.known);
  int rc=step(f);if(rc)return rc;f->retired=1;f->intent=0;c->intent_owned=0;return 0;
}
static const struct nzm_demand_exec_ops ops={guard,status,demand,ledger,quota,intent,commit,execute,retire,preflight};
static void init(struct fixture *f,uint64_t page) {
  memset(f,0,sizeof *f);f->page=page;f->highwater=100;f->validated=9999999;f->used=4;
  f->state.next_request_id=1;f->c.boot_completed=1;
  f->c.config.enable_writeback=f->c.config.demand_enabled=1;
  f->c.config.daily_mib=1;f->c.config.demand_batch_mib=2;
  f->c.config.demand_scan_limit=1000;
}
static int run(struct fixture *f) {return nzm_core_demand_exec(&f->c,&f->state,&ops,f);}
int main(void) {
  _Static_assert(sizeof(struct nzm_demand_exec_wire)==224,"ABI size");
  assert(_IOC_NR(NZM_DEMAND_EXEC_IOCTL)==0x22 && _IOC_SIZE(NZM_DEMAND_EXEC_IOCTL)==224);
  struct fixture f;
  for(uint64_t page=4096;page<=65536;page*=4) {
    init(&f,page);assert(!run(&f));assert(!f.quota && f.retired && f.state.known);
    assert(f.state.granted_pages==(1024-4)/(page/1024));assert(f.state.next_index==1);
  }
  init(&f,4096);assert(!run(&f));int total=f.calls;
  /* Fail every injected boundary: no retry; zero quota cleanup always runs. */
  for(int i=1;i<=total;i++) {
    init(&f,4096);f.fail_at=i;assert(run(&f)<0);assert(!f.quota && f.execs<=1);
    if(f.state.halted) {int before=f.execs;assert(run(&f)<0);assert(f.execs==before);}
  }
  init(&f,4096);f.exec_error=-EFAULT;assert(run(&f)==-EFAULT);
  assert(!f.quota && f.intent && !f.retired && !f.state.known && f.state.halted);
  assert(run(&f)==-EPERM && f.execs==1);
  init(&f,4096);f.malformed=1;assert(run(&f)==-EPROTO && f.intent && !f.state.known);
  init(&f,4096);f.changed=1;assert(run(&f)==-ESTALE && f.intent && f.state.known);
  init(&f,4096);f.paused=1;assert(run(&f)==-EPERM && !f.execs && !f.intent && !f.quota);
  init(&f,4096);f.used=1024;assert(run(&f)==-EDQUOT && !f.execs && !f.intent);
  init(&f,4096);f.state.expected_sequence=8;assert(run(&f)==-EAGAIN && !f.intent);
  init(&f,4096);f.c.config.demand_scan_limit=2;assert(!run(&f) && f.state.granted_pages==2);
  const int errors[]={-ENOSPC,-EIO,-EOVERFLOW,-E2BIG};
  for(size_t i=0;i<sizeof errors/sizeof errors[0];i++) {
    init(&f,4096);f.preflight_error=errors[i];assert(run(&f)==errors[i]);
    assert(!f.intent && !f.committed && !f.execs && !f.quota && !f.state.halted);
  }
  init(&f,4096);f.records=NZM_LEDGER_MAX_RECORDS;f.highwater=100000;
  assert(run(&f)==-E2BIG && !f.preflighted && !f.intent && !f.execs && !f.quota);
  init(&f,4096);f.commit_error=-ENOSPC;
  assert(run(&f)==-ENOSPC && f.intent && !f.committed && !f.execs && !f.quota && f.state.halted);
  assert(run(&f)==-EPERM && !f.execs);
  puts("PASS demand execution: frozen wear, bounded grant, ordering, capacity preflight, fault matrix, no replay");
  return 0;
}
