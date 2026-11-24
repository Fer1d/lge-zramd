#ifndef NZM_RECOVERY_H
#define NZM_RECOVERY_H
#include <sys/types.h>
/* Caller holds the single startup lock for the entire call. verify rechecks
 * authority/pause, mounted identities, cold zram and no DM. Demand execution
 * intent remains a separate fence and is never cleared by preparation.
 * The controller only uses a directory already created by bootstrap.
 * No rollback, repair, retry after storage error or history initialization. */
struct nzm_recovery_ops {
    int (*verify)(void *arg);
    void *arg;
};
int nzm_recovery_owner(int parent, const struct nzm_recovery_ops *ops, int *out);
/* Read-only proof of an allowed container without owner or backing file. */
int nzm_recovery_unowned(int extm_fd);
int nzm_recovery_owner_read(int extm_fd);
/* Pure strict mountinfo parser shared by production and host tests. */
int nzm_mount_record(const char *text, const char *target, const char *fs,
                     dev_t device, unsigned mount_id);
/* Userdata only: exact source, root mount, partition alias and sysfs identity. */
int nzm_data_verify(void);
/* Open the exact vendor parent on that mount. Returns a CLOEXEC directory fd. */
int nzm_vendor_parent(void);
/* Prove that a pinned directory is still the named, same-device child. */
int nzm_bound_dir(int parent, const char *name, int fd);
#ifdef NZM_LEGACY_TEST
int nzm_mounts_verify(void);
int nzm_persist_verify(void);
#endif
/* Conservative init/FBE precondition only, not proof of no inherited policy. */
int nzm_recovery_crypto_allowed(const char *type);
#endif
