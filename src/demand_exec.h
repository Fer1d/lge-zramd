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
  uint64_t next_index;
  uint64_t expected_sequence; /* Optional watcher wake token; mismatch defers. */
  int halted, attempted, known, settled;
  uint64_t known_submitted_kib, pending_kib;
  uint64_t granted_pages;
  uint64_t budget_deadline_ns; /* BOOTTIME when at least one page fits. */
  struct nzm_demand_exec_wire reply;
};
/* Injectable IO boundaries, not accounting. All return zero or negative errno.
 * quota verifies enable=1 and requested limit by readback. guard checks control,
 * pause, identity, old transaction files and any non-owned execution intent.
 * Any exec failure is unknown and is never retried. now supplies BOOTTIME. */
struct nzm_demand_exec_ops {
  int (*guard)(void *, struct nzm_core *);
  int (*status)(void *, struct nzm_core *, struct rb_status_result *);
  int (*demand)(void *, struct nzm_demand_view *);
  int (*quota)(void *, uint64_t limit_4k);
  int (*intent)(void *, struct nzm_core *, const struct nzm_plan *);
  int (*exec)(void *, struct nzm_core *, struct nzm_demand_exec_wire *);
  int (*retire)(void *, struct nzm_core *);
  int (*now)(void *, uint64_t *);
};
/* The core owns the memory budget and request ID across calls. Reserve memory,
 * create durable intent, then open quota. A known reply settles submissions only
 * after closure, fresh STATUS and fresh time. Unknown/uncertain outcomes retain
 * the full reservation, halt, and preserve intent. Every exit closes quota. */
int nzm_core_demand_exec(struct nzm_core *, struct nzm_demand_exec_state *,
                         const struct nzm_demand_exec_ops *, void *);
int nzm_demand_execute(struct nzm_core *, const struct nzm_demand_view *);
#endif
