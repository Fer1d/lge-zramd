#ifndef NZM_DEMAND_WATCH_H
#define NZM_DEMAND_WATCH_H
#include <stdint.h>
struct nzm_core;
struct nzm_demand_view {
  uint64_t sequence, instance, generation, enabled, initialized, backing, active;
  uint64_t physical_pool_bytes, low_mib, high_mib, min_age_seconds;
};
/* Pure scheduling state. All deadlines are CLOCK_BOOTTIME nanoseconds. */
struct nzm_demand_schedule {
  uint64_t sequence, retry_ns, budget_ns;
  unsigned successes, zero_progress;
  int fatal;
};
enum nzm_demand_action { NZM_DEMAND_WAIT, NZM_DEMAND_EXECUTE, NZM_DEMAND_FATAL };
struct nzm_demand_decision { enum nzm_demand_action action; uint64_t deadline_ns; };
struct nzm_demand_decision nzm_demand_decide(struct nzm_demand_schedule *,
    const struct nzm_demand_view *, uint64_t now_ns);
void nzm_demand_complete(struct nzm_demand_schedule *, uint64_t now_ns,
    uint64_t min_age_seconds, int rc, int halted, int known,
    uint64_t installed_pages, uint64_t budget_deadline_ns);
int nzm_demand_parse(const char *, struct nzm_demand_view *);
int nzm_demand_read(int, struct nzm_demand_view *);
int nzm_demand_watch(struct nzm_core *);
#endif
