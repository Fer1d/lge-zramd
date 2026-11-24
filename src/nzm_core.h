#ifndef NZM_CORE_H
#define NZM_CORE_H
#include "nzm.h"
#include "planner.h"
#include "nzm_io.h"
#include "request_backend.h"
#include "boot_state.h"
#include "memory_budget.h"
#include <stdio.h>
/* Single owner only. Eventloop never accesses the worker's core or FDs. */
struct nzm_runtime_control;
struct nzm_runtime_result;
struct nzm_execution {
  int selected, halted, sample_inconsistent;
  enum rb_backend_selection backend;
  struct rb_backend transport;
  struct rb_status_result status;
  struct rb_identity identity;
  uint32_t page_size;
  uint64_t disksize_bytes, next_request_id;
  struct rb_exec_result result;
};
struct nzm_core {
  struct nzm_config config;
  int runfd, wearfd, datafd; /* runfd retained for legacy test ABI; production stays -1. */
  int startupfd; /* Independent extm directory flock; backend may reopen datafd. */
  struct nzm_boot_state boot_state;
  char last_event[1024];
  enum nzm_watermark_state watermark, watermark_resume;
  int boot_completed;
  int timer_valid, no_progress;
  int last_attempt_valid;
  uint64_t last_attempt_tick;
  uint64_t timer_since, no_progress_since;
  struct nzm_plan blocked_plan, last_plan;
  const char *trigger;
  /* Trusted root/init launch contract, not authentication. Only skips property wait. */
  int trusted_boot_completed;
  char boot[37];
  struct nzm_run_lock lock;
  const char *stage;
  int pause_errno, snapshot_errno;
  uint64_t last_wait_log;
  int reserved, attempted, observed_before_valid, observed_after_valid;
  uint64_t observed_before, observed_after;
  struct nzm_execution execution;
  struct nzm_memory_budget memory_budget; /* Object-local; never persisted. */
  int intent_owned;
  dev_t intent_dev;
  ino_t intent_ino;
  char intent_record[384];
  const struct nzm_runtime_control *control; /* Worker-only, valid until finalize. */
  int cooperative_stop, sampled_policy_valid;
  uint64_t sampled_interval;
  /* Runtime supplies the exact startup mask. Synchronous entry keeps the
   * helper's explicitly empty default; never pass the blocked worker mask. */
  int helper_exec_mask_valid;
  sigset_t helper_exec_mask;
};
void nzm_core_init(struct nzm_core *c);
void nzm_core_close(struct nzm_core *c);
int nzm_core_config(struct nzm_core *c);
void nzm_core_config_snapshot(const struct nzm_core *c, FILE *out);
int nzm_core_status(struct nzm_core *c, FILE *out);
/* Production Phase 1 entry: no DM acquisition or writeback runtime. */
int nzm_phase1_start(struct nzm_core *c, int daemon);
int nzm_core_boot(struct nzm_core *c);
int nzm_core_daemon(struct nzm_core *c);
/* Immutable launch config only; worker constructs/boots/closes its own core. */
int nzm_event_daemon(const struct nzm_config *config, int trusted_boot_completed);
int nzm_event_next_result(const struct nzm_core *c, uint64_t now,
                          struct nzm_runtime_result *result);
int nzm_core_pause(struct nzm_core *c, const char *reason);
/* Production helpers exposed for focused syscall-boundary regression tests. */
int nzm_core_vm(struct nzm_core *c, int completed);
int nzm_core_provision(struct nzm_core *c);
int nzm_core_cycle(struct nzm_core *c);
int nzm_core_workload(const struct nzm_config *cfg, uint64_t page,
                      const uint64_t mm[10], const uint64_t bd[3],
                      uint64_t *upper);
#endif
