#ifndef NZM_RUNTIME_H
#define NZM_RUNTIME_H

#include "psi.h"
#include "spawn_fd.h" /* ff9-owned header; link ONE shared spawn_fd.c. */
#include <stdbool.h>
#include <signal.h>
#include <stdint.h>
#include <time.h>

struct nzm_runtime;
struct nzm_runtime_control;

/* Integration contract (no nzm.h dependency, no policy/reload implementation):
 * - create/run/destroy are called on the same eventloop thread. create blocks
 *   INT/TERM/HUP BEFORE any other application threads are created; threads
 *   already present must cooperate with that mask. No signal handlers/reaper.
 * - create saves the exact original mask. Original SIGTERM blocked is rejected
 *   (-EINVAL), not silently replaced by an empty mask. run restores the exact
 *   mask only after worker exit, join, and runtime FD teardown. destroy without
 *   run also restores it. SIGUSR1 and other unrelated bits are preserved.
 * - All executor hooks, including initialization/destruction, run ONLY on the
 *   single worker. owner_context (future whole nzm_core) is opaque to eventloop.
 *   Caller must not access/destroy that context until run has returned.
 * - Runtime callbacks never own a plan: each fresh job is reasons+sequence.
 *   One in-flight job and ONE pending bitmask; there is no job queue.
 * - A scalar result is copied under the mailbox mutex, then eventfd notifies.
 *   eventloop never reads core, status, ledger, or worker-owned descriptors.
 * - Stop is cooperative at observer/commit/submitting boundaries. Never use
 *   SIGcancel/pthread_cancel or close a worker FD from another thread. Blocking
 *   I/O can delay shutdown indefinitely. STOP_RETAIN_RESERVATION is NOT refund
 *   permission; post-precharge accounting belongs exclusively to core owner.
 * - The worker publishes exited only AFTER all worker cleanup. eventloop waits
 *   for that publication, then joins, then closes eventfd/other runtime FDs and
 *   frees resources. Producers write eventfd under mailbox lock; teardown sets
 *   accepting=false under that lock before closing. Runtime object itself lives
 *   until destroy; caller quiesces all API users before destroy. Never destroy
 *   from executor. Runtime does not close FDs acquired by executor.
 * - Every runtime FD open/create/close/ownership-transfer uses the SAME global
 *   spawn_fd gate as helper. Do not wrap helper/preflight in it (non-recursive).
 *   Custom gate callbacks must cooperate with that exact process-wide gate.
 * - Pass control_get_exec_mask's complete original mask to ff9's
 *   nzm_helper_call_with_mask; do not use the legacy empty-mask helper entry.
 *   Worker owns helper PID/waitpid; runtime never consumes/reaps SIGCHLD.
 * - No option mutation/reload while running. HUP records unsupported only.
 */

enum nzm_runtime_reason {
    NZM_REASON_STARTUP = UINT32_C(1) << 0,
    NZM_REASON_TIMER = UINT32_C(1) << 1,
    NZM_REASON_PSI = UINT32_C(1) << 2,
    NZM_REASON_MANUAL = UINT32_C(1) << 3,
    NZM_REASON_FALLBACK = UINT32_C(1) << 4
};
#define NZM_REASON_EXTERNAL (NZM_REASON_MANUAL | NZM_REASON_PSI)

struct nzm_runtime_job {
    uint32_t reasons;
    uint64_t sequence;
};

struct nzm_runtime_result {
    uint64_t next_delay_ns; /* relative to completion; 0 means interval default */
    bool active;
    int code;              /* scalar diagnostic only; not an automatic retry */
};

enum nzm_runtime_boundary {
    NZM_BOUNDARY_OBSERVING,
    NZM_BOUNDARY_PRECOMMIT,     /* BEFORE durable precharge, not after it */
    NZM_BOUNDARY_POSTPRECHARGE,
    NZM_BOUNDARY_SUBMITTING    /* conservative: reservation may already exist */
};
enum nzm_runtime_stop_action {
    NZM_CONTROL_CONTINUE = 0,
    NZM_CONTROL_STOP_BEFORE_COMMIT = 1,
    NZM_CONTROL_STOP_RETAIN_RESERVATION = 2
};

