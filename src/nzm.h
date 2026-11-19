#ifndef NZM_H
#define NZM_H

#include <stddef.h>
#include <stdint.h>

/* All APIs return 0 or negative errno; no module performs device I/O here. */
#define NZM_LEDGER_MAX_RECORDS UINT64_C(100000)
#define NZM_LEDGER_WINDOW_SECONDS UINT64_C(86400)
#define NZM_CONFIG_MAX_BYTES ((size_t)65536)
#define NZM_LEDGER_MAX_BYTES ((size_t)8388608)

enum nzm_request_backend { NZM_REQUEST_AUTO, NZM_REQUEST_LEGACY, NZM_REQUEST_IOCTL };
const char *nzm_request_backend_name(enum nzm_request_backend mode);

struct nzm_config {
    char backend[16]; /* fixed literal dm_extm_file */
    enum nzm_request_backend request_backend;
    uint64_t pool_low_mib, pool_high_mib, cold_age_seconds;
    uint64_t demand_batch_mib, demand_enabled, demand_scan_limit;
    /* Legacy scheduling members remain for old fixtures only. Their config
     * keys are deprecated, validated and ignored; defaults stay dormant. */
    uint64_t runtime_event_enabled, psi_enabled;
    uint64_t runtime_idle_sample_seconds, runtime_active_sample_seconds;
    uint64_t runtime_max_resync_seconds, runtime_health_retry_seconds;
    uint64_t psi_threshold_us, psi_window_us;
    uint64_t enable_writeback, backing_mib, disksize_bytes;
    uint64_t idle_observation_seconds, sample_interval_seconds, wait_log_interval_seconds;
    uint64_t time_trigger_enabled, time_trigger_interval_seconds;
    uint64_t watermark_trigger_enabled, no_progress_retry_seconds;
    uint64_t batch_mib, daily_mib;
    uint64_t watermark_charging_batch_mib, watermark_charging_idle_observation_seconds, watermark_charging_min_interval_seconds;
    uint64_t watermark_battery_batch_mib, watermark_battery_idle_observation_seconds, watermark_battery_min_interval_seconds;
    uint64_t min_data_free_mib, min_mem_available_kib;
    uint64_t watermark_high_pct, watermark_low_pct, data_wait_seconds;
    uint64_t vm_swappiness, vm_page_cluster, vm_watermark_scale_factor;
    uint64_t vm_extra_free_kbytes, vm_min_free_kbytes, vm_minfree_mult;
    uint64_t vm_overcommit_memory, vm_laptop_mode;
    uint64_t expected_bytes; /* checked backing_mib * 1048576; includes 4GiB */
};
struct nzm_reservation { uint64_t timestamp, kib; };
struct nzm_ledger {
    uint64_t generation, highwater, cumulative;
    uint64_t used_kib; /* at validated_at, strictly age < 86400 */
    uint64_t validated_at;
    size_t count;
    struct nzm_reservation *records;
};
/* Shared identity representation; interpretation belongs to core/io. */
struct nzm_identity {
    uint64_t device, inode, size_bytes;
    uint64_t major, minor;
};

void nzm_config_default(struct nzm_config *out);
int nzm_u64(const char *text, uint64_t *out);
/* Strict LF-terminated demand keys plus validated/ignored legacy scheduling
 * keys; defaults for omitted keys.
 * On failure output is unchanged. No embedded NUL, CR or duplicate keys. */
struct nzm_config_error { char key[64]; const char *reason; };
int nzm_config_parse_ex(const char *text, size_t len, struct nzm_config *out,
                        struct nzm_config_error *error);
int nzm_config_parse(const char *text, size_t len, struct nzm_config *out);
/* Output unchanged on failure. On success owns records; initialize output to
 * zero and free before reuse. Pending ledger.new/authority checks are io's
 * responsibility: this parser never treats a pending file as a committed one.
 * Up to 100000 rows can be inspected; at that guard reservations fail closed. */
int nzm_ledger_parse(const char *text, size_t len, uint64_t now,
                     struct nzm_ledger *out);
void nzm_ledger_free(struct nzm_ledger *ledger);
/* Produces an allocated complete replacement preserving every record. Caller
 * frees text; input is never mutated. now must not regress below validated_at.
 * -EDQUOT = window exhausted, -E2BIG = 100k guard, -ENOMEM = allocation failure.
 * batch/daily are positive uint64 KiB; excess batch returns EDQUOT. No refund/pruning API exists.
 * out=NULL,len=0 on failure. Disk transaction/pending exclusion is io's job. */
int nzm_ledger_reserve_text(const struct nzm_ledger *ledger, uint64_t now,
                            uint64_t batch_kib, uint64_t daily_kib,
                            char **out, size_t *len);

#endif
