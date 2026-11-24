#define _GNU_SOURCE
#include "nzm_internal.h"
#include "recovery.h"
#include "log.h"
#include "state_bootstrap.h"
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

int nzm_diag(struct nzm_core *c, const char *stage, int rc) {
  if (!strcmp(stage,"query")) return rc;
  c->stage = stage;
  nzm_log("trigger=%s watermark=%d resident_bytes=%" PRIu64 " candidate_upper=%" PRIu64 " free_backing_pages=%" PRIu64 " plan_pages=%" PRIu64 " no_progress=%d budget_reserved=%d\n",
      c->trigger ? c->trigger : "none",c->watermark,c->last_plan.resident_bytes,
      c->last_plan.candidate_upper,c->last_plan.free_backing_pages,c->last_plan.pages,c->no_progress,c->reserved);
  int emit = 1;
  if (!rc && !strncmp(stage, "waiting_", 8)) {
    struct timespec t;
    if (!clock_gettime(CLOCK_MONOTONIC, &t)) {
      uint64_t tick = (uint64_t)t.tv_sec;
      if (c->last_wait_log &&
          tick >= c->last_wait_log &&
          tick - c->last_wait_log < c->config.wait_log_interval_seconds)
        emit = 0;
      else
        c->last_wait_log = tick;
    }
  }
  const struct rb_exec_result *result=&c->execution.result;
  const char *proof=result->known ? "response" : "unknown";
  char request[512];
  snprintf(request,sizeof request,"enabled_backend=%s exact_request_proof=%s "
      "request_id=%" PRIu64 " submitted_pages=%" PRIu64 " installed_pages=%" PRIu64
      " counters_known=%d op_errno=%d stop_reason=%u error_stage=%u no_replay=%d close_errno=%d",
      nzm_execution_name(c),proof,result->request_id,result->counters.submitted_pages,
      result->counters.installed_pages,result->known,result->op_errno,result->stop_reason,
      result->error_stage,result->no_replay,result->transport.close_errno);
  if (emit)
    nzm_log(
            "stage=%s errno=%d reserved=%d attempted=%d "
            "observed_global_before=%" PRIu64 " observed_global_after=%" PRIu64
            " observed_before_valid=%d observed_after_valid=%d "
            "%s\n",
            stage, rc < 0 ? -rc : rc, c->reserved, c->attempted,
            c->observed_before, c->observed_after, c->observed_before_valid,
            c->observed_after_valid,request);
  /* Ordinary events are process-local. Startup checkpoints alone are durable. */
  if (emit) {
    snprintf(c->last_event, sizeof c->last_event,
        "stage=%s errno=%d reserved=%d attempted=%d %s\n",
        stage, rc < 0 ? -rc : rc, c->reserved, c->attempted, request);
    c->snapshot_errno = 0;
  }
  return rc;
}
int nzm_diag_config_error(const char *stage, const char *field, const char *reason, int rc) {
  nzm_log("config_stage=%s field=%s reason=%s errno=%d "
          "expected_uid=0 expected_gid=0 expected_mode=0644 "
          "expected_label=u:object_r:vendor_configs_file:s0\n",
          stage,field,reason,-rc);
  return rc;
}
static int persist_pause(struct nzm_core *c) {
  if (c->wearfd < 0) {
    int opened = nzm_state_open(&c->startupfd, &c->wearfd);
    if (opened) return opened;
  }
  int rc = nzm_state_bound(c->startupfd, c->wearfd);
  if (!rc) rc = nzm_create_text(c->wearfd, "pause", PCTX, NZM_PAUSE_RECORD);
  return rc == -EEXIST ? 0 : rc;
}
int nzm_core_pause(struct nzm_core *c, const char *reason) {
  int rc = persist_pause(c);
  c->pause_errno = rc < 0 ? -rc : rc;
  return nzm_diag(c, reason, rc);
}
int nzm_stop(struct nzm_core *c, const char *stage, int rc) {
  c->execution.halted=1;
  int pause_rc = persist_pause(c);
  c->pause_errno = pause_rc < 0 ? -pause_rc : pause_rc;
  /* Final event always preserves the cause, not successful pause's errno=0. */
  return nzm_diag(c, stage, rc ? rc : -EPERM);
}
int nzm_waiting(struct nzm_core *c, const char *reason) {
  return nzm_diag(c,reason,0);
}
void nzm_config_snapshot(const struct nzm_core *c, FILE *out, int startup) {
  fprintf(out,"config_source=/vendor/etc/lge-zram.conf scope=%s "
        "running_daemon_config=%s reload=unsupported omitted_keys=defaults\n",
        startup ? "startup_loaded" : "disk_snapshot",startup ? "this_process" : "unknown");
  fprintf(out,"request_backend=%s enabled_backend=%s\n",
      nzm_request_backend_name(c->config.request_backend),nzm_execution_name(c));
#define VALUE(key, unit) fprintf(out, #key "=%" PRIu64 " unit=" unit "\n", c->config.key)
  VALUE(pool_low_mib, "MiB");
  VALUE(pool_high_mib, "MiB");
  VALUE(cold_age_seconds, "seconds");
  VALUE(demand_batch_mib, "MiB");
  VALUE(demand_enabled, "boolean");
  VALUE(demand_scan_limit, "entries");
  VALUE(enable_writeback, "boolean");
  VALUE(backing_mib, "MiB");
  VALUE(disksize_bytes, "bytes_request_only");
  VALUE(data_wait_seconds, "seconds");
  VALUE(vm_swappiness, "integer");
  VALUE(vm_page_cluster, "integer");
  VALUE(vm_watermark_scale_factor, "integer");
  VALUE(vm_extra_free_kbytes, "KiB");
  VALUE(vm_min_free_kbytes, "KiB");
  VALUE(vm_minfree_mult, "multiplier");
  VALUE(vm_overcommit_memory, "integer");
  VALUE(vm_laptop_mode, "seconds");
  VALUE(wait_log_interval_seconds, "seconds");
  VALUE(daily_mib, "MiB/rolling_86400_seconds");
  VALUE(min_data_free_mib, "MiB");
#undef VALUE
  fputs("legacy_scheduling_keys=deprecated_ignored legacy_fallback=disabled\n", out);
}
void nzm_core_config_snapshot(const struct nzm_core *c, FILE *out) {
  nzm_config_snapshot(c,out,0);
}
int nzm_core_status(struct nzm_core *c, FILE *out) {
  nzm_core_config_snapshot(c, out);
  /* Deliberately no start(), scan, helper, marker creation or lock. */
  fprintf(out,
          "native-v1 readonly=1 helper=not-executed precise_idle=unsupported "
          "exact_request_proof=unknown\nbacking_mib=%" PRIu64
          " batch_mib=%" PRIu64 " daily_mib=%" PRIu64 "\n",
          c->config.backing_mib, c->config.batch_mib, c->config.daily_mib);
  fputs("budget_scope=process_local reset=controller_object_restart "
        "running_daemon_budget=unknown\n"
        "budget=unknown history=running_controller_only historical_ledger=ignored\n"
        "budget_policy=rolling_86400_seconds time_basis=CLOCK_BOOTTIME "
        "bucket_seconds=900 completed_expiry=86400_seconds_after_bucket_end\n"
        "accounting=submitted_when_known unknown=full_reservation "
        "known_zero=refund_unused unknown_halts=controller_object "
        "charge_time=completion pending_expiry=never\n", out);
  int paused = 0;
  int rc = nzm_core_bootid(c);
  if (!rc) rc = nzm_data_verify();
  int opened = 0;
  if (!rc && c->wearfd < 0) {
    c->wearfd = nzm_state_readonly();
    if (c->wearfd < 0) rc = c->wearfd;
    else opened = 1;
  }
  if (!rc) rc = nzm_phase1_policy(c, &paused);
  if (rc)
    fprintf(out, "demand_authority=unknown errno=%d\n", -rc);
  else
    fprintf(out, "demand_authority=%s reason=%s\n",
        paused ? "blocked" : "valid",
        paused ? "persistent_pause_or_pending_evidence" : "authority_verified");
  char b[1024];
  struct nzm_boot_state state;
  nzm_boot_init(&state);
  int r = rc ? rc : nzm_boot_load(&state, c->wearfd);
  if (!r) r = nzm_boot_text(&state, "managed", b, sizeof b);
  if (r) fprintf(out, "managed_checkpoint=unknown errno=%d\n", -r);
  else fprintf(out, "managed_checkpoint=%s", b);
  fputs("last_event=process_local running_daemon_event=unknown\n", out);
  const char *nodes[] = {"disksize", "backing_dev", "bd_stat",
                         "writeback_limit", "writeback_limit_enable",
                         "pool_policy", "pool_demand"};
  for (size_t i = 0; i < sizeof nodes / sizeof nodes[0]; i++) {
    char p[128];
    snprintf(p, sizeof p, Z "%s", nodes[i]);
    r = nzm_rawread(p, b, sizeof b);
    if (r)
      fprintf(out, "%s=unknown errno=%d\n", nodes[i], -r);
    else
      fprintf(out, "%s=%s", nodes[i], b);
  }
  if (opened) { close(c->wearfd); c->wearfd = -1; }
  return 0;
}