struct nzm_runtime_executor {
    void *owner_context;
    int (*initialize)(void *owner_context, const struct nzm_runtime_control *ctl);
    void (*execute)(void *owner_context, const struct nzm_runtime_job *job,
                    const struct nzm_runtime_control *ctl,
                    struct nzm_runtime_result *result);
    /* Called even if initialize fails; owner must track partial initialization. */
    void (*finalize)(void *owner_context, const struct nzm_runtime_control *ctl);
};

struct nzm_runtime_fd_gate {
    void *context;
    int (*enter)(void *context, struct nzm_spawn_fd_guard *guard);
    void (*leave)(void *context, struct nzm_spawn_fd_guard *guard);
};

struct nzm_runtime_options {
    uint64_t idle_sample_interval_ns;
    uint64_t active_sample_interval_ns;
    uint64_t max_fallback_interval_ns; /* >= both minima; timer always retained */
    struct nzm_psi_options psi;
    const struct nzm_psi_adapter *psi_adapter; /* optional; callbacks copied */
    const struct nzm_runtime_fd_gate *fd_gate; /* optional; NULL uses shared gate */
};

struct nzm_runtime_stats {
    uint64_t jobs_started, jobs_completed, notifications, coalesced;
    uint64_t timer_events, psi_events, psi_fallbacks, hup_unsupported;
    uint64_t last_sequence, last_delay_ns;
    uint32_t last_reasons;
    int last_result_code, psi_error, worker_error;
    bool worker_joined, timer_only;
};

void nzm_runtime_options_default(struct nzm_runtime_options *options);
int nzm_runtime_create(struct nzm_runtime **out,
                        const struct nzm_runtime_options *options,
                        const struct nzm_runtime_executor *executor);
/* One-shot synchronous eventloop; returns only after worker join/FD teardown. */
int nzm_runtime_run(struct nzm_runtime *runtime);
/* Thread-safe from normal threads, NOT async-signal-safe. No application FDs. */
int nzm_runtime_notify(struct nzm_runtime *runtime, uint32_t reasons);
int nzm_runtime_request_stop(struct nzm_runtime *runtime);
/* Stats are read-only after run; returns -EBUSY while running. */
int nzm_runtime_get_stats(struct nzm_runtime *runtime,
                          struct nzm_runtime_stats *out);
/* Requires no concurrent API users; returns -EBUSY if run hasn't finished. */
int nzm_runtime_destroy(struct nzm_runtime *runtime);

bool nzm_runtime_control_stop_requested(const struct nzm_runtime_control *ctl);
int nzm_runtime_control_check(const struct nzm_runtime_control *ctl,
                              enum nzm_runtime_boundary boundary);
/* Absolute CLOCK_MONOTONIC deadline. Returns CONTINUE on expiry, a distinct
 * stop action on stop, or negative errno. Notifications/spurious wakeups do not
 * extend deadline. OBSERVING waits are interruptible by cooperative stop.
 */
int nzm_runtime_control_wait_until(const struct nzm_runtime_control *ctl,
                                   enum nzm_runtime_boundary boundary,
                                   const struct timespec *deadline);
int nzm_runtime_control_get_exec_mask(const struct nzm_runtime_control *ctl,
                                      sigset_t *out);

/* Pure scheduling helpers, exported for host clock/overflow tests. next_delay
 * is clamped to active/idle minimum and maximum fallback; events still invoke
 * executor, which alone enforces policy/eligibility and may decline work.
 */
uint64_t nzm_runtime_next_delay(const struct nzm_runtime_options *options,
                               const struct nzm_runtime_result *result);
int nzm_runtime_deadline_after(const struct timespec *now, uint64_t delay_ns,
                               struct timespec *deadline);

#endif
