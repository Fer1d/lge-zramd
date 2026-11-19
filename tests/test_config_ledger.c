#include "nzm.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); abort(); } } while (0)
#ifdef NZM_TEST_ALLOC
static int fail_alloc;
void *nzm_test_calloc(size_t n,size_t s) { return fail_alloc ? NULL:calloc(n,s); }
void *nzm_test_malloc(size_t s) { return fail_alloc ? NULL:malloc(s); }
#endif
static void bad_config(const char *s)
{
    struct nzm_config c, before;
    nzm_config_default(&c); before=c;
    CHECK(nzm_config_parse(s,strlen(s),&c)<0);
    CHECK(!memcmp(&c,&before,sizeof(c)));
}
static void config_tests(void)
{
    struct nzm_config c;
    uint64_t u=7;
    const char *invalid[]={"", "-1", "+1", " 1", "1 ", "0x10", "1\n", "18446744073709551616", "9999999999999999999999999999999"};
    size_t i;
    CHECK(!nzm_u64("18446744073709551615",&u) && u==UINT64_MAX);
    CHECK(!nzm_u64("0000",&u) && u==0);
    for(i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) CHECK(nzm_u64(invalid[i],&u)<0);
    CHECK(nzm_u64(NULL,&u)<0 && nzm_u64("1",NULL)<0);
    nzm_config_default(&c);
    CHECK(c.backing_mib==512 && c.batch_mib==4 && c.daily_mib==64 && c.idle_observation_seconds==300 && c.sample_interval_seconds==2 && c.wait_log_interval_seconds==300);
    CHECK(c.time_trigger_interval_seconds==300 && c.no_progress_retry_seconds==600);
    CHECK(c.vm_swappiness==180 && c.vm_page_cluster==0 && c.vm_minfree_mult==2);
    CHECK(!nzm_config_parse(NULL,0,&c));
    CHECK(c.request_backend==NZM_REQUEST_AUTO && !c.runtime_event_enabled && !c.psi_enabled);
    for(int mode=NZM_REQUEST_AUTO;mode<=NZM_REQUEST_IOCTL;mode++) {
        char config[128];
        snprintf(config,sizeof config,"request_backend=%s\n",nzm_request_backend_name(mode));
        CHECK(!nzm_config_parse(config,strlen(config),&c) && c.request_backend==(enum nzm_request_backend)mode);
    }
    const char *bad_modes[]={"", "AUTO", "auto ", " auto", "ioctl #x", "0", "legacyX"};
    for(size_t j=0;j<sizeof bad_modes/sizeof bad_modes[0];j++) {
        char config[128];snprintf(config,sizeof config,"request_backend=%s\n",bad_modes[j]);bad_config(config);
    }
    bad_config("request_backend=auto\nrequest_backend=ioctl\n");
    bad_config("request_backend=auto\r\n");
    bad_config("request_backend=auto");
    bad_config("runtime_event_enabled=2\n");bad_config("psi_enabled=2\n");
    bad_config("runtime_event_enabled=0\nruntime_event_enabled=1\n");
    bad_config("psi_enabled=0\npsi_enabled=1\n");
    CHECK(!nzm_config_parse("runtime_event_enabled=1\npsi_enabled=1\n",strlen("runtime_event_enabled=1\npsi_enabled=1\n"),&c));
    CHECK(c.runtime_event_enabled==0 && c.psi_enabled==0);
    CHECK(!nzm_config_parse("backing_mib=4096\n",17,&c) && c.expected_bytes==UINT64_C(4294967296));
    CHECK(!nzm_config_parse("disksize_bytes=4294967295\n",26,&c) && c.disksize_bytes==UINT64_C(4294967295));
    bad_config("backing_mib=4097\n"); bad_config("backing_mib=0\n");
    bad_config("batch_mib=0\n"); bad_config("daily_mib=0\n");
    bad_config("cooldown_seconds=300\n");
    bad_config("boot_first_writeback=0\n");bad_config("boot_first_writeback=1\n");
    CHECK(!nzm_config_parse("watermark_battery_batch_mib=9\n",strlen("watermark_battery_batch_mib=9\n"),&c));
    CHECK(!nzm_config_parse("watermark_battery_idle_observation_seconds=29\n",strlen("watermark_battery_idle_observation_seconds=29\n"),&c));
    CHECK(!nzm_config_parse("watermark_battery_min_interval_seconds=29\n",strlen("watermark_battery_min_interval_seconds=29\n"),&c));
    for(int k=0;k<2;k++) {
      char b[1024];
      snprintf(b,sizeof b,"watermark_charging_batch_mib=%llu\nwatermark_battery_batch_mib=%llu\nwatermark_charging_idle_observation_seconds=%llu\nwatermark_battery_idle_observation_seconds=%llu\nwatermark_charging_min_interval_seconds=%llu\nwatermark_battery_min_interval_seconds=%llu\n",
        (unsigned long long)(k?UINT64_MAX/1048576:1),(unsigned long long)(k?UINT64_MAX/1048576:1),
        (unsigned long long)(k?UINT64_MAX:0),(unsigned long long)(k?UINT64_MAX:0),
        (unsigned long long)(k?UINT64_MAX:0),(unsigned long long)(k?UINT64_MAX:0));
      CHECK(!nzm_config_parse(b,strlen(b),&c));
    }
    bad_config("watermark_charging_batch_mib=17592186044416\n");
    bad_config("timeinterval=300\n");
    bad_config("time_trigger_enabled=2\n");
    bad_config("watermark_trigger_enabled=2\n");
    bad_config("time_trigger_interval_seconds=0\n");
    bad_config("no_progress_retry_seconds=0\n");
    bad_config("time_trigger_enabled=1\ntime_trigger_enabled=0\n");
    bad_config("backing_mib=18446744073709551616\n");
    bad_config("backing_mib=512"); bad_config("# comment");
    bad_config("batch_mib=4\nbatch_mib=4\n");
    bad_config("backend=dm_extm_file\nbackend=dm_extm_file\n");
    bad_config("backend=loop\n"); bad_config("unknown=1\n");
    CHECK(!nzm_config_parse("batch_mib=32\ndaily_mib=1\n",strlen("batch_mib=32\ndaily_mib=1\n"),&c));
    bad_config("vm_min_free_kbytes=1\n"); bad_config("batch_mib=4\r\n");
    bad_config(" batch_mib=4\n"); bad_config("batch_mib=4 #x\n");
    CHECK(nzm_config_parse("a\0b\n",4,&c)<0);
    CHECK(!nzm_config_parse("#v6\nbackend=dm_extm_file\nvm_minfree_mult=0\nvm_min_free_kbytes=1\n",strlen("#v6\nbackend=dm_extm_file\nvm_minfree_mult=0\nvm_min_free_kbytes=1\n"),&c));
    CHECK(!nzm_config_parse("backend=dm_extm_file\nvm_minfree_mult=0\nvm_min_free_kbytes=1\n", strlen("backend=dm_extm_file\nvm_minfree_mult=0\nvm_min_free_kbytes=1\n"), &c));
    puts("PASS config: strict uint64, defaults, 4GiB, bounds, duplicate/unknown/newline");
}
static void migration_tests(void)
{
    struct nzm_config defaults, c;
    static const struct { const char *key; uint64_t lo, hi; } ignored[]={
        {"runtime_event_enabled",0,1},
        {"psi_enabled",0,1},
        {"runtime_idle_sample_seconds",1,INT64_MAX/1000000000},
        {"runtime_active_sample_seconds",1,INT64_MAX/1000000000},
        {"runtime_max_resync_seconds",1,INT64_MAX/1000000000},
        {"runtime_health_retry_seconds",1,INT64_MAX/1000000000},
        {"psi_threshold_us",1,10000000},
        {"psi_window_us",500000,10000000},
        {"idle_observation_seconds",0,UINT64_MAX},
        {"sample_interval_seconds",1,UINT64_MAX},
        {"time_trigger_enabled",0,1},
        {"time_trigger_interval_seconds",1,UINT64_MAX},
        {"watermark_trigger_enabled",0,1},
        {"no_progress_retry_seconds",1,UINT64_MAX},
        {"watermark_charging_batch_mib",1,UINT64_MAX/1048576},
        {"watermark_charging_idle_observation_seconds",0,UINT64_MAX},
        {"watermark_charging_min_interval_seconds",0,UINT64_MAX},
        {"watermark_battery_batch_mib",1,UINT64_MAX/1048576},
        {"watermark_battery_idle_observation_seconds",0,UINT64_MAX},
        {"watermark_battery_min_interval_seconds",0,UINT64_MAX},
        {"batch_mib",1,UINT64_MAX/1048576},
        {"min_mem_available_kib",0,UINT64_MAX},
        {"watermark_high_pct",1,100},
        {"watermark_low_pct",0,99},
    };
    static const struct { const char *key; uint64_t lo, hi; } demand[]={
        {"demand_enabled",0,1}, {"cold_age_seconds",1,86400},
        {"demand_batch_mib",1,16}, {"demand_scan_limit",1,1048576}
    };
    char text[1024];
    nzm_config_default(&defaults);
    CHECK(defaults.pool_low_mib==512 && defaults.pool_high_mib==768);
    CHECK(defaults.cold_age_seconds==60 && defaults.demand_batch_mib==4);
    CHECK(defaults.demand_enabled==0 && defaults.demand_scan_limit==4096);
    for(size_t i=0;i<sizeof ignored/sizeof ignored[0];i++) {
        for(int j=0;j<2;j++) {
            snprintf(text,sizeof text,"%s=%" PRIu64 "\n",ignored[i].key,j?ignored[i].hi:ignored[i].lo);
            CHECK(!nzm_config_parse(text,strlen(text),&c));
            CHECK(!memcmp(&defaults,&c,sizeof c));
        }
        snprintf(text,sizeof text,"%s=%" PRIu64 "\n%s=%" PRIu64 "\n",ignored[i].key,ignored[i].lo,ignored[i].key,ignored[i].hi);
        bad_config(text);
        if(ignored[i].lo) {
            snprintf(text,sizeof text,"%s=%" PRIu64 "\n",ignored[i].key,ignored[i].lo-1); bad_config(text);
        }
        if(ignored[i].hi<UINT64_MAX) {
            snprintf(text,sizeof text,"%s=%" PRIu64 "\n",ignored[i].key,ignored[i].hi+1); bad_config(text);
        }
    }
    for(size_t i=0;i<sizeof demand/sizeof demand[0];i++) {
        for(int j=0;j<2;j++) {
            snprintf(text,sizeof text,"%s=%" PRIu64 "\n",demand[i].key,j?demand[i].hi:demand[i].lo);
            CHECK(!nzm_config_parse(text,strlen(text),&c));
        }
        if(demand[i].lo) {
            snprintf(text,sizeof text,"%s=%" PRIu64 "\n",demand[i].key,demand[i].lo-1); bad_config(text);
        }
        snprintf(text,sizeof text,"%s=%" PRIu64 "\n",demand[i].key,demand[i].hi+1); bad_config(text);
        snprintf(text,sizeof text,"%s=%" PRIu64 "\n%s=%" PRIu64 "\n",demand[i].key,demand[i].lo,demand[i].key,demand[i].hi); bad_config(text);
    }
    const char *cross="runtime_event_enabled=0\npsi_enabled=1\npsi_threshold_us=10000000\npsi_window_us=500000\nruntime_active_sample_seconds=90\nruntime_idle_sample_seconds=1\nruntime_max_resync_seconds=1\nwatermark_high_pct=1\nwatermark_low_pct=99\nwatermark_charging_batch_mib=1\nwatermark_battery_batch_mib=9\nwatermark_charging_idle_observation_seconds=100\nwatermark_battery_idle_observation_seconds=0\nwatermark_charging_min_interval_seconds=100\nwatermark_battery_min_interval_seconds=0\n";
    CHECK(!nzm_config_parse(cross,strlen(cross),&c));
    CHECK(!memcmp(&defaults,&c,sizeof c));
    const char *pool="pool_low_mib=1\npool_high_mib=2\n";
    CHECK(!nzm_config_parse(pool,strlen(pool),&c) && c.pool_low_mib==1 && c.pool_high_mib==2);
    snprintf(text,sizeof text,"pool_low_mib=%" PRIu64 "\npool_high_mib=%" PRIu64 "\n",(uint64_t)UINT32_MAX-1,(uint64_t)UINT32_MAX);
    CHECK(!nzm_config_parse(text,strlen(text),&c));
    CHECK(c.pool_low_mib==UINT32_MAX-1 && c.pool_high_mib==UINT32_MAX);
    bad_config("pool_low_mib=768\n"); bad_config("pool_high_mib=512\n");
    bad_config("pool_low_mib=0\n"); bad_config("pool_high_mib=1\n");
    bad_config("pool_low_mib=4294967295\npool_high_mib=4294967295\n");
    bad_config("pool_high_mib=4294967296\n");
    bad_config("pool_low_mib=17592186044416\n"); bad_config("pool_high_mib=17592186044416\n");
    bad_config("pool_low_mib=1\npool_low_mib=1\n"); bad_config("pool_high_mib=1024\npool_high_mib=1024\n");
    const char *mixed="demand_enabled=1\ndemand_batch_mib=8\ncold_age_seconds=120\npool_low_mib=256\npool_high_mib=1024\ndemand_scan_limit=32\nbatch_mib=16\nidle_observation_seconds=0\n";
    CHECK(!nzm_config_parse(mixed,strlen(mixed),&c));
    CHECK(c.demand_enabled==1 && c.demand_batch_mib==8 && c.cold_age_seconds==120);
    CHECK(c.pool_low_mib==256 && c.pool_high_mib==1024 && c.demand_scan_limit==32);
    CHECK(c.batch_mib==defaults.batch_mib && c.idle_observation_seconds==defaults.idle_observation_seconds);
    puts("PASS migration: all deprecated key ranges/duplicates, ignored cross-fields, demand bounds/profile isolation");
}
static void bad_ledger(const char *s,uint64_t now)
{
    struct nzm_reservation record={7,11};
    struct nzm_ledger l={17,23,31,37,41,1,&record}, before;
    memcpy(&before,&l,sizeof(l));
    CHECK(nzm_ledger_parse(s,strlen(s),now,&l)<0);
    CHECK(!memcmp(&l,&before,sizeof(l)));
}
static void bounds_tests(void)
{
    static const struct { const char *key; uint64_t lo,hi; } bounds[]={
        {"enable_writeback",0,1}, {"backing_mib",1,4096},
        {"disksize_bytes",1,UINT64_MAX-65535},
        {"idle_observation_seconds",0,UINT64_MAX}, {"sample_interval_seconds",1,UINT64_MAX},
        {"wait_log_interval_seconds",0,UINT64_MAX},
        {"batch_mib",1,UINT64_MAX/1048576}, {"daily_mib",1,UINT64_MAX/1024},
        {"min_data_free_mib",0,UINT64_MAX/1048576}, {"min_mem_available_kib",0,UINT64_MAX},
        {"data_wait_seconds",0,UINT64_MAX}, {"vm_swappiness",0,200},
        {"vm_page_cluster",0,30}, {"vm_watermark_scale_factor",0,1000},
        {"vm_extra_free_kbytes",0,INT_MAX}, {"vm_min_free_kbytes",0,INT_MAX},
        {"vm_minfree_mult",0,INT_MAX}, {"vm_overcommit_memory",0,2}, {"vm_laptop_mode",0,INT_MAX/300}
    };
    struct nzm_config c;
    size_t i;
    char text[256];
    for(i=0;i<sizeof(bounds)/sizeof(bounds[0]);++i) {
        uint64_t values[2]={bounds[i].lo,bounds[i].hi};
        size_t j;
        for(j=0;j<2;++j) {
            int n=snprintf(text,sizeof(text),"batch_mib=1\nvm_minfree_mult=0\n%s=%" PRIu64 "\n",bounds[i].key,values[j]);
            /* Avoid intentionally duplicated setup keys. */
            if (!strcmp(bounds[i].key,"batch_mib") || !strcmp(bounds[i].key,"vm_minfree_mult"))
                n=snprintf(text,sizeof(text),"%s=%" PRIu64 "\n",bounds[i].key,values[j]);
            CHECK(n>0 && !nzm_config_parse(text,(size_t)n,&c));
        }
        if (bounds[i].hi<UINT64_MAX) {
            snprintf(text,sizeof(text),"%s=%" PRIu64 "\n",bounds[i].key,bounds[i].hi+1);
            bad_config(text);
        }
        if(bounds[i].lo) {
            snprintf(text,sizeof(text),"%s=%" PRIu64 "\n",bounds[i].key,bounds[i].lo-1);
            bad_config(text);
        }
    }
    puts("PASS every deployed v6 config key lower/upper bounds and outside-bound rejection");
}
static void ledger_tests(void)
{
    struct nzm_ledger l={0}, next={0};
    char *out=NULL;
    size_t len=0;
    const char *gold="wear-v1 2 100000 8192\n1 4096\n100000 4096\n";
    CHECK(!nzm_ledger_parse("wear-v1 0 0 0\n",14,100000,&l));
    CHECK(!nzm_ledger_reserve_text(&l,100000,4096,65536,&out,&len));
    CHECK(!strcmp(out,"wear-v1 1 100000 4096\n100000 4096\n"));
    CHECK(!nzm_ledger_parse(out,len,100000,&next));
    free(out); nzm_ledger_free(&l); nzm_ledger_free(&next);
    CHECK(!nzm_ledger_parse(gold,strlen(gold),100000,&l));
    CHECK(l.used_kib==4096 && l.cumulative==8192);
    CHECK(!nzm_ledger_reserve_text(&l,100001,4096,65536,&out,&len));
    CHECK(!strcmp(out,"wear-v1 3 100001 12288\n1 4096\n100000 4096\n100001 4096\n"));
    CHECK(l.generation==2 && l.cumulative==8192); free(out);
    CHECK(nzm_ledger_reserve_text(&l,99999,4096,65536,&out,&len)<0 && !out && !len);
    CHECK(nzm_ledger_reserve_text(&l,100000,0,65536,&out,&len)<0);
    CHECK(nzm_ledger_reserve_text(&l,100000,1,0,&out,&len)<0);
    CHECK(nzm_ledger_reserve_text(&l,100000,65537,65536,&out,&len)<0);
#ifdef NZM_TEST_ALLOC
    {
        struct nzm_reservation record={7,11};
        struct nzm_ledger sentinel={17,23,31,37,41,1,&record}, before;
        char marker;
        memcpy(&before,&sentinel,sizeof(sentinel));
        out=&marker; len=12345;
        fail_alloc=1;
        CHECK(nzm_ledger_reserve_text(&l,100000,4096,65536,&out,&len)==-ENOMEM);
        CHECK(!out && !len);
        CHECK(nzm_ledger_parse(gold,strlen(gold),100000,&sentinel)==-ENOMEM);
        CHECK(!memcmp(&sentinel,&before,sizeof(sentinel)));
        fail_alloc=0;
    }
#endif
    nzm_ledger_free(&l);
    bad_ledger("wear-v1 0 0 0",1); bad_ledger("wear-v1 0 0 0\n",0);
    bad_ledger("wear-v1 0 2 0\n",1); bad_ledger("wear-v1 1 2 1\n3 1\n",3);
    bad_ledger("wear-v1 2 2 2\n2 1\n1 1\n",3);
    bad_ledger("wear-v1 1 2 2\n2 1\n",3); bad_ledger("wear-v1 2 2 1\n2 1\n",3);
    bad_ledger("wear-v1 0 0 0\n1 1\n",3); bad_ledger("wear-v1 1 2 0\n2 0\n",3);
    bad_ledger("wear-v1 1 2 1\n0 1\n",3);
    bad_ledger("wear-v1 18446744073709551616 0 0\n",1);
    bad_ledger("wear-v1 0 18446744073709551616 0\n",UINT64_MAX);
    bad_ledger("wear-v1 0 0 18446744073709551616\n",1);
    bad_ledger("wear-v1 1 2 18446744073709551615\n2 1\n",3);
    bad_ledger("wear-v1 1 2 1\n2 18446744073709551616\n",3);
    bad_ledger("wear-v1 100001 0 0\n",1);
    bad_ledger("wear-v1 1 2 1\n2 1",3); bad_ledger("wear-v1 0 0 0\n\n",3);
    bad_ledger("wear-v1  0 0 0\n",3); bad_ledger("wear-v1 0 0 0\r\n",3);
    /* Raw legacy budget is not silently adopted: golden fixture supplies its
     * explicit reviewed wear-v1 encoding; no device conversion exists. */
    bad_ledger("100000 4096\n",100000);
    puts("PASS ledger: golden append, ALL history preserved, no refund, corruption/future checks");
}
static void review_regressions(void)
{
    struct nzm_ledger l={0}, next={0};
    struct nzm_reservation record={7,11};
    struct nzm_ledger sentinel={17,23,31,37,41,1,&record}, before;
    const char early[]="wear-v1 1 1 4096\n1 4096\n";
    const char embedded[]="wear-v1 1 1 4096\n1 4096\n\0\n";
    char text[1024], *out=NULL;
    size_t pos, len=0, i;
    CHECK(!nzm_ledger_parse(early,sizeof(early)-1,200000,&l));
    CHECK(l.highwater==1 && l.validated_at==200000);
    /* now remains above highwater: only validated_at detects this rollback. */
    CHECK(nzm_ledger_reserve_text(&l,199999,4096,65536,&out,&len)==-EINVAL);
    CHECK(!out && !len);
    nzm_ledger_free(&l);
    memcpy(&before,&sentinel,sizeof(sentinel));
    /* Explicit length includes the embedded NUL and following newline. */
    CHECK(nzm_ledger_parse(embedded,sizeof(embedded)-1,200000,&sentinel)==-EINVAL);
    CHECK(!memcmp(&sentinel,&before,sizeof(sentinel)));
    pos=(size_t)sprintf(text,"wear-v1 16 100015 65536\n");
    for(i=0;i<16;++i)
        pos+=(size_t)sprintf(text+pos,"%" PRIu64 " 4096\n",UINT64_C(100000)+i);
    CHECK(!nzm_ledger_parse(text,pos,200000,&l));
    CHECK(!nzm_ledger_reserve_text(&l,200000,4096,65536,&out,&len));
    CHECK(!nzm_ledger_parse(out,len,200000,&next));
    CHECK(next.generation==17 && next.count==17);
    CHECK(next.cumulative==UINT64_C(17)*4096 && next.highwater==200000);
    for(i=0;i<16;++i) {
        CHECK(l.records[i].timestamp==UINT64_C(100000)+i);
        CHECK(next.records[i].timestamp==l.records[i].timestamp);
        CHECK(next.records[i].kib==l.records[i].kib && next.records[i].kib==4096);
    }
    CHECK(next.records[16].timestamp==200000 && next.records[16].kib==4096);
    CHECK(l.generation==16 && l.count==16 && l.cumulative==65536);
    free(out); nzm_ledger_free(&l); nzm_ledger_free(&next);
    puts("PASS review: independent validated_at rollback, explicit-length NUL, nonzero failure snapshot, 16 distinct historical rows roundtrip");
}
static void window_and_guard(void)
{
    struct nzm_ledger l={0};
    char *text=malloc(5000100), *out=NULL;
    size_t pos, len=0, i;
    CHECK(text!=NULL);
    pos=(size_t)sprintf(text,"wear-v1 16 100000 65536\n");
    for(i=0;i<16;++i) pos+=(size_t)sprintf(text+pos,"100000 4096\n");
    CHECK(!nzm_ledger_parse(text,pos,100000,&l) && l.used_kib==65536);
    CHECK(nzm_ledger_reserve_text(&l,186399,4096,65536,&out,&len)==-EDQUOT && !out);
    CHECK(!nzm_ledger_reserve_text(&l,186400,4096,65536,&out,&len));
    CHECK(strstr(out,"100000 4096\n100000 4096\n")!=NULL);
    CHECK(strstr(out,"186400 4096\n")!=NULL);
    free(out); nzm_ledger_free(&l);
    pos=(size_t)sprintf(text,"wear-v1 100000 1 100000\n");
    for(i=0;i<100000;++i) pos+=(size_t)sprintf(text+pos,"1 1\n");
    CHECK(!nzm_ledger_parse(text,pos,100000,&l) && l.count==100000);
    CHECK(nzm_ledger_reserve_text(&l,100000,1,65536,&out,&len)==-E2BIG && !out && !len);
    nzm_ledger_free(&l);
    /* 64-bit timestamps must neither wrap nor get truncated to shell int32. */
    pos=(size_t)sprintf(text,"wear-v1 1 %" PRIu64 " 1\n%" PRIu64 " 1\n",UINT64_MAX,UINT64_MAX);
    CHECK(!nzm_ledger_parse(text,pos,UINT64_MAX,&l));
    CHECK(!nzm_ledger_reserve_text(&l,UINT64_MAX,1,2,&out,&len));
    free(out); nzm_ledger_free(&l); free(text);
    puts("PASS legacy 16x4MiB golden compatibility, exact rolling boundary, 100k guard, uint64 time");
}
int main(int argc, char **argv)
{
    struct nzm_config c;
    struct nzm_config_error e;
    if (argc==2) {
        char buffer[NZM_CONFIG_MAX_BYTES];
        FILE *f=fopen(argv[1],"rb"); CHECK(f!=NULL);
        size_t n=fread(buffer,1,sizeof(buffer),f); CHECK(!ferror(f) && feof(f)); fclose(f);
        CHECK(!nzm_config_parse_ex(buffer,n,&c,&e));
        CHECK(c.backing_mib==1024 && c.idle_observation_seconds==300 && c.wait_log_interval_seconds==30 && c.batch_mib==4 && c.daily_mib==1024);
        CHECK(c.time_trigger_interval_seconds==300 && c.no_progress_retry_seconds==600);
        puts("PASS actual user config file, exact bytes (no device I/O)");
    }
    const char *profile="idle_observation_seconds=60\nsample_interval_seconds=300\nwait_log_interval_seconds=30\nbatch_mib=16\ndaily_mib=1024\nwatermark_high_pct=20\nwatermark_low_pct=10\n";
    CHECK(!nzm_config_parse_ex(profile,strlen(profile),&c,&e));
    CHECK(c.idle_observation_seconds==300 && c.wait_log_interval_seconds==30 && c.daily_mib==1024);
    const char *overflow="batch_mib=17592186044416\n";
    CHECK(nzm_config_parse_ex(overflow,strlen(overflow),&c,&e)==-ERANGE);
    CHECK(!strcmp(e.key,"batch_mib") && strstr(e.reason,"fit uint64"));
    const char *zero="idle_observation_seconds=0\nwait_log_interval_seconds=0\nmin_data_free_mib=0\nmin_mem_available_kib=0\n";
    CHECK(!nzm_config_parse(zero,strlen(zero),&c));
    CHECK(nzm_config_parse_ex("sample_interval_seconds=0\n",strlen("sample_interval_seconds=0\n"),&c,&e)<0);
    CHECK(!strcmp(e.key,"sample_interval_seconds") && strstr(e.reason,"busy-spin"));
    struct nzm_ledger old={0}, next={0}; char *appended=NULL; size_t len=0;
    const char *sixteen="wear-v1 1 2 16384\n2 16384\n";
    CHECK(!nzm_ledger_parse(sixteen,strlen(sixteen),3,&old));
    CHECK(!nzm_ledger_reserve_text(&old,3,16384,1048576,&appended,&len));
    CHECK(!nzm_ledger_parse(appended,len,3,&next));
    CHECK(next.count==2 && next.cumulative==32768 && next.records[0].kib==16384);
    free(appended); nzm_ledger_free(&old); nzm_ledger_free(&next);
    const char *huge="wear-v1 1 2 18446744073709551615\n2 18446744073709551615\n";
    CHECK(!nzm_ledger_parse(huge,strlen(huge),3,&old));
    CHECK(nzm_ledger_reserve_text(&old,3,1,UINT64_MAX,&appended,&len)==-ERANGE);
    nzm_ledger_free(&old);
    const char *large="wear-v1 1 2 65537\n2 65537\n";
    CHECK(!nzm_ledger_parse(large,strlen(large),3,&old));
    CHECK(!nzm_ledger_reserve_text(&old,3,65537,UINT64_MAX,&appended,&len));
    free(appended); nzm_ledger_free(&old);
    bad_config("sampleinterval=2\n");
    bad_config("highpct=20\n");
    bad_config("lowpct=10\n");
    bad_config("sample_interval_seconds=2\nsampleinterval=2\n");
    bad_config("watermark_high_pct=20\nhighpct=20\n");
    bad_config("watermark_low_pct=10\nlowpct=10\n");
    bad_config("poll_interval_seconds=300\n");
    bad_config("min_zram_mem_mib=0\n");
    bad_config("min_idle_mib=0\nwatermark_high_pct=20\n");
    CHECK(!nzm_config_parse("watermark_low_pct=20\nwatermark_high_pct=20\n",strlen("watermark_low_pct=20\nwatermark_high_pct=20\n"),&c));
    bad_config("watermark_high_pct=101\n");
    bad_config("watermark_low_pct=100\n");
    migration_tests();
    config_tests(); bounds_tests(); ledger_tests(); review_regressions(); window_and_guard();
    printf("PASS %u checks (production config.c + ledger.c)\n",checks);
    return 0;
}
