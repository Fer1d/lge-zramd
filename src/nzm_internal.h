#ifndef NZM_INTERNAL_H
#define NZM_INTERNAL_H

/* Private core-module declarations; nzm_core.h remains the public interface. */
#include "nzm_core.h"
#include "runtime.h"

#define MIB UINT64_C(1048576)
#define WEAR "/mnt/vendor/persist/lge-zram-wear"
#define Z "/sys/block/zram0/"
#define RCTX "u:object_r:lge_zram_runtime_file:s0"
#define DCTX "u:object_r:extm_root_data:s0"
#define OCTX "u:object_r:extm_owner_data:s0"
#define FCTX "u:object_r:extm_backing_data:s0"
#define WCTX "u:object_r:extm_wear_dir:s0"
#define LCTX "u:object_r:extm_wear_ledger:s0"
#define PCTX "u:object_r:extm_wear_pause:s0"
#define NZM_PAUSE_RECORD "native-v1 persistent pause\n"

struct nzm_cycle_policy { int watermark; uint64_t idle, interval; };

int nzm_sleep_seconds(uint64_t seconds);
int nzm_core_control(struct nzm_core *c, enum nzm_runtime_boundary boundary);
int nzm_core_observe(struct nzm_core *c, uint64_t seconds);
int nzm_core_ready(struct nzm_core *c);
/* Trusted read-only boot identity; does not initialize startup ownership. */
int nzm_core_bootid(struct nzm_core *c);
int nzm_directory(const char *path);
int nzm_private(const char *path, const char *ctx);
int nzm_rawread(const char *p, char *b, size_t cap);
int nzm_text(int fd, const char *name, const char *ctx, char *b,
                size_t cap);
int nzm_absent(int fd, const char *name);
int nzm_create_text(int fd, const char *name, const char *ctx,
                       const char *s);
int nzm_core_authority(struct nzm_core *c);
int nzm_state_absent(struct nzm_core *c, const char *name);
int nzm_state_text(struct nzm_core *c, const char *name, char *b, size_t cap);
int nzm_mark(struct nzm_core *c, const char *name, const char *s);
int nzm_put(const char *p, const char *s);
int nzm_putnum(const char *p, uint64_t v);
int nzm_number(const char *p, uint64_t *v);
int nzm_fields(const char *p, uint64_t *v, size_t count);
uint64_t nzm_nowtime(void);
int nzm_monotonic_now(uint64_t *tick);
#ifdef NZM_LEGACY_TEST
int nzm_wear(struct nzm_core *c, struct nzm_ledger *l);
#endif
int nzm_pause_verified(struct nzm_core *c, int *paused);
int nzm_phase1_policy(struct nzm_core *c, int *paused);
int nzm_identity(struct nzm_core *c, struct nzm_helper_identity *id);
int nzm_helper(struct nzm_core *c, enum nzm_helper_op op,
                  struct nzm_helper_identity *id, struct nzm_helper_result *r);
int nzm_managed(struct nzm_core *c);
int nzm_no_mapping(void);
int nzm_active(void);
int nzm_cold(void);
int nzm_activate(struct nzm_core *c);
int nzm_ram_boot(struct nzm_core *c);
int nzm_existing_identity(struct nzm_core *c, struct nzm_helper_identity *id, int fallback);
int nzm_dm_boot(struct nzm_core *c);
int nzm_dm_quota(void);
int nzm_dm_bound(const struct nzm_helper_identity *id);
int nzm_dm_activate(struct nzm_core *c, const struct nzm_helper_identity *id);
int nzm_space(struct nzm_core *c);
int nzm_health(struct nzm_core *c);
int nzm_power_policy(int *off, int *charging);
#ifdef NZM_LEGACY_TEST
int nzm_elapsed(uint64_t now,uint64_t since,uint64_t interval);
int nzm_sample_plan(struct nzm_core *c, struct nzm_cycle_policy *policy, struct nzm_plan *plan);
#endif
/* Core-owned synchronous execution adapter, not part of frozen transport.
 * sample/precheck are read-only. EAGAIN means global quota wait, not fallback.
 * execute requires the durable planned-KiB reservation; never retries EXEC. */
int nzm_intent_check(struct nzm_core *c);
int nzm_intent_create(struct nzm_core *c, const struct nzm_plan *plan);
int nzm_intent_retire(struct nzm_core *c);
int nzm_execution_select(struct nzm_core *c);
int nzm_execution_sample(struct nzm_core *c, uint64_t mm[10], uint64_t bd[3],
                         uint64_t *disk, uint64_t *page);
int nzm_execution_precheck(struct nzm_core *c, const struct nzm_plan *plan);
int nzm_execution_execute(struct nzm_core *c, const struct nzm_plan *plan);
const char *nzm_execution_name(const struct nzm_core *c);
int nzm_boundary(struct nzm_core *c);
int nzm_diag(struct nzm_core *c, const char *stage, int rc);
int nzm_diag_config_error(const char *stage, const char *field, const char *reason, int rc);
int nzm_stop(struct nzm_core *c, const char *stage, int rc);
int nzm_waiting(struct nzm_core *c, const char *reason);
void nzm_config_snapshot(const struct nzm_core *c, FILE *out, int startup);

#endif
