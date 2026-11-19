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
  puts("PASS demand text parser: canonical bounded state, identity and active/geometry checks");
  return 0;
}
