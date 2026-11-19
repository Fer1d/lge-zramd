#ifndef NZM_RECOVERY_H
#define NZM_RECOVERY_H
#include <sys/types.h>
/* Caller holds the single startup lock for the entire call. verify rechecks
 * authority/full ledger/pause/pending, mounted identities, cold zram and no DM.
 * request only asks platform init to attempt mkdir; it conveys no readiness.
 * No rollback, repair, retry after storage error or history initialization. */
struct nzm_recovery_ops {
    int (*verify)(void *arg);
    int (*request)(void *arg);
    void *arg;
};
int nzm_recovery_owner(int data, const struct nzm_recovery_ops *ops, int *out);
/* Pure strict mountinfo parser shared by production and host tests. */
int nzm_mount_record(const char *text, const char *target, const char *fs,
                     dev_t device, unsigned mount_id);
int nzm_mounts_verify(void);
int nzm_persist_verify(void);
/* Conservative init/FBE precondition only, not proof of no inherited policy. */
int nzm_recovery_crypto_allowed(const char *type);
#endif
