#include "memory_budget.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); abort(); \
} } while (0)

static void available(struct nzm_memory_budget *b, uint64_t now,
                      uint64_t daily, uint64_t expected)
{
    uint64_t result = UINT64_MAX;
    CHECK(!nzm_memory_available(b, now, daily, &result));
    CHECK(result == expected);
}

static void due(struct nzm_memory_budget *b, uint64_t now, uint64_t daily,
                uint64_t required, uint64_t page, uint64_t expected)
{
    uint64_t result = 123;
    CHECK(!nzm_memory_due(b, now, daily, required, page, &result));
    CHECK(result == expected);
}

static void complete(struct nzm_memory_budget *b, uint64_t now,
                     uint64_t grant, uint64_t submitted, uint64_t daily)
{
    CHECK(!nzm_memory_reserve(b, now, grant, daily));
    CHECK(!nzm_memory_settle_known(b, now, submitted));
}

static void results_and_restart(void)
{
    struct nzm_memory_budget b, before;
    CHECK(!nzm_memory_init(&b, 0));
    CHECK(!b.approved_kib && !b.known_submitted_kib && !b.unknown_kib);
    CHECK(!b.pending_kib && !b.pending && !b.halted && !b.count);
    available(&b, 0, 100, 100);
    available(&b, 0, 0, 0);
    due(&b, 0, 100, 0, 4, 0);
    complete(&b, 0, 20, 0, 100); /* Known zero: no completion bucket. */
    CHECK(!b.count && b.approved_kib == 20 && !b.known_submitted_kib);
    complete(&b, 1, 20, 8, 100); /* Partial completion refunds 12. */
    available(&b, 1, 100, 92);
    complete(&b, 2, 20, 20, 100); /* Full completion. */
    /* A failed executor with confirmed submissions uses the same known API. */
    complete(&b, 3, 20, 12, 100);
    CHECK(b.approved_kib == 80 && b.known_submitted_kib == 40);
    CHECK(b.count == 1 && b.buckets[0].kib == 40);
    CHECK(!b.pending && !b.pending_kib);
    before = b;
    CHECK(nzm_memory_settle_known(&b, 3, 12) == -EINVAL);
    CHECK(!memcmp(&b, &before, sizeof b)); /* No double settlement. */
    CHECK(!nzm_memory_reserve(&b, 4, 60, 100));
    available(&b, 4, 100, 0);
    before = b;
    CHECK(nzm_memory_reserve(&b, 4, 1, 100) == -EBUSY);
    CHECK(nzm_memory_settle_known(&b, 4, 61) == -EINVAL);
    CHECK(!memcmp(&b, &before, sizeof b));
    CHECK(!nzm_memory_settle_known(&b, 4, 60));
    CHECK(b.approved_kib == 140 && b.known_submitted_kib == 100);
    before = b;
    CHECK(nzm_memory_reserve(&b, 4, 1, 100) == -ENOSPC);
    CHECK(!memcmp(&b, &before, sizeof b));
    /* A fresh daemon gets a fresh object even at identical uptime. */
    CHECK(!nzm_memory_init(&b, 4));
    CHECK(!b.approved_kib && !b.known_submitted_kib && !b.count);
    available(&b, 4, 100, 100);
    complete(&b, 4, 100, 100, 100);
}

static void boundaries_and_completion(void)
{
    struct nzm_memory_budget b;
    const uint64_t bucket = NZM_MEMORY_BUCKET_NS, window = NZM_MEMORY_WINDOW_NS;
    CHECK(!nzm_memory_init(&b, 0));
    complete(&b, 0, 10, 10, 100);
    complete(&b, bucket - 1, 20, 20, 100);
    complete(&b, bucket, 40, 40, 100);
    CHECK(b.count == 2 && b.buckets[0].kib == 30);
    available(&b, window, 100, 30); /* Never expires at start + 24h. */
    available(&b, window + bucket - 1, 100, 30);
    available(&b, window + bucket, 100, 60);
    CHECK(b.count == 1 && b.buckets[0].index == 1);
    available(&b, window + 2 * bucket, 100, 100);
    CHECK(!b.count && b.approved_kib == 70 && b.known_submitted_kib == 70);

    CHECK(!nzm_memory_init(&b, 0));
    CHECK(!nzm_memory_reserve(&b, 0, 100, 100));
    available(&b, 3 * window, 100, 0); /* In-flight work never ages. */
    CHECK(!b.count && b.pending_kib == 100);
    CHECK(!nzm_memory_settle_known(&b, 3 * window, 40));
    CHECK(b.count == 1 && b.buckets[0].index == 288);
    available(&b, 4 * window, 100, 60);
    available(&b, 4 * window + bucket - 1, 100, 60);
    available(&b, 4 * window + bucket, 100, 100);
}

