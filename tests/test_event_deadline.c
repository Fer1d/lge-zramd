#define _GNU_SOURCE
#include "nzm_core.h"
#include "runtime.h"
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <string.h>
#define NS UINT64_C(1000000000)
static uint64_t delay(struct nzm_core *c,uint64_t now,int active) {
  struct nzm_runtime_result result={0};
  assert(!nzm_event_next_result(c,now,&result));assert(result.active==!!active);
  struct nzm_runtime_options options;
  nzm_runtime_options_default(&options);
  options.active_sample_interval_ns=c->config.runtime_active_sample_seconds*NS;
  options.idle_sample_interval_ns=c->config.runtime_idle_sample_seconds*NS;
  options.max_fallback_interval_ns=c->config.runtime_max_resync_seconds*NS;
  return nzm_runtime_next_delay(&options,&result)/NS;
}
int main(void) {
  struct nzm_core c={0};nzm_config_default(&c.config);
  assert(!c.config.runtime_event_enabled && !c.config.psi_enabled);
  assert(c.config.request_backend==NZM_REQUEST_AUTO);
  assert(c.config.psi_threshold_us==500000 && c.config.psi_window_us==10000000);
  c.trigger="time";c.timer_valid=1;c.timer_since=100;
  assert(delay(&c,100,0)==60); /* 300s time remaining bounded by resync */
  assert(delay(&c,345,0)==55 && delay(&c,390,0)==30);
  assert(delay(&c,99,0)==60); /* rollback does not manufacture expiry */
  c.config.time_trigger_interval_seconds=UINT64_MAX;
  assert(delay(&c,UINT64_MAX,0)==60); /* subtraction, no now+interval overflow */
  c.config.time_trigger_interval_seconds=300;
  c.trigger="watermark";c.watermark=NZM_DRAINING;
  c.last_attempt_valid=c.sampled_policy_valid=1;c.last_attempt_tick=100;c.sampled_interval=30;
  assert(delay(&c,100,1)==30 && delay(&c,129,1)==2);
  c.no_progress=1;c.watermark=NZM_NO_PROGRESS;c.no_progress_since=200;
  assert(delay(&c,700,0)==60 && delay(&c,750,0)==50 && delay(&c,799,0)==30);
  c.no_progress=0;c.watermark=NZM_DRAINING;c.stage="waiting_safety_or_policy";
  c.config.runtime_health_retry_seconds=45;
  assert(delay(&c,900,0)==45);
  c.stage="waiting_kernel_limit";assert(delay(&c,900,0)==45);
  c.stage="waiting_budget";assert(delay(&c,900,0)==45);
  const char *bad[]={
    "runtime_idle_sample_seconds=0\n", "runtime_active_sample_seconds=0\n",
    "runtime_max_resync_seconds=0\n", "runtime_health_retry_seconds=0\n",
    "runtime_active_sample_seconds=9223372037\n", "runtime_idle_sample_seconds=9223372037\n",
    "runtime_max_resync_seconds=9223372037\n", "psi_enabled=2\n",
    "psi_threshold_us=0\n", "psi_window_us=499999\n", "psi_window_us=10000001\n",
    "psi_threshold_us=10000001\n", "psi_window_us=500000\npsi_window_us=500000\n",
    "runtime_idle_sample_seconds=30\nruntime_idle_sample_seconds=30\n",
    "runtime_active_sample_seconds=2\r\n", "runtime_max_resync_seconds=60 ",
    "runtime_health_retry_seconds=9223372037\n"
  };
  struct nzm_config config,original;nzm_config_default(&original);
  for(size_t i=0;i<sizeof bad/sizeof bad[0];i++) {
    config=original;assert(nzm_config_parse(bad[i],strlen(bad[i]),&config)<0);
    assert(!memcmp(&config,&original,sizeof config));
  }
  char text[512];uint64_t bound=INT64_MAX/NS;
  snprintf(text,sizeof text,"runtime_event_enabled=1\npsi_enabled=1\nruntime_idle_sample_seconds=%" PRIu64
      "\nruntime_active_sample_seconds=%" PRIu64 "\nruntime_max_resync_seconds=%" PRIu64
      "\nruntime_health_retry_seconds=%" PRIu64 "\n",bound,bound,bound,bound);
  assert(!nzm_config_parse(text,strlen(text),&config));
  /* Live parsing validates but ignores retired scheduling scalars. Exercise the
   * retained deadline module with an explicitly constructed legacy config. */
  assert(!memcmp(&config,&original,sizeof config));
  c.config=config;
  c.config.runtime_event_enabled=c.config.psi_enabled=1;
  c.config.runtime_idle_sample_seconds=bound;
  c.config.runtime_active_sample_seconds=bound;
  c.config.runtime_max_resync_seconds=bound;
  c.config.runtime_health_retry_seconds=bound;
  c.no_progress=0;c.stage=NULL;
  assert(delay(&c,900,1)==bound);
  c.config.runtime_max_resync_seconds=UINT64_MAX;
  struct nzm_runtime_result result={0};assert(nzm_event_next_result(&c,900,&result)==-EINVAL);
  puts("PASS event deadlines/config: real time/NP/shared interval/health gates, finite resync, clamp, rollback/u64 conversion, strict fields, defaults off/off/auto");
  return 0;
}
