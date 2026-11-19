#define _POSIX_C_SOURCE 200809L
#include "memory_budget.h"

#include <errno.h>
#include <string.h>
#include <time.h>

/* Index comparisons preserve charges even when expiry nanoseconds overflow. */
static int expired(uint64_t index, uint64_t now_ns)
{
    uint64_t current = now_ns / NZM_MEMORY_BUCKET_NS;
    return current >= index && current - index >= NZM_MEMORY_MAX_BUCKETS;
}

static uint64_t expiry(uint64_t index)
{
    if (index > UINT64_MAX / NZM_MEMORY_BUCKET_NS - NZM_MEMORY_MAX_BUCKETS)
        return UINT64_MAX;
    return (index + NZM_MEMORY_MAX_BUCKETS) * NZM_MEMORY_BUCKET_NS;
}

static int valid(const struct nzm_memory_budget *b)
{
    uint64_t sum;
    size_t i;

    if (!b || b->count > NZM_MEMORY_MAX_BUCKETS ||
        (b->pending != 0 && b->pending != 1) ||
        (b->halted != 0 && b->halted != 1) ||
        b->pending != (b->pending_kib != 0) ||
        (b->halted && !b->pending) ||
        b->unknown_kib != (b->halted ? b->pending_kib : 0))
        return -EINVAL;
    if (b->pending_kib > b->approved_kib ||
        b->known_submitted_kib > b->approved_kib - b->pending_kib)
        return -EINVAL;
    sum = 0;
    for (i = 0; i < b->count; ++i) {
        const struct nzm_memory_bucket *bucket = &b->buckets[i];
        if (!bucket->kib || bucket->index > b->last_ns / NZM_MEMORY_BUCKET_NS ||
            expired(bucket->index, b->last_ns) ||
            (i && bucket->index <= b->buckets[i - 1].index))
            return -EINVAL;
        if (sum > UINT64_MAX - bucket->kib)
            return -ERANGE;
        sum += bucket->kib;
    }
    return sum > b->known_submitted_kib ? -EINVAL : 0;
}

static int advance(const struct nzm_memory_budget *b, uint64_t now_ns,
                   struct nzm_memory_budget *next)
{
    size_t i, n = 0;
    int rc = valid(b);

    if (rc)
        return rc;
    if (now_ns < b->last_ns)
        return -ERANGE;
    *next = *b;
    next->last_ns = now_ns;
    for (i = 0; i < b->count; ++i)
        if (!expired(b->buckets[i].index, now_ns))
            next->buckets[n++] = b->buckets[i];
    memset(next->buckets + n, 0,
           (NZM_MEMORY_MAX_BUCKETS - n) * sizeof next->buckets[0]);
    next->count = n;
    return 0;
}

/* valid() proves completed + pending <= approved, so this cannot overflow. */
static uint64_t used(const struct nzm_memory_budget *b)
{
    uint64_t sum = b->pending_kib;
    size_t i;
    for (i = 0; i < b->count; ++i)
        sum += b->buckets[i].kib;
    return sum;
}

int nzm_memory_init(struct nzm_memory_budget *b, uint64_t now_ns)
{
    struct nzm_memory_budget empty = {0};
    if (!b)
        return -EINVAL;
    empty.last_ns = now_ns;
    *b = empty;
    return 0;
}

int nzm_memory_available(struct nzm_memory_budget *b, uint64_t now_ns,
                         uint64_t daily_kib, uint64_t *available_kib)
{
    struct nzm_memory_budget next;
    uint64_t charge;
    int rc;
    if (!available_kib)
        return -EINVAL;
    if ((rc = advance(b, now_ns, &next)))
        return rc;
    charge = used(&next);
    *b = next;
    *available_kib = charge >= daily_kib ? 0 : daily_kib - charge;
    return 0;
}

