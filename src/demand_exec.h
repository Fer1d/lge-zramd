#ifndef NZM_DEMAND_EXEC_H
#define NZM_DEMAND_EXEC_H
#include "nzm_core.h"
#include "demand_watch.h"
#include "uapi/linux/zram.h"

/* Private copy of include/uapi/linux/zram_policy.h: do not use v1 EXEC. */
struct nzm_demand_exec_wire {
  struct zram_wb_exec exec;
  __aligned_u64 demand_sequence, max_scanned_pages, min_age_ns;
  __aligned_u64 start_index, next_index, reserved[3];
};
#define NZM_DEMAND_EXEC_IOCTL _IOWR('Z', 0x22, struct nzm_demand_exec_wire)
struct nzm_demand_exec_state {
  uint64_t next_request_id; /* Initialize to 1; zero means exhausted. */
  uint64_t next_index;
  uint64_t expected_sequence; /* Optional watcher wake token; mismatch defers. */
  int halted, attempted, known;
  uint64_t granted_pages;
  struct nzm_demand_exec_wire reply;
};
/* All callbacks return zero or negative errno, never retry. exec is one ioctl
 * maximum; ANY failure is conservatively unknown, even failure before entry.
 * quota must set enable=1, set the limit, then verify both by readback. guard
 * verifies pause, stop and managed identity; it must NOT require quota zero.
 * ledger returns owned records (freed by the caller). commit is durable.
 * Injection replaces boundaries, not transaction ordering/accounting. */
struct nzm_demand_exec_ops {
  int (*guard)(void *, struct nzm_core *);
  int (*status)(void *, struct nzm_core *, struct rb_status_result *);
  int (*demand)(void *, struct nzm_demand_view *);
  int (*ledger)(void *, struct nzm_core *, struct nzm_ledger *);
  int (*quota)(void *, uint64_t limit_4k);
  int (*intent)(void *, struct nzm_core *, const struct nzm_plan *);
  int (*commit)(void *, struct nzm_core *, const char *, size_t);
  int (*exec)(void *, struct nzm_core *, struct nzm_demand_exec_wire *);
  int (*retire)(void *, struct nzm_core *);
  /* Before intent: extra whole replacement bytes plus filesystem reserve.
   * commit independently rechecks before mutating its transaction. */
  int (*preflight)(void *, struct nzm_core *, size_t);
};
/* Single worker owner with run lock held and boot/provision complete. NULL ops
 * selects real core/IO helpers. One finite transaction per call; attempt and
 * installed caps equal the grant, bounded by batch, frozen-highwater wear
 * availability and scan cap. Never refunds or retries.
 * Every exit attempts verified zero quota, including invalid/paused/unknown.
 * Errors after intent creation latch halted and preserve evidence. */
int nzm_core_demand_exec(struct nzm_core *, struct nzm_demand_exec_state *,
                         const struct nzm_demand_exec_ops *, void *);
int nzm_demand_execute(struct nzm_core *, const struct nzm_demand_view *);
#endif
