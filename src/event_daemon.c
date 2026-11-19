#define _GNU_SOURCE
#include "nzm_internal.h"
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef NZM_CORE_ADAPTER
#include NZM_CORE_ADAPTER
#endif
#define SECOND_NS UINT64_C(1000000000)

/* Main owns only immutable launch config and runtime handle. Everything in
 * core (including partial initialization and teardown) belongs to the worker.
 * runtime publishes exited only AFTER finalize, then joins, then closes its FDs. */
#ifdef NZM_LEGACY_TEST
struct event_owner {
  struct nzm_config launch;
  int trusted, initialized, terminal;
  struct nzm_core core;
  struct nzm_runtime *runtime;
};
#endif
int nzm_core_control(struct nzm_core *c, enum nzm_runtime_boundary boundary) {
  if(!c->control)return 0;
#ifdef NZM_LEGACY_TEST
  int action=nzm_runtime_control_check(c->control,boundary);
  if(action<0)return action;
  if(!action)return 0;
  c->cooperative_stop=1;
  return nzm_diag(c,action==NZM_CONTROL_STOP_RETAIN_RESERVATION ?
      "stopped_retain_reservation" : "stopped_before_commit",-ECANCELED);
#else
  (void)boundary;
  return -EOPNOTSUPP;
#endif
}
#ifdef NZM_LEGACY_TEST
int nzm_core_observe(struct nzm_core *c, uint64_t seconds) {
  if(!c->control)return nzm_sleep_seconds(seconds);
  int rc=nzm_core_control(c,NZM_BOUNDARY_OBSERVING);
  if(rc)return rc;
  /* Preserve uint64 legacy idle domain; bounded absolute monotonic chunks avoid
   * overflow. Notifications/spurious wakeups never extend any chunk deadline. */
  while(seconds) {
    uint64_t chunk=seconds>86400 ? 86400 : seconds;
    struct timespec now,deadline;
    if(clock_gettime(CLOCK_MONOTONIC,&now))return -errno;
    if((rc=nzm_runtime_deadline_after(&now,chunk*SECOND_NS,&deadline)))return rc;
    rc=nzm_runtime_control_wait_until(c->control,NZM_BOUNDARY_OBSERVING,&deadline);
    if(rc<0)return rc;
    if(rc)return nzm_core_control(c,NZM_BOUNDARY_OBSERVING);
    seconds-=chunk;
  }
  return nzm_core_control(c,NZM_BOUNDARY_OBSERVING);
}
static uint64_t remaining(uint64_t now,uint64_t since,uint64_t interval) {
  if(now<since)return interval; /* rollback never makes an immediate job */
  uint64_t elapsed=now-since;
  return elapsed>=interval ? 0 : interval-elapsed;
}
int nzm_event_next_result(const struct nzm_core *c, uint64_t now,
                          struct nzm_runtime_result *result) {
  const struct nzm_config *cfg=&c->config;
  uint64_t maximum=cfg->runtime_max_resync_seconds;
  if(!maximum || maximum>INT64_MAX/SECOND_NS ||
     !cfg->runtime_active_sample_seconds ||
     cfg->runtime_idle_sample_seconds<cfg->runtime_active_sample_seconds ||
     maximum<cfg->runtime_idle_sample_seconds ||
     !cfg->runtime_health_retry_seconds || cfg->runtime_health_retry_seconds>INT64_MAX/SECOND_NS)
    return -EINVAL;
  uint64_t delay=0;
  int health=c->stage && (!strcmp(c->stage,"waiting_safety_or_policy") ||
      !strcmp(c->stage,"waiting_kernel_limit") || !strcmp(c->stage,"waiting_budget") ||
      !strcmp(c->stage,"waiting_sample_inconsistent"));
  int watermark=c->trigger && !strcmp(c->trigger,"watermark");
  result->active=watermark && c->watermark==NZM_DRAINING && !c->no_progress && !health;
  if(c->no_progress)delay=remaining(now,c->no_progress_since,cfg->no_progress_retry_seconds);
  if(!watermark && cfg->time_trigger_enabled && c->timer_valid) {
    uint64_t due=remaining(now,c->timer_since,cfg->time_trigger_interval_seconds);
    if(due>delay)delay=due;
  }
  if(c->last_attempt_valid && c->sampled_policy_valid) {
    uint64_t due=remaining(now,c->last_attempt_tick,c->sampled_interval);
    if(due>delay)delay=due; /* Both policy gates must expire, not just either one. */
  }
  if(health && cfg->runtime_health_retry_seconds>delay)delay=cfg->runtime_health_retry_seconds;
  if(!delay)delay=result->active ? cfg->runtime_active_sample_seconds : cfg->runtime_idle_sample_seconds;
  if(delay>maximum)delay=maximum; /* Finite read-only resync even if policy is far away. */
  result->next_delay_ns=delay*SECOND_NS;
  return 0;
}
static int owner_initialize(void *context,const struct nzm_runtime_control *ctl) {
  struct event_owner *owner=context;
  struct nzm_core *c=&owner->core;
  nzm_core_init(c);owner->initialized=1;
  c->config=owner->launch;c->trusted_boot_completed=owner->trusted;c->control=ctl;
  int rc=nzm_runtime_control_get_exec_mask(ctl,&c->helper_exec_mask);
  if(rc)return rc;
  c->helper_exec_mask_valid=1;
  fputs("config_scope=startup_loaded next_boot_only=1 reload=unsupported runtime_owner=worker\n",stderr);
  nzm_config_snapshot(c,stderr,1);
  if((rc=nzm_core_control(c,NZM_BOUNDARY_OBSERVING)))return c->cooperative_stop ? 0 : rc;
  rc=nzm_core_ready(c);
  return c->cooperative_stop ? 0 : rc;
}
static void owner_execute(void *context,const struct nzm_runtime_job *job,
                           const struct nzm_runtime_control *ctl,
                           struct nzm_runtime_result *result) {
  struct event_owner *owner=context;
  struct nzm_core *c=&owner->core;
  (void)job;(void)ctl; /* Reasons wake sampling; they NEVER bypass policy. */
  int rc=owner->terminal ? owner->terminal : nzm_core_cycle(c);
  if(rc || !c->config.enable_writeback) {
    if(rc && !c->cooperative_stop)owner->terminal=rc;
    result->code=c->cooperative_stop ? 0 : rc;
    (void)nzm_runtime_request_stop(owner->runtime);
    return;
  }
  uint64_t now;
  rc=nzm_monotonic_now(&now);
  if(!rc)rc=nzm_event_next_result(c,now,result);
  if(rc) {
    owner->terminal=nzm_stop(c,"runtime_deadline",rc);result->code=rc;
    (void)nzm_runtime_request_stop(owner->runtime);
  }
}
static void owner_finalize(void *context,const struct nzm_runtime_control *ctl) {
  struct event_owner *owner=context;(void)ctl;
  if(owner->initialized) {
    nzm_core_close(&owner->core);owner->initialized=0;
  }
}
int nzm_event_daemon(const struct nzm_config *config,int trusted_boot_completed) {
  if(!config || !config->runtime_event_enabled)return -EINVAL;
  struct event_owner *owner=calloc(1,sizeof *owner);
  if(!owner)return -ENOMEM;
  owner->launch=*config;owner->trusted=trusted_boot_completed;
  struct nzm_runtime_options options;
  nzm_runtime_options_default(&options);
  /* Parse checks conversion; also validate programmatic callers before multiply. */
  struct nzm_core validation={.config=*config};struct nzm_runtime_result check={0};
  int rc=nzm_event_next_result(&validation,0,&check);
  if(rc || config->psi_threshold_us>UINT32_MAX || config->psi_window_us>UINT32_MAX) {
    free(owner);return rc ? rc : -EINVAL;
  }
  options.idle_sample_interval_ns=config->runtime_idle_sample_seconds*SECOND_NS;
  options.active_sample_interval_ns=config->runtime_active_sample_seconds*SECOND_NS;
  options.max_fallback_interval_ns=config->runtime_max_resync_seconds*SECOND_NS;
  options.psi.enabled=config->psi_enabled!=0;
  options.psi.threshold_us=(uint32_t)config->psi_threshold_us;
  options.psi.window_us=(uint32_t)config->psi_window_us;
  struct nzm_runtime_executor executor={owner,owner_initialize,owner_execute,owner_finalize};
  struct nzm_runtime *runtime=NULL;
  rc=nzm_runtime_create(&runtime,&options,&executor);
  if(rc) {free(owner);return rc;} /* No worker initialization, no core close. */
  owner->runtime=runtime; /* Immutable before run creates the worker. */
  rc=nzm_runtime_run(runtime);
  struct nzm_runtime_stats stats;
  int stats_rc=nzm_runtime_get_stats(runtime,&stats);
  if(stats_rc) {
    /* Join/publication failure: retain owner+runtime, do NOT free live storage
     * or claim teardown complete. Main receives an error and must not close it. */
    fprintf(stderr,"runtime_teardown=unconfirmed errno=%d ownership=retained\n",-stats_rc);
    return rc ? rc : stats_rc;
  }
  fprintf(stderr,"runtime_worker_joined=%d jobs=%" PRIu64 " completed=%" PRIu64
      " last_delay_ns=%" PRIu64 " psi_fallbacks=%" PRIu64 " psi_errno=%d hup_unsupported=%" PRIu64 "\n",
      stats.worker_joined,stats.jobs_started,stats.jobs_completed,stats.last_delay_ns,
      stats.psi_fallbacks,stats.psi_error,stats.hup_unsupported);
  if(!rc && stats.last_result_code)rc=stats.last_result_code;
  int destroy_rc=nzm_runtime_destroy(runtime);
  if(destroy_rc)return rc ? rc : destroy_rc; /* retain until teardown confirmed */
  free(owner);
  return rc;
}
#endif /* NZM_LEGACY_TEST */
