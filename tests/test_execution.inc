/* Serial syscall-boundary fixtures: Stage 3 core/adapter/frozen rb linked.
 * No fixture runs concurrently; real helper concurrency is a separate suite. */
#include "nzm_internal.h"
static int request_fixture_ioctl(int fd, unsigned long command, void *arg) {
  assert(rb_fd==-1);rb_fd=fd;
  assert(fcntl(fd,F_GETFD)&FD_CLOEXEC);
  int flags=fcntl(fd,F_GETFL);
  assert(flags&O_NOFOLLOW);
  if(command==ZRAM_IOC_WB_STATUS) {
    assert((flags&O_ACCMODE)==O_RDONLY);
    rb_status_calls++;
    if(rb_status_errno) {errno=rb_status_errno;return -1;}
    struct zram_wb_status *s=arg;
    assert(s->size==sizeof *s && s->version==1 && !s->flags);
    s->capabilities=ZRAM_WB_CAP_STATUS|ZRAM_WB_CAP_EXEC|ZRAM_WB_CAP_FILTERED_IDLE|ZRAM_WB_CAP_DUAL_LIMIT;
    s->instance=11;s->generation=rb_generation;s->disksize_bytes=rb_disk;
    s->page_size=(uint32_t)(rb_page_mismatch ? 16384 : rb_status_page);
    s->state_flags=ZRAM_WB_STATE_INITIALIZED|ZRAM_WB_STATE_BACKING;
    if(rb_global_enabled)s->state_flags|=ZRAM_WB_STATE_GLOBAL_LIMIT;
    s->wb_limit_remaining_4k=rb_remaining;
    s->pages_stored=rb_disk/rb_status_page;s->lowratio_pages=s->pages_stored;
    if(rb_invalid_samples) {s->same_pages=UINT64_MAX;rb_invalid_samples--;}
    if(rb_bad_status)s->reserved_out[0]=1;
    return 0;
  }
  assert(command==ZRAM_IOC_WB_EXEC && (flags&O_ACCMODE)==O_RDWR);
  rb_exec_calls++;
  struct zram_wb_exec *r=arg;
  assert(r->size==sizeof *r && r->version==1 && r->mode==ZRAM_WB_MODE_FILTERED_IDLE);
  assert(r->expected_instance==11 && r->expected_generation==rb_generation);
  assert(r->max_attempt_pages && r->max_attempt_pages==r->max_installed_pages);
  assert(r->max_attempt_pages==expected_charge/(rb_status_page/1024));
  assert(r->request_id && (!rb_last_id || r->request_id==rb_last_id+1));
  assert(!r->page_index && r->idle_count==1);
  rb_last_id=r->request_id;
  capture_submit_tick();verify_precharge_at_request();
  /* Model effects BEFORE the copyout fault. Never authorize replay. */
  file("/sys/block/zram0/bd_stat","0 0 999\n");
  if(rb_exec_errno) {errno=rb_exec_errno;return -1;}
  r->scanned_pages=r->selected_pages=r->max_attempt_pages;
  r->submitted_pages=r->max_attempt_pages;
  r->installed_pages=rb_installed;r->stop_reason=rb_stop_reason;
  if(rb_stop_reason==ZRAM_WB_STOP_COMPLETE)
    r->stop_reason=rb_installed==r->max_installed_pages ? ZRAM_WB_STOP_INSTALLED_LIMIT : ZRAM_WB_STOP_ATTEMPT_LIMIT;
  r->op_errno=rb_op_errno;
  if(rb_stop_reason==ZRAM_WB_STOP_ERROR)r->error_stage=ZRAM_WB_STAGE_IO;
  if(rb_bad_exec)r->request_id++;
  return 0;
}
static void request_fixture(void) {
  fail_pause_create=intent_fault=intent_dirsync=intent_retire_sync=0;intent_syncs=0;
  rb_mode=1;rb_status_errno=rb_exec_errno=rb_bad_status=rb_bad_exec=0;
  rb_invalid_samples=rb_global_enabled=rb_op_errno=rb_page_mismatch=0;
  rb_wrong_device=rb_wrong_label=rb_fail_open=0;
  rb_quota_writes=rb_idle_writes=0;rb_status_calls=rb_exec_calls=rb_legacy_calls=0;
  rb_fd=-1;rb_closes=0;rb_remaining=UINT64_MAX;rb_generation=7;rb_installed=1;
  rb_last_id=0;rb_status_page=cycle_page=4096;rb_disk=67108864;
  rb_stop_reason=ZRAM_WB_STOP_COMPLETE;
  watermark_probe=timing_probe=advance_idle_clock=0;
  switch_power=switch_screen=NULL;shrink_during_wait=grow_during_wait=0;
  bd_effect=0;attempted_effect=0;query_fail_at=identity_on_wait=0;
  helper_backing_mib=512;expected_charge=4096;request_calls=sleeps=0;
  monotonic_tick=1000;boot_prop_unreadable=0;
  success_fixture();
  file("/sys/block/zram0/writeback_limit_enable","1\n");
  file("/sys/block/zram0/writeback_limit","7777\n");
}
static void request_owner(struct nzm_core *c) {
  request_fixture();
  nzm_core_init(c);assert(!legacy_module_config(c));assert(!nzm_core_boot(c));
  assert(c->execution.backend==RB_BACKEND_REQUEST && rb_status_calls==1);
  c->boot_completed=1;watermark_probe=1;
}
static int fixture_exists(const char *suffix) {
  char p[1024];path(p,suffix);return access(p,F_OK)==0;
}
static void fd_closed(void) {
  /* The integer may already belong to a newly reopened identity fd. Check the
   * operation's close boundary, not a later fd number that can be reused. */
  assert(rb_fd==-1 && rb_closes==rb_status_calls+rb_exec_calls);
}
static void legacy_mm_stat_tests(void) {
  static const struct {const char *text;int error;} cases[]={
    {"67108864 0 134217728 0 0 0 0 0 0\n",-EOPNOTSUPP},
    {"67108864 0 134217728 0 0 0 0 0\n",-EOPNOTSUPP},
    {"67108864 0 134217728 0 0 0 0 0 0 4096 0\n",-EOPNOTSUPP},
    {"67108864 0 134217728 0 0 0 0 0 0 -1\n",-EINVAL},
    {"67108864 0 134217728 0 0 0 0 0 0 4096x\n",-EINVAL},
    {"67108864 0 134217728 0 0 0 0 0 0 18446744073709551616\n",-ERANGE},
    {"67108864 0 134217728 0 0 0 0 0 0 4096 18446744073709551616\n",-ERANGE},
    {"67108864 0 134217728 0 0 0 0 0 0 4096 garbage\n",-EINVAL},
    {"67108864 0 134217728\n0 0 0 0 0 0 4096\n",-EINVAL},
    {"\n",-EINVAL}
  };
  for(int automatic=0;automatic<2;automatic++) {
    for(size_t i=0;i<sizeof cases/sizeof cases[0];i++) {
      request_fixture();
      struct nzm_core c;nzm_core_init(&c);
      c.config.request_backend=automatic?NZM_REQUEST_AUTO:NZM_REQUEST_LEGACY;
      rb_status_errno=ENOTTY;
      assert(!nzm_core_boot(&c));
      assert(c.execution.backend==RB_BACKEND_LEGACY);
      c.boot_completed=1;watermark_probe=1;
      file("/sys/block/zram0/mm_stat",cases[i].text);
      uint64_t mm[10]={0},bd[3]={0},disk=0,page=0;
      int before=writes,opened=opens_write;
      assert(nzm_execution_sample(&c,mm,bd,&disk,&page)==cases[i].error);
      assert(writes==before && opens_write==opened); /* parser is readonly */
      assert(nzm_core_cycle(&c)==cases[i].error);
      assert(!strcmp(c.stage,"sample_unknown") && !c.reserved && !c.attempted);
      assert(!rb_exec_calls && !rb_legacy_calls && !rb_idle_writes && !rb_quota_writes);
      assert(!ledger_generation() && readnum("/sys/block/zram0/writeback_limit")==7777);
      assert(!fixture_exists("/data/vendor/extm/state/exec.intent"));
      nzm_core_close(&c);
    }
  }
  /* Known paired-kernel fixture, not runtime proof of tenth-field semantics. */
  request_fixture();
  struct nzm_core c;nzm_core_init(&c);c.config.request_backend=NZM_REQUEST_LEGACY;
  assert(!nzm_core_boot(&c));
  uint64_t mm[10],bd[3],disk,page;
  file("/sys/block/zram0/bd_stat","1 2 3\n");
  assert(!nzm_execution_sample(&c,mm,bd,&disk,&page));
  assert(mm[0]==67108864 && mm[2]==134217728 && mm[9]==4096);
  assert(bd[0]==1 && bd[1]==2 && bd[2]==3 && page==4096 && disk==67108864);
  nzm_core_close(&c);
  puts("PASS legacy paired mm_stat: known10; unknown shorter/longer unsupported; malformed/overflow rejected; no fallback writes; bd page4k unchanged");
}
static void execution_tests(void) {
  legacy_mm_stat_tests();
  /* Stage 3 daemon module selects and executes both paths. Default auto + ENOTTY
   * happens before idle/reservation/writeback, not after an EXEC failure. */
  for(int mode=0;mode<3;mode++) {
    request_fixture();
    if(mode==0)rb_status_errno=ENOTTY;
    if(mode==2)file("/vendor/etc/lge-zram.conf","request_backend=legacy\nwatermark_charging_idle_observation_seconds=301\nwatermark_battery_idle_observation_seconds=301\nsample_interval_seconds=503\nwatermark_charging_batch_mib=4\n");
    assert(stage3_start(1,0)==1); /* bounded fixture outer sleep */
    assert(ledger_generation()==1 && request_calls==1);
    assert(rb_exec_calls==(unsigned)(mode==1) && rb_legacy_calls==(unsigned)(mode!=1));
    assert(rb_quota_writes==(mode==1?0:2));
    assert(rb_status_calls==(unsigned)(mode==2?0:mode==0?1:4));
    if(mode==1)assert(readnum("/sys/block/zram0/writeback_limit")==7777);
    if(mode!=2)fd_closed();
  }
  puts("PASS Stage 3 daemon module: auto ENOTTY legacy, auto ioctl, explicit legacy; no ioctl global quota writes");
  /* ENOTTY never authorizes fallback in forced ioctl; permission/response
   * failure never authorizes fallback in auto. No idle mark or reservation. */
  for(int failure=0;failure<4;failure++) {
    request_fixture();
    if(failure==0)rb_status_errno=EACCES;
    if(failure==1)rb_bad_status=1;
    if(failure==2)rb_page_mismatch=1;
    if(failure==3) {rb_status_errno=ENOTTY;file("/vendor/etc/lge-zram.conf","request_backend=ioctl\n");}
    assert(stage3_start(1,0)==1);
    assert(!rb_exec_calls && !rb_legacy_calls && !rb_idle_writes && !rb_quota_writes);
    assert(!ledger_generation() && fixture_exists("/data/vendor/extm/state/pause"));
    fd_closed();
  }
  /* Global quota zero or one unit below the plan: wait, never reserve/refill or
   * pause. A later new cycle can proceed after the external authority refills. */
  for(int insufficient=0;insufficient<2;insufficient++) {
    struct nzm_core c;request_owner(&c);
    rb_global_enabled=1;rb_remaining=insufficient?1023:0;
    assert(!nzm_core_cycle(&c));
    assert(!strcmp(c.stage,"waiting_kernel_limit") && !c.reserved && !c.attempted);
    assert(!ledger_generation() && !rb_exec_calls && !rb_legacy_calls && !rb_quota_writes);
    assert(!fixture_exists("/data/vendor/extm/state/pause"));
    rb_remaining=1024;
    assert(!nzm_core_cycle(&c) && c.reserved && c.attempted && rb_exec_calls==1);
    assert(c.execution.result.known && c.execution.result.counters.submitted_pages==1024);
    assert(c.execution.result.counters.installed_pages==1 && !c.no_progress);
    assert(!readnum("/sys/block/zram0/bd_stat")); /* global delta zero, exact progress positive */
    assert(!rb_quota_writes && ledger_generation()==1);fd_closed();nzm_core_close(&c);
  }
  /* EFAULT copyout, ENOTTY/EINTR/EACCES EXEC, and bad echoes are ALL terminal.
   * Subsequent STATUS ENOTTY/config legacy/cycle/select cannot clear latch. */
  const int errors[]={EFAULT,ENOTTY,EINTR,EACCES,0};
  for(size_t i=0;i<sizeof errors/sizeof errors[0];i++) {
    struct nzm_core c;request_owner(&c);
    rb_exec_errno=errors[i];rb_bad_exec=!errors[i];
    assert(nzm_core_cycle(&c)<0 && c.execution.halted && c.reserved && c.attempted);
    assert(c.execution.result.no_replay && !c.execution.result.known);
    assert(rb_exec_calls==1 && !rb_legacy_calls && ledger_generation()==1 && !rb_quota_writes);
    assert(fixture_exists("/data/vendor/extm/state/pause"));
    unsigned statuses=rb_status_calls;
    rb_status_errno=ENOTTY;c.config.request_backend=NZM_REQUEST_LEGACY;
    assert(nzm_core_cycle(&c)<0 && nzm_execution_select(&c)<0);
    assert(rb_status_calls==statuses && rb_exec_calls==1 && !rb_legacy_calls);
    assert(c.reserved && c.attempted);fd_closed();nzm_core_close(&c);
  }
  request_fixture();rb_exec_errno=EFAULT;
  assert(stage3_start(1,0)==1 && rb_exec_calls==1 && !rb_legacy_calls);
  assert(stage3_start(1,0)==1 && rb_exec_calls==1); /* persistent pause on restart */
  puts("PASS stage3 EFAULT/effect, EXEC ENOTTY/EINTR/EACCES/bad reply: one call, persistent pause, retained charge, no replay");
  /* Durable intent closes the restart hole even if writing pause fails. */
  for(int partial=0;partial<2;partial++) {
    request_fixture();fail_pause_create=1;
    if(partial) {rb_op_errno=-EIO;rb_stop_reason=ZRAM_WB_STOP_ERROR;}
    else rb_exec_errno=EFAULT;
    assert(stage3_start(1,0)==1 && rb_exec_calls==1 && ledger_generation()==1);
    assert(fixture_exists("/data/vendor/extm/state/exec.intent"));
    assert(!fixture_exists("/data/vendor/extm/state/pause"));
    for(int legacy=0;legacy<2;legacy++) {
      file("/vendor/etc/lge-zram.conf",legacy?"request_backend=legacy\n":"request_backend=ioctl\n");
      assert(stage3_start(1,0)==1 && rb_exec_calls==1 && !rb_legacy_calls && ledger_generation()==1);
    }
  }
  for(int failure=1;failure<=4;failure++) {
    request_fixture();intent_fault=failure;
    assert(stage3_start(1,0)==1 && !rb_exec_calls && !rb_legacy_calls && !ledger_generation());
    assert(fixture_exists("/data/vendor/extm/state/exec.intent")== (failure!=1));
  }
  for(int failure=5;failure<=6;failure++) {
    struct nzm_core retained;request_owner(&retained);intent_fault=failure;fail_pause_create=1;
    assert(nzm_core_cycle(&retained)<0 && rb_exec_calls==1 && retained.execution.result.known);
    assert(!strcmp(retained.stage,"intent_retire_uncertain") && retained.execution.halted);
    assert(fixture_exists("/data/vendor/extm/state/exec.intent"));
    assert(nzm_core_cycle(&retained)<0 && rb_exec_calls==1);
    nzm_core_close(&retained);
  }
  request_fixture();assert(stage3_start(1,0)==1 && rb_exec_calls==1);
  assert(!fixture_exists("/data/vendor/extm/state/exec.intent") && intent_syncs==1);
  puts("PASS exec.intent: EFAULT/knownpartial + pause ENOSPC, restart legacy/ioctl blocked; create/shortwrite/fsync failure zero EXEC; knownsuccess retires");
  /* Precise no-progress uses installed, never submitted or global bd writes. */
  struct nzm_core c;request_owner(&c);rb_installed=0;
  assert(!nzm_core_cycle(&c) && c.no_progress && c.watermark==NZM_NO_PROGRESS);
  assert(c.execution.result.counters.submitted_pages==1024 && c.blocked_plan.pages==1024);
  assert(!nzm_core_cycle(&c) && rb_exec_calls==1 && ledger_generation()==1);
  nzm_core_close(&c);
  request_owner(&c);rb_op_errno=-EIO;rb_stop_reason=ZRAM_WB_STOP_ERROR;
  assert(nzm_core_cycle(&c)==-EIO && c.execution.result.known && c.execution.halted);
  assert(!strcmp(c.stage,"request_known_partial_no_refund") && !c.no_progress && ledger_generation()==1);
  nzm_core_close(&c);
  /* Request IDs are caller-local, not ledger generation. No zero/wrap. */
  request_owner(&c);
  char ledger[256];unsigned long long now=(unsigned long long)time(NULL);
  snprintf(ledger,sizeof ledger,"wear-v1 9 %llu 36\n",now);
  size_t n=strlen(ledger);for(int i=0;i<9;i++)n+=(size_t)snprintf(ledger+n,sizeof ledger-n,"%llu 4\n",now);
  file("/data/vendor/extm/state/ledger",ledger);
  assert(!nzm_core_cycle(&c) && rb_last_id==1 && ledger_generation()==10);
  monotonic_tick+=1000;assert(!nzm_core_cycle(&c) && rb_last_id==2 && ledger_generation()==11);
  c.execution.next_request_id=0;monotonic_tick+=1000;
  assert(nzm_core_cycle(&c)==-EOVERFLOW && rb_exec_calls==2 && ledger_generation()==11);
  nzm_core_close(&c);
  /* Invalid sampled statistics have bounded readonly re-sampling. Metadata,
   * labels, permissions and lifecycle failures never disappear in that loop. */
  for(int invalid=2;invalid<=3;invalid++) {
    request_owner(&c);rb_invalid_samples=invalid;unsigned before=rb_status_calls;
    int rc=nzm_core_cycle(&c);
    if(invalid==2)assert(!rc && rb_exec_calls==1 && rb_status_calls==before+5);
    else {
      assert(!rc && rb_exec_calls==0 && rb_status_calls==before+3 && !ledger_generation());
      assert(!strcmp(c.stage,"waiting_sample_inconsistent") && !c.reserved && !c.execution.halted);
      assert(!fixture_exists("/data/vendor/extm/state/pause") &&
             !fixture_exists("/data/vendor/extm/state/exec.intent"));
      assert(!nzm_core_cycle(&c) && rb_exec_calls==1 && ledger_generation()==1);
    }
    nzm_core_close(&c);
  }
  for(int identity=0;identity<6;identity++) {
    request_owner(&c);unsigned before=rb_status_calls;
    if(identity==0)rb_generation++;
    if(identity==1)rb_wrong_device=1;
    if(identity==2)rb_wrong_label=1;
    if(identity==3)rb_fail_open=1;
    if(identity==4)rb_status_errno=EACCES;
    if(identity==5)file("/sys/block/zram0/dev","0:2\n");
    assert(nzm_core_cycle(&c)<0 && !rb_exec_calls && !ledger_generation() && !c.reserved);
    assert(rb_status_calls==before+(identity==0?3:identity==4?1:0));
    assert(!rb_legacy_calls && c.execution.halted);nzm_core_close(&c);
  }
  /* STATUS page units bind actual kernel pages, not fixed 4KiB. Provision once
   * using legacy, then resume under the corresponding simulated host page. */
  for(uint64_t page=4096;page<=65536;page*=4) {
    request_fixture();nzm_core_init(&c);c.config.request_backend=NZM_REQUEST_LEGACY;
    assert(!nzm_core_boot(&c));nzm_core_close(&c);
    cycle_page=rb_status_page=page;
    nzm_core_init(&c);assert(!legacy_module_config(&c));assert(!nzm_core_boot(&c));
    c.boot_completed=1;watermark_probe=1;
    assert(!nzm_core_cycle(&c) && c.execution.page_size==page && c.last_plan.pages==4194304/page);
    assert(c.last_plan.reserve_kib==4096 && c.last_plan.limit_4k==1024 && !rb_quota_writes);
    fd_closed();nzm_core_close(&c);
  }
  request_owner(&c);c.config.enable_writeback=0;
  assert(!nzm_core_cycle(&c) && !rb_exec_calls && !rb_idle_writes && !ledger_generation());
  nzm_core_close(&c);
  /* Invalid scalar still fails before backend selection; retired PSI/event
   * cross-field policy is no longer a production parser constraint. */
  request_fixture();file("/vendor/etc/lge-zram.conf","psi_enabled=2\n");
  assert(stage3_start(1,0)==1 && !rb_status_calls && !rb_exec_calls && !rb_legacy_calls);
  /* Actual event-on main uses a separate thread-safe fixture, never these globals. */
  puts("PASS stage3 global-zero/insufficient wait before reservation, exact counts, partial errors, independent IDs, bounded samples, identity errors, 4/16/64K, disabled writeback/runtime");
}
