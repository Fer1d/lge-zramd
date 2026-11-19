#include "planner.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
int main(void) {
  struct nzm_config c; nzm_config_default(&c);
  c.batch_mib=16;c.watermark_high_pct=20;c.watermark_low_pct=10;
  for(uint64_t page=4096;page<=65536;page*=4) {
    uint64_t disk=1000*page,mm[10]={0},bd[3]={0};
    struct nzm_plan p,unchanged;
    enum nzm_watermark_state state=NZM_ARMED;
    mm[0]=page;mm[9]=1;
    assert(!nzm_plan_writeback(&c,page,disk,mm,bd,1,&state,&p));
    assert(p.pages==1 && p.reserve_kib==page/1024 && p.limit_4k==page/4096);
    mm[9]=0;
    assert(nzm_plan_writeback(&c,page,disk,mm,bd,1,&state,&p)==-EAGAIN && !p.pages);
    mm[0]=199*page;mm[9]=199;
    assert(nzm_plan_writeback(&c,page,disk,mm,bd,0,&state,&p)==-EAGAIN);
    mm[0]=500*page;mm[9]=500;
    uint64_t drained=0;
    do {
      assert(!nzm_plan_writeback(&c,page,disk,mm,bd,0,&state,&p));
      assert(state==NZM_DRAINING && p.pages<=c.batch_mib*1048576/page);
      assert(p.reserve_kib==p.pages*page/1024 && p.limit_4k==p.pages*page/4096);
      assert(p.pages<=400-drained);
      uint64_t before=bd[0];bd[0]+=p.limit_4k;drained+=p.pages;
      assert(!nzm_watermark_observe(&state,before,bd[0]));
    }while(drained<400);
    assert(nzm_plan_writeback(&c,page,disk,mm,bd,0,&state,&p)==-EAGAIN);
    assert(state==NZM_ARMED && p.resident_bytes==100*page);
    bd[0]=0;
    assert(!nzm_plan_writeback(&c,page,disk,mm,bd,0,&state,&p));
    assert(!nzm_watermark_observe(&state,0,0) && state==NZM_NO_PROGRESS);
    for(int i=0;i<10;i++)assert(nzm_plan_writeback(&c,page,disk,mm,bd,0,&state,&p)==-EAGAIN);
    mm[0]=100*page;mm[9]=100;
    assert(nzm_plan_writeback(&c,page,disk,mm,bd,0,&state,&p)==-EAGAIN && state==NZM_ARMED);
    mm[0]=500*page;mm[9]=500;
    assert(!nzm_plan_writeback(&c,page,disk,mm,bd,0,&state,&p));
    unchanged=p;bd[0]=UINT64_MAX;
    assert(nzm_plan_writeback(&c,page,disk,mm,bd,0,&state,&p)==-ERANGE);
    assert(!memcmp(&p,&unchanged,sizeof p));
    bd[0]=0;c.batch_mib=UINT64_MAX;
    assert(nzm_plan_writeback(&c,page,disk,mm,bd,0,&state,&p)==-ERANGE);
    c.batch_mib=16;c.watermark_high_pct=20;c.watermark_low_pct=10;
    assert(!nzm_watermark_observe(&state,2,1) && state==NZM_NO_PROGRESS);
  }
  puts("PASS pure planner: time one page/no candidate, 4/16/64K, high batch drain/low exit, no-progress rearm, overflow and atomic outputs");
}
