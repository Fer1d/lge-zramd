#ifndef NZM_DEMAND_WATCH_H
#define NZM_DEMAND_WATCH_H
#include <stdint.h>
struct nzm_core;
struct nzm_demand_view {
  uint64_t sequence, instance, generation, enabled, initialized, backing, active;
  uint64_t physical_pool_bytes, low_mib, high_mib, min_age_seconds;
};
int nzm_demand_parse(const char *, struct nzm_demand_view *);
int nzm_demand_read(int, struct nzm_demand_view *);
int nzm_demand_watch(struct nzm_core *);
#endif