int nzm_memory_due(struct nzm_memory_budget *b, uint64_t now_ns,
                   uint64_t daily_kib, uint64_t required_kib,
                   uint64_t page_kib, uint64_t *due_ns)
{
    struct nzm_memory_budget next;
    uint64_t charge, due = UINT64_MAX, remainder;
    size_t i;
    int rc;

    if (!due_ns || !page_kib)
        return -EINVAL;
    if (!required_kib)
        required_kib = page_kib;
    remainder = required_kib % page_kib;
    if (remainder) {
        uint64_t increment = page_kib - remainder;
        if (required_kib > UINT64_MAX - increment)
            return -ERANGE;
        required_kib += increment;
    }
    if ((rc = advance(b, now_ns, &next)))
        return rc;
    if (!next.pending && !next.halted && required_kib <= daily_kib) {
        charge = used(&next);
        if (charge <= daily_kib - required_kib) {
            due = now_ns;
        } else {
            for (i = 0; i < next.count; ++i) {
                charge -= next.buckets[i].kib;
                if (charge <= daily_kib - required_kib) {
                    due = expiry(next.buckets[i].index);
                    break;
                }
            }
        }
    }
    *b = next;
    *due_ns = due;
    return 0;
}

int nzm_memory_reserve(struct nzm_memory_budget *b, uint64_t now_ns,
                       uint64_t grant_kib, uint64_t daily_kib)
{
    struct nzm_memory_budget next;
    uint64_t charge;
    int rc;

    if (!grant_kib)
        return -EINVAL;
    if ((rc = advance(b, now_ns, &next)))
        return rc;
    if (next.halted)
        return -ESHUTDOWN;
    if (next.pending)
        return -EBUSY;
    charge = used(&next);
    if (charge >= daily_kib || grant_kib > daily_kib - charge)
        return -ENOSPC;
    if (next.approved_kib > UINT64_MAX - grant_kib)
        return -ERANGE;
    next.approved_kib += grant_kib;
    next.pending_kib = grant_kib;
    next.pending = 1;
    *b = next;
    return 0;
}

int nzm_memory_settle_known(struct nzm_memory_budget *b, uint64_t now_ns,
                            uint64_t submitted_kib)
{
    struct nzm_memory_budget next;
    uint64_t index = now_ns / NZM_MEMORY_BUCKET_NS;
    int rc;

    if ((rc = advance(b, now_ns, &next)))
        return rc;
    if (next.halted)
        return -ESHUTDOWN;
    if (!next.pending || submitted_kib > next.pending_kib)
        return -EINVAL;
    if (next.known_submitted_kib > UINT64_MAX - submitted_kib)
        return -ERANGE;
    if (submitted_kib) {
        if (next.count && next.buckets[next.count - 1].index == index) {
            struct nzm_memory_bucket *bucket = &next.buckets[next.count - 1];
            if (bucket->kib > UINT64_MAX - submitted_kib)
                return -ERANGE;
            bucket->kib += submitted_kib;
        } else {
            if (next.count == NZM_MEMORY_MAX_BUCKETS)
                return -ENOSPC;
            next.buckets[next.count].index = index;
            next.buckets[next.count++].kib = submitted_kib;
        }
    }
    next.known_submitted_kib += submitted_kib;
    next.pending_kib = 0;
    next.pending = 0;
    *b = next;
    return 0;
}

int nzm_memory_retain_unknown(struct nzm_memory_budget *b, uint64_t now_ns)
{
    struct nzm_memory_budget next;
    int rc;
    if ((rc = advance(b, now_ns, &next)))
        return rc;
    if (next.halted)
        return -ESHUTDOWN;
    if (!next.pending)
        return -EINVAL;
    next.halted = 1;
    next.unknown_kib = next.pending_kib;
    *b = next;
    return 0;
}

int nzm_memory_now(uint64_t *boottime_ns)
{
    struct timespec ts;
    uint64_t sec;
    if (!boottime_ns)
        return -EINVAL;
    if (clock_gettime(CLOCK_BOOTTIME, &ts))
        return -errno;
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L)
        return -ERANGE;
    sec = (uint64_t)ts.tv_sec;
    if (sec > (UINT64_MAX - (uint64_t)ts.tv_nsec) / UINT64_C(1000000000))
        return -ERANGE;
    *boottime_ns = sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
    return 0;
}
