#define _GNU_SOURCE
#include "nzm_internal.h"
#include "recovery.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/fs.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/statvfs.h>
#include <sys/swap.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
#ifdef NZM_CORE_ADAPTER
#include NZM_CORE_ADAPTER
#endif

int nzm_boundary(struct nzm_core *c) {
  struct nzm_ledger l = {0};
  int rc = nzm_wear(c, &l);
  nzm_ledger_free(&l);
  if (rc)
    return rc;
  return nzm_managed(c);
}
int nzm_core_cycle(struct nzm_core *c) {
  /* Process latch survives even failed persistent-pause writes. Never clear an
   * unknown EXEC by calling cycle/select again, or changing config to legacy. */
  if(c->execution.halted)return -EPERM;
  c->reserved = c->attempted = 0;
  memset(&c->execution.result,0,sizeof c->execution.result);
  c->observed_before = c->observed_after = 0;
  c->observed_before_valid = c->observed_after_valid = 0;
  if(!c->boot_completed)return nzm_waiting(c,"waiting_boot_completed");
  if(!c->config.enable_writeback)return nzm_waiting(c,"waiting_writeback_disabled");
  if(!c->execution.selected || c->execution.backend==RB_BACKEND_UNAVAILABLE)
    return nzm_stop(c,"backend_unselected",-EPERM);
  int rc=nzm_core_control(c,NZM_BOUNDARY_OBSERVING);
  if(rc)return rc;
  struct nzm_cycle_policy initial_policy, policy;
  /* Cheap readonly sampling on idle ticks: no helper subprocess or ledger scan.
   * Full provenance/ledger/identity before any write and after actual request. */
  struct nzm_plan initial,plan;
  rc=nzm_sample_plan(c,&initial_policy,&initial);
  if(rc==-EAGAIN && c->execution.sample_inconsistent)return nzm_waiting(c,"waiting_sample_inconsistent");
  if(rc==-EAGAIN)return nzm_waiting(c,c->watermark==NZM_NO_PROGRESS ?
      "waiting_watermark_no_progress_process_only" : "waiting_watermark");
  if(rc)return nzm_stop(c,"sample_unknown",rc);
  rc=nzm_health(c);
  if(rc==-EAGAIN || rc==-ENOSPC)return nzm_waiting(c,"waiting_safety_or_policy");
  if(rc)return nzm_stop(c,"health_unknown",rc);
  if((rc=nzm_boundary(c)))return nzm_stop(c,"boundary",rc);
  if((rc=nzm_put(Z "idle","all\n")))return nzm_stop(c,"idle_mark",rc);
  if((rc=nzm_core_observe(c,initial_policy.idle)))
    return c->cooperative_stop ? rc : nzm_stop(c,"idle_observation_interrupted",rc);
  if((rc=nzm_boundary(c)))return nzm_stop(c,"post_idle_boundary",rc);
  rc=nzm_health(c);
  if(rc==-EAGAIN || rc==-ENOSPC)return nzm_waiting(c,"waiting_safety_or_policy");
  if(rc)return nzm_stop(c,"post_idle_health",rc);
  rc=nzm_sample_plan(c,&policy,&plan);
  if(rc==-EAGAIN && c->execution.sample_inconsistent)return nzm_waiting(c,"waiting_sample_inconsistent");
  if(rc==-EAGAIN)return nzm_waiting(c,c->no_progress ? "waiting_watermark_no_progress_process_only" : "waiting_watermark");
  if(rc)return nzm_stop(c,"post_idle_plan",rc);
  /* Intersection, never upgrade a prepared operation or switch tracks. */
  uint64_t tick;
  if((rc=nzm_monotonic_now(&tick)))return nzm_stop(c,"policy_clock",rc);
  uint64_t interval=policy.interval > initial_policy.interval ? policy.interval : initial_policy.interval;
  if(policy.watermark!=initial_policy.watermark || policy.idle>initial_policy.idle ||
      (c->last_attempt_valid && !nzm_elapsed(tick,c->last_attempt_tick,interval)))
    return nzm_waiting(c,"waiting_policy_changed");
  if(plan.pages>initial.pages) {
    plan.pages=initial.pages;
    uint64_t page=(uint64_t)sysconf(_SC_PAGESIZE);
    plan.reserve_kib=plan.pages*(page/1024);plan.limit_4k=plan.pages*(page/4096);
  }
  c->last_plan=plan;
  /* STATUS's enabled global quota is external authority, never set/refilled by
   * ioctl execution. Insufficient quota waits BEFORE the durable precharge. */
  rc=nzm_execution_precheck(c,&plan);
  if(rc==-EAGAIN)return nzm_waiting(c,"waiting_kernel_limit");
  if(rc)return nzm_stop(c,"pre_reservation_status",rc);
  /* No precharge exists during observation. Final plan is authoritative. */
  struct nzm_ledger l={0}; char *s=NULL; size_t n=0;
  rc=nzm_wear(c,&l);
  if(!rc)rc=nzm_ledger_reserve_text(&l,nzm_nowtime(),plan.reserve_kib,
                                  c->config.daily_mib*1024,&s,&n);
  nzm_ledger_free(&l);
  if(rc==-EDQUOT)return nzm_waiting(c,"waiting_budget");
  if(rc)return nzm_stop(c,"reservation",rc);
  if((rc=nzm_core_control(c,NZM_BOUNDARY_PRECOMMIT))) {free(s);return rc;}
  rc=nzm_intent_create(c,&plan);
  if(rc) {free(s);return nzm_stop(c,"exec_intent_create",rc);}
  /* Intent fsync can block: recheck stop at the actual precharge boundary.
   * An already-created intent remains evidence even with zero ledger charge. */
  if((rc=nzm_core_control(c,NZM_BOUNDARY_PRECOMMIT))) {free(s);return rc;}
  rc=nzm_io_ledger_commit(c->wearfd,s,n,WCTX,LCTX);free(s);
  if(rc)return nzm_stop(c,"reservation_uncertain",rc);
  c->reserved=1;
  if((rc=nzm_core_control(c,NZM_BOUNDARY_POSTPRECHARGE)))return rc;
  if(c->execution.backend==RB_BACKEND_REQUEST)
    return nzm_execution_execute(c,&plan);
  /* Legacy retains its original sysfs quota/readback and global-delta logic. */
  uint64_t limit=plan.limit_4k,v,io[4],after[4],bd[3],before_backed;
  if((rc=nzm_boundary(c)) || (rc=nzm_putnum(Z "writeback_limit_enable",1)) ||
      (rc=nzm_putnum(Z "writeback_limit",limit)))return nzm_stop(c,"limit",rc);
  if((rc=nzm_number(Z "writeback_limit_enable",&v)) || v!=1)
    return nzm_stop(c,"limit_enable_readback",rc ? rc : -EIO);
  if((rc=nzm_number(Z "writeback_limit",&v)) || v!=limit)
    return nzm_stop(c,"limit_readback",rc ? rc : -EIO);
  if((rc=nzm_fields(Z "io_stat",io,4)) || (rc=nzm_fields(Z "bd_stat",bd,3)))
    return nzm_stop(c,"snapshot",rc);
  before_backed=bd[0];c->observed_before=bd[0];c->observed_before_valid=1;
  if((rc=nzm_boundary(c)))return nzm_stop(c,"pre_attempt_boundary",rc);
  char req[80];snprintf(req,sizeof req,"idle %" PRIu64 " 1\n",plan.pages);
  uint64_t submit_tick;
  int fd=open(Z "writeback",O_WRONLY|O_NOFOLLOW|O_CLOEXEC);
  if(fd<0)return nzm_stop(c,"attempt_open",-errno);
  if((rc=nzm_core_control(c,NZM_BOUNDARY_SUBMITTING))) {close(fd);return rc;}
  if((rc=nzm_monotonic_now(&submit_tick))) {close(fd);return nzm_stop(c,"submit_clock",rc);}
  c->timer_since=submit_tick;c->timer_valid=1;
  c->last_attempt_tick=submit_tick;c->last_attempt_valid=1;
  c->attempted=1;
  rc=nzm_io_sysfs_write(fd,req,strlen(req));
  close(fd);
  if(rc)return nzm_stop(c,"attempt_uncertain_no_refund",rc);
  if((rc=nzm_fields(Z "io_stat",after,4)) || (rc=nzm_fields(Z "bd_stat",bd,3)))
    return nzm_stop(c,"post_snapshot",rc);
  c->observed_after=bd[0];c->observed_after_valid=1;
  if(after[0]!=io[0] || after[1]!=io[1])return nzm_stop(c,"io_error_changed",-EIO);
  if((rc=nzm_number(Z "writeback_limit",&v)) || v>limit)
    return nzm_stop(c,"remaining_unknown",rc ? rc : -EIO);
  if((rc=nzm_boundary(c)))return nzm_stop(c,"post_attempt_identity",rc);
  c->no_progress=bd[0]<=before_backed;
  if(c->no_progress) { c->blocked_plan=plan;c->no_progress_since=submit_tick; }
  c->watermark_resume=c->watermark;
  if((rc=nzm_watermark_observe(&c->watermark,before_backed,bd[0])))
    return nzm_stop(c,"backed_count_unknown",rc);
  if (bd[0]<before_backed)
    return nzm_diag(c,"waiting_watermark_backed_decreased_no_refund_process_only",0);
  return nzm_diag(c,c->watermark==NZM_NO_PROGRESS ? "waiting_watermark_no_progress_process_only" :
      "watermark_backed_count_increased_no_refund",0);
}
