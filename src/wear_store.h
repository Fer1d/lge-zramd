#ifndef NZM_WEAR_STORE_H
#define NZM_WEAR_STORE_H
#include "wear_budget.h"
struct nzm_core;
#define NZM_WEAR_CHECKPOINT_NS UINT64_C(1800000000000)
/* All callbacks are injectable IO boundaries; no callback retries mutation.
 * read allocates exact original bytes (caller frees). backup must exclusively
 * create or verify the immutable exact original, fsync file and directory.
 * absent refuses exec.intent, ledger.new and ledger.txn, even owned intents. */
struct nzm_wear_store_ops {
  int (*absent)(void *);
  int (*sample)(void *, struct nzm_budget_sample *);
  int (*read)(void *, const char *, char **, size_t *);
  int (*backup)(void *, const char *, size_t);
  int (*commit)(void *, const char *, size_t);
  /* Settlement only: exact OWNED intent and no ledger transaction residue.
   * Ordinary loads/checkpoints always use absent, even with owned intents. */
  int (*owned)(void *, const struct nzm_budget *expected_pending);
};
int nzm_wear_store_load_ops(const struct nzm_wear_store_ops *, void *, int readonly,
    struct nzm_budget *, struct nzm_budget_sample *, int *migration_required);
int nzm_wear_store_checkpoint_ops(const struct nzm_wear_store_ops *, void *,
    uint64_t *next_deadline_ns);
/* Lock-held mutable load: durable v1/v2 migration or newboot reanchor.
 * Missing ledger is an error; never creates an empty ledger. */
int nzm_wear_store_load(struct nzm_core *, struct nzm_budget *, struct nzm_budget_sample *);
/* Read-only diagnostics: v1 is summarized conservatively, flagged for migration;
 * v2 projection is available via nzm_budget_budget. Never changes any file. */
int nzm_wear_store_read(struct nzm_core *, struct nzm_budget *, struct nzm_budget_sample *,
    int *migration_required);
int nzm_wear_store_checkpoint(struct nzm_core *, uint64_t *next_deadline_ns);
/* Narrow post-EXEC commit: requires exact persisted reservation snapshot and
 * owned intent. Never migrates, resamples, retries a mutation, or retires intent.
 * Exact persisted receipt is idempotent; stale/conflicting state is rejected. */
int nzm_wear_store_settle_ops(const struct nzm_wear_store_ops *, void *,
    const struct nzm_budget *expected_pending, const struct nzm_budget_sample *,
    uint64_t submitted_kib);
int nzm_wear_store_settle(struct nzm_core *, const struct nzm_budget *expected_pending,
    const struct nzm_budget_sample *, uint64_t submitted_kib);
#endif
