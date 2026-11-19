#ifndef NZM_WEAR_BUDGET_H
#define NZM_WEAR_BUDGET_H
#include <stddef.h>
#include <stdint.h>

#define NZM_WEAR_BUCKET_NS UINT64_C(900000000000)
#define NZM_WEAR_WINDOW_NS UINT64_C(86400000000000)
#define NZM_WEAR_MAX_BUCKETS 97u
#define NZM_WEAR_MAX_BYTES 8192u
#define NZM_WEAR_BOOT_LEN 37u

/* nzm_budget_parse migration indicator: only v1 needs an exact original backup. */
enum nzm_budget_migration {
    NZM_BUDGET_CURRENT = 0,
    NZM_BUDGET_MIGRATED_V1 = 1,
    NZM_BUDGET_MIGRATED_V2 = 2
};
struct nzm_budget_sample {
    char boot[NZM_WEAR_BOOT_LEN]; /* Canonical lowercase UUID, NUL terminated. */
    uint64_t boottime_ns;
};
struct nzm_budget_bucket {
    uint64_t index; /* floor(logical_ns / NZM_WEAR_BUCKET_NS) */
    uint64_t kib;
};
struct nzm_budget_reservation {
    char boot[NZM_WEAR_BOOT_LEN];
    uint64_t request_id, generation, reserved_kib;
};
struct nzm_budget_receipt {
    struct nzm_budget_reservation reservation;
    uint64_t submitted_kib;
};
struct nzm_budget {
    uint64_t generation, cumulative_kib; /* Lifetime APPROVED KiB; never refunded. */
    char boot[NZM_WEAR_BOOT_LEN];
    uint64_t logical_ns, boottime_ns; /* Persisted anchor; not updated by project. */
    uint64_t next_request_id; /* 1 on a new boot; zero means exhausted. */
    size_t count;
    struct nzm_budget_bucket buckets[NZM_WEAR_MAX_BUCKETS];
    uint64_t known_submitted_kib; /* Only distinct, confirmed v3 settlements. */
    /* request_id==0 means absent; every other field must then be zero. */
    struct nzm_budget_reservation pending;
    struct nzm_budget_receipt receipt; /* Single most recent settlement. */
};

/* All operations return zero or negative errno. Failed state outputs are left
 * untouched; text outputs are reset to NULL/0 and are malloc-owned on success.
 * No IO, RTC, environment, locks or implicit clock reads. Single worker owner.
 * v3 header: wear-v3 GEN APPROVED BOOT LOGICAL_NS BOOTTIME_NS NEXT_ID COUNT KNOWN\n
 * rows: BUCKET_INDEX KIB\n (bounded 97), then exactly:
 * pending BOOT REQUEST GEN RESERVED\n
 * receipt BOOT REQUEST GEN RESERVED SUBMITTED\n
 * Absent records use '-' for BOOT and all zero numbers. Canonical numbers/UUID.
 */
int nzm_budget_init(const struct nzm_budget_sample *, struct nzm_budget *);
/* Current v3 and migrated v2 retain ALL persisted anchors/buckets/IDs/totals:
 * no projection, pruning or re-aging. v2 sets *migrated=2, known=0, no records.
 * v1 reuses the legacy parser, distrusts timestamps, moves ALL lifetime KiB to
 * the current bucket, preserves generation and sets *migrated=1. sample is
 * required only for v1. Caller preserves exact original v1 bytes as backup.
 * Both migration versions must be durably committed before any writeback. */
int nzm_budget_parse(const char *, size_t, const struct nzm_budget_sample *,
                     struct nzm_budget *, int *migrated);
/* Same boot adds ONLY delta from persisted anchor; a new boot freezes at
 * persisted logical time until reanchor, losing unpersisted tails. Rejects
 * regressions/overflow. Does not mutate anchor; repeated calls are safe. */
int nzm_budget_project(const struct nzm_budget *, const struct nzm_budget_sample *,
                       uint64_t *logical_ns);
/* Project/prune expired completion buckets and reset ID on a different boot.
 * Pending identity and receipt survive unchanged, including their old boots. */
int nzm_budget_reanchor(const struct nzm_budget *, const struct nzm_budget_sample *,
                        struct nzm_budget *);
/* Used = live completed buckets + FULL pending reservation, which NEVER ages.
 * next_expiry_ns concerns buckets only: zero if none, UINT64_MAX if overflow.
 * Availability saturates at zero if migrated lifetime exceeds daily_kib. */
int nzm_budget_budget(const struct nzm_budget *, const struct nzm_budget_sample *,
                      uint64_t daily_kib, uint64_t *used_kib,
                      uint64_t *available_kib, uint64_t *next_expiry_ns);
/* Reanchor/prune/serialize only; never clears pending or changes totals. */
int nzm_budget_checkpoint_text(const struct nzm_budget *, const struct nzm_budget_sample *,
                               struct nzm_budget *next, char **text, size_t *len);
/* Reanchor/prune, create pending (NO start bucket), increment generation and
 * lifetime APPROVED KiB, consume ID. Any pending returns -EBUSY across boots.
 * ID must equal projected next_request_id, never zero. UINT64_MAX is usable
 * once, then ID becomes zero. Caller MUST durably commit before execution. */
int nzm_budget_reserve_text(const struct nzm_budget *, const struct nzm_budget_sample *,
                            uint64_t batch_kib, uint64_t daily_kib,
                            uint64_t request_id, struct nzm_budget *next,
                            char **text, size_t *len);
/* Exact pending identity (all four fields) and submitted<=reserved required.
 * Actual settlement requires sample and ledger boots equal reservation boot.
 * Reanchor/prune, charge submitted at COMPLETION, clear pending, replace last
 * receipt, increment known_submitted_kib only. Zero is a valid known result.
 * Exact receipt+amount retry succeeds WITHOUT ANY state mutation/reanchor or
 * credit; conflicting amount/identity returns -EINVAL. Source state is pure.
 * Caller MUST durably commit settlement before any next reservation. Unknown
 * result MUST NOT settle: leave full nonaging pending and never retry work. */
int nzm_budget_settle_text(const struct nzm_budget *, const struct nzm_budget_sample *,
                           const struct nzm_budget_reservation *identity,
                           uint64_t submitted_kib, struct nzm_budget *next,
                           char **text, size_t *len);
/* Due on boot change, elapsed >= interval_ns, or first bucket expiry. */
int nzm_budget_checkpoint_due(const struct nzm_budget *, const struct nzm_budget_sample *,
                              uint64_t interval_ns, int *due);
/* Injectable optional production clock boundary (CLOCK_BOOTTIME, never RTC).
 * Caller supplies boot UUID obtained through its trusted startup IO. */
typedef int (*nzm_budget_sampler)(void *, uint64_t *boottime_ns);
int nzm_budget_clock_boottime(void *, uint64_t *boottime_ns);
int nzm_budget_sample_clock(const char *boot, nzm_budget_sampler, void *,
                            struct nzm_budget_sample *);
#endif
