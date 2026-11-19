#include "nzm.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

int nzm_u64(const char *s, uint64_t *out)
{
    uint64_t n = 0;
    if (!s || !out || !*s) return -EINVAL;
    for (; *s; ++s) {
        unsigned d = (unsigned char)*s - (unsigned)'0';
        if (d > 9) return -EINVAL;
        if (n > (UINT64_MAX - d) / 10) return -ERANGE;
        n = n * 10 + d;
    }
    *out = n;
    return 0;
}

/* Actual domains/representation only, not intensity policy. arm64 target:
 * HZ=300; page_cluster is used in signed 1 << n (positive through 30).
 * Seconds are slept in chunks; uint64 is never cast to unsigned/time_t. */
#define ACTIVE_FIELDS(X) \
 X(enable_writeback,0,1,1) \
 X(backing_mib,1,4096,512) \
 X(disksize_bytes,1,UINT64_MAX-65535,1) \
 X(wait_log_interval_seconds,0,UINT64_MAX,300) \
 X(daily_mib,1,UINT64_MAX/1024,64) \
 X(min_data_free_mib,0,UINT64_MAX/1048576,2048) \
 X(data_wait_seconds,0,UINT64_MAX,60) \
 X(vm_swappiness,0,200,180) \
 X(vm_page_cluster,0,30,0) \
 X(vm_watermark_scale_factor,0,1000,200) \
 X(vm_extra_free_kbytes,0,INT_MAX,65536) \
 X(vm_min_free_kbytes,0,INT_MAX,0) \
 X(vm_minfree_mult,0,INT_MAX,2) \
 X(vm_overcommit_memory,0,2,1) \
 X(vm_laptop_mode,0,INT_MAX/300,0) \
 X(pool_low_mib,1,UINT32_MAX-1,512) \
 X(pool_high_mib,2,UINT32_MAX,768) \
 X(cold_age_seconds,1,86400,60) \
 X(demand_batch_mib,1,16,4) \
 X(demand_enabled,0,1,0) \
 X(demand_scan_limit,1,1048576,4096)

/* Accepted with original scalar ranges and duplicate checks, never applied. */
#define DEPRECATED_FIELDS(X) \
 X(runtime_event_enabled,0,1,0) \
 X(psi_enabled,0,1,0) \
 X(runtime_idle_sample_seconds,1,INT64_MAX/1000000000,30) \
 X(runtime_active_sample_seconds,1,INT64_MAX/1000000000,2) \
 X(runtime_max_resync_seconds,1,INT64_MAX/1000000000,60) \
 X(runtime_health_retry_seconds,1,INT64_MAX/1000000000,30) \
 X(psi_threshold_us,1,10000000,500000) \
 X(psi_window_us,500000,10000000,10000000) \
 X(idle_observation_seconds,0,UINT64_MAX,300) \
 X(sample_interval_seconds,1,UINT64_MAX,2) \
 X(time_trigger_enabled,0,1,1) \
 X(time_trigger_interval_seconds,1,UINT64_MAX,300) \
 X(watermark_trigger_enabled,0,1,1) \
 X(no_progress_retry_seconds,1,UINT64_MAX,600) \
 X(watermark_charging_batch_mib,1,UINT64_MAX/1048576,8) \
 X(watermark_charging_idle_observation_seconds,0,UINT64_MAX,30) \
 X(watermark_charging_min_interval_seconds,0,UINT64_MAX,30) \
 X(watermark_battery_batch_mib,1,UINT64_MAX/1048576,2) \
 X(watermark_battery_idle_observation_seconds,0,UINT64_MAX,60) \
 X(watermark_battery_min_interval_seconds,0,UINT64_MAX,120) \
 X(batch_mib,1,UINT64_MAX/1048576,4) \
 X(min_mem_available_kib,0,UINT64_MAX,524288) \
 X(watermark_high_pct,1,100,80) \
 X(watermark_low_pct,0,99,50)

