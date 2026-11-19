#define _GNU_SOURCE
#include "nzm_internal.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* Snapshot is pure: link only this function, never device-facing diagnostics. */
const char *nzm_execution_name(const struct nzm_core *c)
{
    (void)c;
    return "test_no_device";
}
int main(void)
{
    struct nzm_core c = {0};
    char *text = NULL;
    size_t len = 0;
    nzm_config_default(&c.config);
    FILE *out = open_memstream(&text, &len);
    assert(out);
    nzm_config_snapshot(&c, out, 0);
    assert(!fclose(out));
    const char *expected[] = {
        "scope=disk_snapshot", "running_daemon_config=unknown",
        "pool_low_mib=512 unit=MiB\n", "pool_high_mib=768 unit=MiB\n",
        "cold_age_seconds=60 unit=seconds\n", "demand_batch_mib=4 unit=MiB\n",
        "demand_enabled=0 unit=boolean\n", "demand_scan_limit=4096 unit=entries\n",
        "daily_mib=64 unit=MiB/rolling_86400_seconds\n",
        "legacy_scheduling_keys=deprecated_ignored legacy_fallback=disabled\n"
    };
    for (size_t i = 0; i < sizeof expected / sizeof expected[0]; ++i)
        assert(strstr(text, expected[i]));
    const char *absent[] = {"runtime_event_enabled=", "psi_enabled=", "time_policy=",
        "watermark_policy=", "watermark_high_pct=", "idle_observation_seconds=",
        "\nbatch_mib=", "min_mem_available_kib="};
    for (size_t i = 0; i < sizeof absent / sizeof absent[0]; ++i)
        assert(!strstr(text, absent[i]));
    free(text);
    puts("PASS config snapshot: demand units/defaults, ignored legacy labeling, no old policy");
    return 0;
}
