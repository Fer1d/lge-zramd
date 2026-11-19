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

/* Phase 1 never authorizes a physical backend. A verified pause dominates
 * persistent pending records; without pause require normal wear validation.
 * Neither branch provisions, repairs, clears, or reserves anything. */
int nzm_phase1_policy(struct nzm_core *c, int *paused) {
  int rc = nzm_pause_verified(c, paused);
  if (rc || *paused)
    return rc;
  struct nzm_ledger l = {0};
  rc = nzm_wear(c, &l);
  nzm_ledger_free(&l);
  return rc;
}

int nzm_core_workload(const struct nzm_config *cfg, uint64_t page,
                      const uint64_t mm[10], const uint64_t bd[3],
                      uint64_t *upper) {
  struct nzm_plan p;
  enum nzm_watermark_state state=NZM_ARMED;
  /* Compatibility test API; runtime always samples actual disksize. */
  int rc=nzm_plan_writeback(cfg,page,mm[0] ? mm[0] : page,mm,bd,1,&state,&p);
  if(!rc || rc==-EAGAIN)*upper=p.candidate_upper;
  return rc;
}
int nzm_elapsed(uint64_t now,uint64_t since,uint64_t interval) {
  return now>=since && now-since>=interval;
}
static int grew(uint64_t now,uint64_t base,uint64_t minimum) {
  return now>=base && now-base>=minimum;
}
int nzm_sample_plan(struct nzm_core *c, struct nzm_cycle_policy *policy, struct nzm_plan *plan) {
  uint64_t mm[10],bd[3],disk,tick,page;
  int rc;
  if((rc=nzm_monotonic_now(&tick)))return rc;
  if(!c->timer_valid) { c->timer_since=tick;c->timer_valid=1; }
  if((rc=nzm_execution_sample(c,mm,bd,&disk,&page)))return rc;
  enum nzm_watermark_state state=c->watermark;
  if(state==NZM_NO_PROGRESS)state=c->watermark_resume;
  int off,charging;
  if((rc=nzm_power_policy(&off,&charging)))return rc;
  struct nzm_config cfg=c->config;
  cfg.batch_mib=charging ? cfg.watermark_charging_batch_mib : cfg.watermark_battery_batch_mib;
  struct nzm_plan wm,any;
  rc=nzm_plan_writeback(&cfg,page,disk,mm,bd,0,&state,&wm);
  if(rc && rc!=-EAGAIN)return rc;
  int wm_active=c->config.watermark_trigger_enabled && state==NZM_DRAINING;
  enum nzm_watermark_state dummy=NZM_ARMED;
  rc=nzm_plan_writeback(&c->config,page,disk,mm,bd,1,&dummy,&any);
  if(rc && rc!=-EAGAIN)return rc;
  c->watermark=state;c->watermark_resume=state;
  policy->watermark=wm_active;
  policy->idle=wm_active ? (charging ? cfg.watermark_charging_idle_observation_seconds : cfg.watermark_battery_idle_observation_seconds) : cfg.idle_observation_seconds;
  policy->interval=wm_active ? (charging ? cfg.watermark_charging_min_interval_seconds : cfg.watermark_battery_min_interval_seconds) : cfg.time_trigger_interval_seconds;
  c->sampled_interval=policy->interval;c->sampled_policy_valid=1;
  c->trigger=wm_active ? "watermark" : "time";
  *plan=wm_active ? wm : any;c->last_plan=*plan;
  /* An active watermark owns arbitration even when throttled/empty. Never
   * relabel it time to escape its smaller batch or minimum interval. */
  if(!wm_active && (!off || !charging || !cfg.time_trigger_enabled ||
      !nzm_elapsed(tick,c->timer_since,cfg.time_trigger_interval_seconds)))return -EAGAIN;
  if(c->last_attempt_valid && !nzm_elapsed(tick,c->last_attempt_tick,policy->interval))return -EAGAIN;
  if(!plan->pages)return -EAGAIN;
  if(c->no_progress) {
    const struct nzm_plan *b=&c->blocked_plan;
    uint64_t need=b->pages;
    int change=grew(any.candidate_upper,b->candidate_upper,need) ||
        grew(any.resident_bytes,b->resident_bytes,need*page) ||
        grew(any.free_backing_pages,b->free_backing_pages,need);
    if(!nzm_elapsed(tick,c->no_progress_since,c->config.no_progress_retry_seconds) || !change) {
      c->watermark=NZM_NO_PROGRESS;return -EAGAIN;
    }
    /* Keep the latch until actual submission; observation is not a retry. */
  }
  return 0;
}