struct field { const char *name; size_t offset; uint64_t min, max; int deprecated; };
#define FIELD(n,lo,hi,d) {#n, offsetof(struct nzm_config,n), (uint64_t)(lo), (uint64_t)(hi), 0},
#define DEPRECATED(n,lo,hi,d) {#n, offsetof(struct nzm_config,n), (uint64_t)(lo), (uint64_t)(hi), 1},
static const struct field fields[] = { ACTIVE_FIELDS(FIELD) DEPRECATED_FIELDS(DEPRECATED) };
#undef DEPRECATED
#undef FIELD
_Static_assert(sizeof(fields)/sizeof(fields[0]) + 2 <= 64, "all fields must fit seen bits");

const char *nzm_request_backend_name(enum nzm_request_backend mode)
{
    switch (mode) {
    case NZM_REQUEST_AUTO: return "auto";
    case NZM_REQUEST_LEGACY: return "legacy";
    case NZM_REQUEST_IOCTL: return "ioctl";
    default: return "invalid";
    }
}

void nzm_config_default(struct nzm_config *c)
{
    if (!c) return;
    memset(c, 0, sizeof(*c));
    memcpy(c->backend, "dm_extm_file", sizeof("dm_extm_file"));
#define DEFAULT(n,lo,hi,d) c->n = UINT64_C(d);
    ACTIVE_FIELDS(DEFAULT)
    DEPRECATED_FIELDS(DEFAULT)
#undef DEFAULT
    c->expected_bytes = c->backing_mib * UINT64_C(1048576);
}

