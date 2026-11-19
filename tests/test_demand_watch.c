#include "demand_watch.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  const char *good="sequence 7 instance 4 generation 3 enabled 1 initialized 1 backing 1 active 1 physical_pool_bytes 805306368 low_mib 512 high_mib 768 min_age_seconds 60\n";
  struct nzm_demand_view out={0};
  assert(!nzm_demand_parse(good,&out) && out.sequence==7 && out.instance==4 && out.active==1);
  const char *bad[]={
    "sequence -7 instance 4 generation 3 enabled 1 initialized 1 backing 1 active 1 physical_pool_bytes 805306368 low_mib 512 high_mib 768 min_age_seconds 60\n",
    "sequence 7 instance 4 generation 3 enabled 0 initialized 1 backing 1 active 1 physical_pool_bytes 805306368 low_mib 512 high_mib 768 min_age_seconds 60\n",
    "sequence 7 instance 4 generation 3 enabled 1 initialized 1 backing 1 active 1 physical_pool_bytes 805306368 low_mib 768 high_mib 512 min_age_seconds 60\n",
    "sequence 7 instance 4 generation 3 enabled 1 initialized 1 backing 1 active 1 physical_pool_bytes 805306368 low_mib 512 high_mib 768 min_age_seconds 60",
    "sequence 7 instance 4 generation 3 enabled 1 initialized 1 backing 1 active 1 physical_pool_bytes 805306368 low_mib 512 high_mib 768 min_age_seconds 60\nunknown 1\n",
  };
  for(unsigned i=0;i<sizeof bad/sizeof bad[0];i++)assert(nzm_demand_parse(bad[i],&out)==-EPROTO);
  const uint64_t sec=UINT64_C(1000000000);
  struct nzm_demand_schedule s={0};
  assert(nzm_demand_decide(&s,&out,sec).action==NZM_DEMAND_EXECUTE);
  nzm_demand_complete(&s,sec,10,0,0,1,0,0);
  assert(s.retry_ns==61*sec);
  assert(nzm_demand_decide(&s,&out,60*sec).action==NZM_DEMAND_WAIT);
  out.sequence++; /* Age refresh cannot bypass backoff. */
  assert(nzm_demand_decide(&s,&out,60*sec).action==NZM_DEMAND_WAIT);
  assert(s.zero_progress==1);
  assert(nzm_demand_decide(&s,&out,61*sec).action==NZM_DEMAND_EXECUTE);
  nzm_demand_complete(&s,61*sec,10,0,0,1,0,0);
  assert(s.retry_ns==181*sec);
  for(unsigned i=0;i<10;i++)nzm_demand_complete(&s,200*sec,60,0,0,1,0,0);
  assert(s.retry_ns==2000*sec); /* Exponential delay capped at 30 minutes. */
  memset(&s,0,sizeof s);out.sequence++;
  assert(nzm_demand_decide(&s,&out,sec).action==NZM_DEMAND_EXECUTE);
  nzm_demand_complete(&s,sec,60,-EDQUOT,0,0,0,100*sec);
  out.sequence++;
  struct nzm_demand_decision d=nzm_demand_decide(&s,&out,99*sec);
  assert(d.action==NZM_DEMAND_WAIT && d.deadline_ns==100*sec);
  assert(nzm_demand_decide(&s,&out,100*sec).action==NZM_DEMAND_EXECUTE);
  for(unsigned i=0;i<4;i++)nzm_demand_complete(&s,100*sec,60,0,0,1,1,0);
  assert(nzm_demand_decide(&s,&out,100*sec).action==NZM_DEMAND_WAIT);
  out.sequence++;
  assert(nzm_demand_decide(&s,&out,100*sec).action==NZM_DEMAND_EXECUTE);
  nzm_demand_complete(&s,100*sec,60,-EIO,1,0,0,0);
  out.sequence++;
  assert(nzm_demand_decide(&s,&out,UINT64_MAX).action==NZM_DEMAND_FATAL);
  memset(&s,0,sizeof s);
  nzm_demand_complete(&s,1,60,-EDQUOT,0,0,0,0);
  assert(!s.fatal && s.retry_ns==1+sec);
  assert(nzm_demand_decide(&s,&out,sec).action==NZM_DEMAND_WAIT);
  assert(nzm_demand_decide(&s,&out,1+sec).action==NZM_DEMAND_EXECUTE);
  nzm_demand_complete(&s,2*sec,60,-EDQUOT,0,0,0,sec);
  assert(!s.fatal && s.retry_ns==3*sec);
  assert(nzm_demand_decide(&s,&out,2*sec).action==NZM_DEMAND_WAIT);
  assert(nzm_demand_decide(&s,&out,3*sec).action==NZM_DEMAND_EXECUTE);
  /* Same demand sequence must resume when BOOTTIME budget expiry arrives. */
  memset(&s,0,sizeof s);
  nzm_demand_complete(&s,sec,60,-EDQUOT,0,0,0,10*sec);
  assert(nzm_demand_decide(&s,&out,9*sec).action==NZM_DEMAND_WAIT);
  assert(nzm_demand_decide(&s,&out,10*sec).action==NZM_DEMAND_EXECUTE);
  memset(&s,0,sizeof s);
  nzm_demand_complete(&s,sec,60,-EDQUOT,0,0,0,UINT64_MAX);
  assert(!s.fatal && nzm_demand_decide(&s,&out,UINT64_MAX).action==NZM_DEMAND_WAIT);
  memset(&s,0,sizeof s);
  nzm_demand_complete(&s,UINT64_MAX,60,-EDQUOT,0,0,0,1);
  assert(!s.fatal && nzm_demand_decide(&s,&out,UINT64_MAX).action==NZM_DEMAND_WAIT);
  memset(&s,0,sizeof s);
  nzm_demand_complete(&s,1,120,0,0,1,0,0);assert(s.retry_ns==1+120*sec);
  memset(&s,0,sizeof s);
  /* A slow execution must receive a full delay from completion, not start. */
  assert(nzm_demand_decide(&s,&out,sec).action==NZM_DEMAND_EXECUTE);
  nzm_demand_complete(&s,121*sec,60,0,0,1,0,0);
  assert(s.retry_ns==181*sec);
  assert(nzm_demand_decide(&s,&out,122*sec).action==NZM_DEMAND_WAIT);
  puts("PASS demand parser and pure scheduler: budget expiry, unchanged sequence, completion-based backoff, four successes, fatal latch");
  return 0;
}