static void required_amount(void)
{
    struct nzm_memory_budget b;
    const uint64_t bucket = NZM_MEMORY_BUCKET_NS;
    CHECK(!nzm_memory_init(&b, 0));
    complete(&b, 0, 10, 10, 100);
    complete(&b, bucket, 20, 20, 100);
    complete(&b, 2 * bucket, 30, 30, 100);
    complete(&b, 3 * bucket, 35, 35, 100); /* 5 KiB left. */
    due(&b, 3 * bucket, 100, 0, 4, 3 * bucket);
    due(&b, 3 * bucket, 100, 5, 4, 97 * bucket); /* round 5 to 8 */
    due(&b, 3 * bucket, 100, 16, 4, 98 * bucket); /* skips first expiry */
    due(&b, 3 * bucket, 100, 40, 4, 99 * bucket); /* three buckets */
    due(&b, 3 * bucket, 100, 68, 4, 100 * bucket); /* all four */
    due(&b, 3 * bucket, 100, 101, 4, UINT64_MAX);
    due(&b, 3 * bucket, 100, 0, 128, UINT64_MAX);
    due(&b, 3 * bucket, 0, 0, 4, UINT64_MAX);
    /* Smaller configured daily budget must not erase old charges. */
    available(&b, 3 * bucket, 20, 0);
    due(&b, 3 * bucket, 20, 4, 4, 100 * bucket);
    due(&b, 98 * bucket, 100, 16, 4, 98 * bucket);
    available(&b, 98 * bucket, 100, 35);
    CHECK(!nzm_memory_reserve(&b, 98 * bucket, 4, 100));
    due(&b, 98 * bucket, 100, 4, 4, UINT64_MAX); /* One worker, not just credit. */
    due(&b, 1000 * bucket, 100, 4, 4, UINT64_MAX);
    CHECK(b.pending_kib == 4 && !b.count);
    CHECK(!nzm_memory_settle_known(&b, 1000 * bucket, 0));
    due(&b, 1000 * bucket, 100, 4, 4, 1000 * bucket);
    /* Non-power-of-two page sizes still round safely and correctly. */
    complete(&b, 1000 * bucket, 93, 93, 100);
    due(&b, 1000 * bucket, 100, 7, 3, 1097 * bucket);
}

static void unknown_never_ages(void)
{
    struct nzm_memory_budget b, before;
    const uint64_t window = NZM_MEMORY_WINDOW_NS;
    CHECK(!nzm_memory_init(&b, 0));
    complete(&b, 0, 20, 20, 100);
    CHECK(!nzm_memory_reserve(&b, 1, 60, 100));
    CHECK(!nzm_memory_retain_unknown(&b, 2));
    CHECK(b.halted && b.pending && b.pending_kib == 60 && b.unknown_kib == 60);
    CHECK(b.approved_kib == 80 && b.known_submitted_kib == 20);
    available(&b, 2, 100, 20); /* Unknown is not double-counted. */
    due(&b, 2, 100, 4, 4, UINT64_MAX);
    available(&b, 100 * window, 100, 40);
    CHECK(!b.count && b.pending_kib == 60 && b.unknown_kib == 60);
    before = b;
    CHECK(nzm_memory_reserve(&b, 100 * window, 4, 100) == -ESHUTDOWN);
    CHECK(nzm_memory_settle_known(&b, 100 * window, 0) == -ESHUTDOWN);
    CHECK(nzm_memory_retain_unknown(&b, 100 * window) == -ESHUTDOWN);
    CHECK(!memcmp(&b, &before, sizeof b));
    available(&b, UINT64_MAX, 100, 40);
    CHECK(b.halted && b.pending && b.unknown_kib == 60);
    CHECK(!nzm_memory_init(&b, UINT64_MAX));
    CHECK(!b.halted && !b.pending && !b.unknown_kib && !b.approved_kib);
    available(&b, UINT64_MAX, 100, 100);
}

static void long_run(void)
{
    struct nzm_memory_budget b;
    size_t maximum = 0;
    uint64_t i;
    CHECK(!nzm_memory_init(&b, 0));
    for (i = 0; i < 100 * 96 * 4; ++i) {
        uint64_t now = i * (NZM_MEMORY_BUCKET_NS / 4);
        complete(&b, now, 1, 1, 1000);
        CHECK(b.count <= NZM_MEMORY_MAX_BUCKETS);
        CHECK(!b.pending && !b.halted);
        if (b.count > maximum)
            maximum = b.count;
    }
    CHECK(maximum == 97 && b.count == 97);
    CHECK(b.approved_kib == 38400 && b.known_submitted_kib == 38400);
    available(&b, b.last_ns, 1000, 612);
    available(&b, b.last_ns + 2 * NZM_MEMORY_WINDOW_NS, 1000, 1000);
    CHECK(!b.count && b.approved_kib == 38400 && b.known_submitted_kib == 38400);
}

