#ifndef NZM_PLANNER_H
#define NZM_PLANNER_H
#include "nzm.h"
/* Watermark state, including NO_PROGRESS, is process-local. A daemon restart
 * resets it; no boot writeback exception exists.
 * No persistent watermark state or cross-process no-progress claim. */
enum nzm_watermark_state { NZM_ARMED, NZM_DRAINING, NZM_NO_PROGRESS };
struct nzm_plan {
  uint64_t resident_bytes, candidate_upper, free_backing_pages;
  uint64_t pages, reserve_kib, limit_4k;
};
/* Pure planner. On error output and state unchanged. EAGAIN is a valid waiting
 * decision (state is updated, output contains sample with zero request).
 * unrestricted selects the time-track plan without watermarks; caller owns safety/policy.
 * mm[9] is a coarse candidate upper bound, not proof of idle pages. */
int nzm_plan_writeback(const struct nzm_config *, uint64_t page,
    uint64_t actual_disksize, const uint64_t mm[10], const uint64_t bd[3],
    int unrestricted, enum nzm_watermark_state *, struct nzm_plan *);
int nzm_watermark_observe(enum nzm_watermark_state *, uint64_t before_backed,
                          uint64_t after_backed);
#endif
