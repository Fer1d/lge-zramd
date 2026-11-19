#ifndef NZM_MEMORY_BUDGET_H
#define NZM_MEMORY_BUDGET_H

#include <stddef.h>
#include <stdint.h>

#define NZM_MEMORY_BUCKET_NS UINT64_C(900000000000)
#define NZM_MEMORY_WINDOW_NS UINT64_C(86400000000000)
#define NZM_MEMORY_MAX_BUCKETS 97u

struct nzm_memory_bucket {
    uint64_t index;
    uint64_t kib;
};

/* One daemon/worker owns this object. Fresh init deliberately resets all
 * accounting on daemon restart. No wear-ledger, IO or persistence dependency.
 * Treat fields as read-only outside this module. Stats are lifetime KiB for
 * this object: approved never refunds, known counts confirmed submissions,
 * unknown counts the full retained unknown reservation. pending_kib includes
 * that unknown reservation (do not add unknown_kib to pending_kib for usage).
 */
struct nzm_memory_budget {
    uint64_t last_ns;
    uint64_t approved_kib;
    uint64_t known_submitted_kib;
    uint64_t unknown_kib;
    uint64_t pending_kib;
    size_t count;
    int pending;
    int halted;
    struct nzm_memory_bucket buckets[NZM_MEMORY_MAX_BUCKETS];
};

/* All return 0 or negative errno. Failed calls leave object and outputs
 * unchanged. now_ns is caller-supplied CLOCK_BOOTTIME nanoseconds; every
 * successful time-taking call records it and rejects subsequent regressions.
 * Completed submissions expire 24 hours AFTER their 15-minute bucket ends.
 * Pending/unknown reservations never age. Only one reservation may exist.
 */
int nzm_memory_init(struct nzm_memory_budget *, uint64_t now_ns);
int nzm_memory_available(struct nzm_memory_budget *, uint64_t now_ns,
                         uint64_t daily_kib, uint64_t *available_kib);
/* Earliest absolute boottime where a new reservation can fit. required_kib is
 * rounded UP to a multiple of page_kib (zero means one page). Walks as many
 * expiry buckets as necessary, not merely the first. UINT64_MAX means no
 * representable due time, insufficient daily limit, pending, or halted.
 * A zero daily limit provides no credit; page_kib must be nonzero.
 */
int nzm_memory_due(struct nzm_memory_budget *, uint64_t now_ns,
                   uint64_t daily_kib, uint64_t required_kib,
                   uint64_t page_kib, uint64_t *due_ns);
int nzm_memory_reserve(struct nzm_memory_budget *, uint64_t now_ns,
                       uint64_t grant_kib, uint64_t daily_kib);
/* submitted_kib may be zero, partial, or full, including confirmed submissions
 * from an otherwise failed operation. Charge at COMPLETION, refund unused
 * reservation, clear pending. No request identity/retry semantics here.
 */
int nzm_memory_settle_known(struct nzm_memory_budget *, uint64_t now_ns,
                            uint64_t submitted_kib);
/* Unknown result permanently halts this object and retains its entire pending
 * reservation without aging. Further settlement/reservation is prohibited.
 */
int nzm_memory_retain_unknown(struct nzm_memory_budget *, uint64_t now_ns);

/* Optional production clock boundary, separate from all pure accounting APIs. */
int nzm_memory_now(uint64_t *boottime_ns);

#endif