static void errors_and_overflow(void)
{
    struct nzm_memory_budget b, before;
    uint64_t out = 123;
    const uint64_t bucket = NZM_MEMORY_BUCKET_NS;
    CHECK(nzm_memory_init(NULL, 0) == -EINVAL);
    CHECK(!nzm_memory_init(&b, 100));
    complete(&b, 100, 4, 4, 100);
    CHECK(!nzm_memory_reserve(&b, 100, 4, 100));
    before = b;
    CHECK(nzm_memory_available(&b, 99, 100, &out) == -ERANGE);
    CHECK(out == 123);
    CHECK(nzm_memory_due(&b, 99, 100, 4, 4, &out) == -ERANGE);
    CHECK(nzm_memory_reserve(&b, 99, 4, 100) == -ERANGE);
    CHECK(nzm_memory_settle_known(&b, 99, 0) == -ERANGE);
    CHECK(nzm_memory_retain_unknown(&b, 99) == -ERANGE);
    CHECK(!memcmp(&b, &before, sizeof b) && out == 123);
    CHECK(nzm_memory_available(&b, 101, 100, NULL) == -EINVAL);
    CHECK(nzm_memory_due(&b, 101, 100, 4, 0, &out) == -EINVAL);
    CHECK(nzm_memory_due(&b, 101, 100, 4, 4, NULL) == -EINVAL);
    CHECK(nzm_memory_reserve(&b, 101, 0, 100) == -EINVAL);
    CHECK(nzm_memory_due(&b, 101, UINT64_MAX, UINT64_MAX, 4, &out) == -ERANGE);
    CHECK(!memcmp(&b, &before, sizeof b) && out == 123);
    CHECK(!nzm_memory_settle_known(&b, 100, 0));
    before = b;
    CHECK(nzm_memory_retain_unknown(&b, 101) == -EINVAL);
    CHECK(nzm_memory_reserve(&b, 101, 1, 0) == -ENOSPC);
    CHECK(!memcmp(&b, &before, sizeof b));
    /* Successful queries establish the same regression floor as mutations. */
    available(&b, 102, 100, 96);
    before = b;
    CHECK(nzm_memory_reserve(&b, 101, 1, 100) == -ERANGE);
    CHECK(!memcmp(&b, &before, sizeof b));

    CHECK(!nzm_memory_init(&b, 0));
    complete(&b, 0, UINT64_MAX, UINT64_MAX, UINT64_MAX);
    available(&b, 0, UINT64_MAX, 0);
    available(&b, 97 * bucket, UINT64_MAX, UINT64_MAX);
    before = b;
    CHECK(nzm_memory_reserve(&b, 97 * bucket, 1, UINT64_MAX) == -ERANGE);
    CHECK(!memcmp(&b, &before, sizeof b)); /* Lifetime totals never wrap. */
    CHECK(!nzm_memory_init(&b, 0));
    complete(&b, 0, UINT64_MAX - 4, 0, UINT64_MAX);
    complete(&b, 1, 4, 4, UINT64_MAX);
    CHECK(b.approved_kib == UINT64_MAX && b.known_submitted_kib == 4);

    /* Last representable bucket: expiry overflow must never erase a charge. */
    CHECK(!nzm_memory_init(&b, UINT64_MAX - 1));
    complete(&b, UINT64_MAX - 1, 10, 10, 10);
    due(&b, UINT64_MAX - 1, 10, 1, 1, UINT64_MAX);
    available(&b, UINT64_MAX, 10, 0);
    CHECK(b.count == 1 && b.buckets[0].kib == 10);
    /* Invalid internal state also fails transactionally. */
    b.count = 98;
    before = b;
    CHECK(nzm_memory_available(&b, UINT64_MAX, 10, &out) == -EINVAL);
    CHECK(!memcmp(&b, &before, sizeof b) && out == 123);
    CHECK(nzm_memory_available(NULL, 0, 10, &out) == -EINVAL);
    CHECK(nzm_memory_now(NULL) == -EINVAL);
    CHECK(!nzm_memory_now(&out));
}

int main(void)
{
    results_and_restart();
    boundaries_and_completion();
    required_amount();
    unknown_never_ages();
    long_run();
    errors_and_overflow();
    printf("memory_budget: %u checks passed\n", checks);
    return 0;
}
