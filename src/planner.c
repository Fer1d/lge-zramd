#include "planner.h"
#include <errno.h>
#include <string.h>
#define MIB UINT64_C(1048576)
/* ceil(n*pct/100), without overflowing n* pct. */
static uint64_t threshold(uint64_t n, uint64_t pct) {
  return n / 100 * pct + (n % 100 * pct + 99) / 100;
}
int nzm_plan_writeback(const struct nzm_config *c, uint64_t page,
    uint64_t disk, const uint64_t mm[10], const uint64_t bd[3], int unrestricted,
    enum nzm_watermark_state *state, struct nzm_plan *out) {
  if (!c || !mm || !bd || !state || !out || (unrestricted != 0 && unrestricted != 1)) return -EINVAL;
  if (page != 4096 && page != 16384 && page != 65536) return -ENOTSUP;
  if (!disk || disk % page || mm[0] % page || mm[0] > disk ||
      !c->watermark_high_pct || c->watermark_high_pct > 100 || c->watermark_low_pct >= c->watermark_high_pct ||
      *state < NZM_ARMED || *state > NZM_NO_PROGRESS) return -EINVAL;
  if (!c->batch_mib || !c->backing_mib || c->batch_mib > UINT64_MAX/MIB ||
      c->backing_mib > UINT64_MAX/MIB || bd[0] > UINT64_MAX/4096) return -ERANGE;
  uint64_t backed_bytes = bd[0]*4096, backing_bytes=c->backing_mib*MIB;
  if (bd[0] % (page/4096) || backed_bytes > mm[0] || backed_bytes > backing_bytes)
    return -EINVAL;
  uint64_t orig=mm[0]/page, backed=backed_bytes/page;
  if (mm[5]>orig || mm[9]>orig || mm[5]>orig-backed) return -EINVAL;
  struct nzm_plan p;
  memset(&p,0,sizeof p);
  p.resident_bytes=mm[0]-backed_bytes;
  p.candidate_upper=orig-backed-mm[5];
  if(p.candidate_upper>mm[9])p.candidate_upper=mm[9];
  p.free_backing_pages=(backing_bytes-backed_bytes)/page;
  enum nzm_watermark_state next=*state;
  uint64_t low=threshold(disk,c->watermark_low_pct), high=threshold(disk,c->watermark_high_pct);
  if(p.resident_bytes<=low)next=NZM_ARMED;
  if(!unrestricted && next==NZM_ARMED && p.resident_bytes>=high)next=NZM_DRAINING;
  if(!unrestricted && next!=NZM_DRAINING)goto waiting;
  p.pages=c->batch_mib*MIB/page;
  if(p.pages>p.candidate_upper)p.pages=p.candidate_upper;
  if(p.pages>p.free_backing_pages)p.pages=p.free_backing_pages;
  if(!unrestricted) {
    uint64_t distance=(p.resident_bytes-low)/page;
    if(p.pages>distance)p.pages=distance;
  }
  if(!p.pages)goto waiting;
  /* Multipliers bounded by batch bytes, nevertheless validate at API boundary. */
  if(p.pages>UINT64_MAX/(page/1024) || p.pages>UINT64_MAX/(page/4096))return -ERANGE;
  p.reserve_kib=p.pages*(page/1024);
  p.limit_4k=p.pages*(page/4096);
  *state=next;*out=p;return 0;
waiting:
  *state=next;*out=p;return -EAGAIN;
}
int nzm_watermark_observe(enum nzm_watermark_state *state, uint64_t before, uint64_t after) {
  if(!state || *state<NZM_ARMED || *state>NZM_NO_PROGRESS)return -EINVAL;
  if(after<=before)*state=NZM_NO_PROGRESS;
  return 0;
}