static int fail(struct nzm_config_error *e, const char *key, size_t n,
                const char *reason, int rc)
{
    if (e) {
        snprintf(e->key,sizeof(e->key),"%.*s",(int)(n<63?n:63),key);
        e->reason=reason;
    }
    return rc;
}
static const char *range_reason(const char *key)
{
    if (!strcmp(key,"cold_age_seconds")) return "requires 1..86400 seconds";
    if (!strcmp(key,"demand_batch_mib")) return "requires 1..16 MiB";
    if (!strcmp(key,"demand_scan_limit")) return "requires 1..1048576 entries";
    if (!strncmp(key,"pool_",5)) return "MiB-to-byte product must fit uint64";
    if (!strcmp(key,"sample_interval_seconds")) return "zero would busy-spin; positive seconds required";
    if (!strcmp(key,"batch_mib")) return "positive MiB; *MiB, *1024 and *256 must fit uint64";
    if (!strcmp(key,"daily_mib")) return "positive MiB; ledger *1024 must fit uint64";
    if (!strncmp(key,"min_",4)) return "MiB-to-byte product must fit uint64";
    if (!strcmp(key,"disksize_bytes")) return "uint64 PAGE_ALIGN must not overflow at supported page sizes; actual kernel hardcodes RAM size";
    if (!strcmp(key,"backing_mib")) return "helper supports positive MiB through 4GiB; no active resize";
    if (!strcmp(key,"vm_page_cluster")) return "kernel signed-int 1 << page_cluster must remain positive (0..30)";
    if (!strcmp(key,"vm_laptop_mode")) return "kernel int jiffies at target CONFIG_HZ=300";
    return "outside boolean/enum/kernel integer domain";
}
int nzm_config_parse_ex(const char *text, size_t len, struct nzm_config *out,
                        struct nzm_config_error *e)
{
    struct nzm_config c;
    uint64_t seen = 0;
    size_t pos = 0;
#define FAIL(k,r,rc) return fail(e,k,strlen(k),r,rc)
    if (e) { e->key[0]=0; e->reason="none"; }
    if (!out || (!text && len)) { FAIL("<file>","invalid input",-EINVAL); }
    if (len > NZM_CONFIG_MAX_BYTES) { FAIL("<file>","config size guard",-E2BIG); }
    if (len && (text[len-1] != '\n' || memchr(text, 0, len) || memchr(text, '\r', len)))
        { FAIL("<file>","requires LF termination; NUL/CR forbidden",-EINVAL); }
    nzm_config_default(&c);
    while (pos < len) {
        const char *line = text + pos, *end = memchr(line, '\n', len-pos), *eq;
        size_t n = (size_t)(end-line), i;
        pos += n+1;
        if (!n || line[0] == '#') continue;
        eq = memchr(line, '=', n);
        if (!eq || eq == line) return fail(e,line,n,"expected key=value",-EINVAL);
        if ((size_t)(eq-line) == 7 && !memcmp(line, "backend", 7)) {
            uint64_t bit = UINT64_C(1) << (sizeof(fields)/sizeof(fields[0]));
            if (seen & bit) { FAIL("backend","duplicate key",-EEXIST); }
            seen |= bit;
            if ((size_t)(end-eq-1) != 12 || memcmp(eq+1,"dm_extm_file",12)) { FAIL("backend","only dm_extm_file supported",-EINVAL); }
            continue;
        }
        if ((size_t)(eq-line) == 15 && !memcmp(line,"request_backend",15)) {
            uint64_t bit = UINT64_C(1) << (sizeof(fields)/sizeof(fields[0]) + 1);
            if (seen & bit) { FAIL("request_backend","duplicate key",-EEXIST); }
            seen |= bit;
            size_t length = (size_t)(end-eq-1);
            if (length == 4 && !memcmp(eq+1,"auto",4)) c.request_backend=NZM_REQUEST_AUTO;
            else if (length == 6 && !memcmp(eq+1,"legacy",6)) c.request_backend=NZM_REQUEST_LEGACY;
            else if (length == 5 && !memcmp(eq+1,"ioctl",5)) c.request_backend=NZM_REQUEST_IOCTL;
            else { FAIL("request_backend","requires exact auto|legacy|ioctl",-EINVAL); }
            continue;
        }
        if ((size_t)(eq-line)==20 && !memcmp(line,"boot_first_writeback",20))
            { FAIL("boot_first_writeback","removed key; boot exception unsupported",-EINVAL); }
        for (i = 0; i < sizeof(fields)/sizeof(fields[0]); ++i) {
            const struct field *f = &fields[i];
            uint64_t v=0;
            const char *p;
            if (strlen(f->name) != (size_t)(eq-line) || memcmp(f->name,line,(size_t)(eq-line))) continue;
            if (seen & (UINT64_C(1)<<i)) { FAIL(f->name,"duplicate key",-EEXIST); }
            seen |= UINT64_C(1)<<i;
            if (eq+1==end) { FAIL(f->name,"empty uint64",-EINVAL); }
            for (p=eq+1;p<end;++p) {
                unsigned d=(unsigned char)*p-(unsigned)'0';
                if (d>9) { FAIL(f->name,"decimal uint64 required",-EINVAL); }
                if (v>(UINT64_MAX-d)/10) { FAIL(f->name,"uint64 overflow",-ERANGE); }
                v=v*10+d;
            }
            if (v < f->min || v > f->max) { FAIL(f->name,range_reason(f->name),-ERANGE); }
            if (!f->deprecated) *(uint64_t *)((char *)&c+f->offset)=v;
            break;
        }
        if (i == sizeof(fields)/sizeof(fields[0])) return fail(e,line,(size_t)(eq-line),"unknown key",-EINVAL);
    }
    if (c.pool_low_mib >= c.pool_high_mib)
        { FAIL("pool_low_mib","must be less than pool_high_mib",-EINVAL); }
    if (c.vm_min_free_kbytes && c.vm_minfree_mult) { FAIL("vm_min_free_kbytes","absolute and multiplier mutually exclusive",-EINVAL); }
    c.expected_bytes=c.backing_mib*UINT64_C(1048576);
    *out=c;
    return 0;
}

#undef FAIL
int nzm_config_parse(const char *text, size_t len, struct nzm_config *out)
{
    return nzm_config_parse_ex(text,len,out,NULL);
}
