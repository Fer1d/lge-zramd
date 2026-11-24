#define _GNU_SOURCE
#include "state_bootstrap.h"
#include "nzm_internal.h"
#include "recovery.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/fscrypt.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
/* Tests replace only external metadata/probe boundaries; paths and creation
 * operations remain real descriptor-relative filesystem operations. */
#ifdef NZM_BOOTSTRAP_ADAPTER
#include NZM_BOOTSTRAP_ADAPTER
#elif defined(NZM_CORE_ADAPTER)
#include NZM_CORE_ADAPTER
#endif
#ifndef NZM_BOOTSTRAP_POLICY
#define NZM_BOOTSTRAP_POLICY unencrypted
static int unencrypted(int fd) {
  struct fscrypt_policy_v1 policy;
  memset(&policy, 0, sizeof policy);
  if (!ioctl(fd, FS_IOC_GET_ENCRYPTION_POLICY, &policy)) return -EPERM;
  /* Unsupported ioctl is not evidence of an unencrypted filesystem. */
  return errno == ENODATA ? 0 : -errno;
}
#endif
#ifndef NZM_BOOTSTRAP_CRYPTO
#define NZM_BOOTSTRAP_CRYPTO crypto
static int crypto(void) {
#ifdef __ANDROID__
  char type[PROP_VALUE_MAX];
  __system_property_get("ro.crypto.type", type);
  return nzm_recovery_crypto_allowed(type);
#else
  /* Host execution is unsupported, except through a compile-time test probe. */
  return -ENOTSUP;
#endif
}
#endif
static int cold(void) {
  int rc = nzm_cold();
  return rc ? rc : nzm_no_mapping();
}
static int directory(int parent, const char *name, const char *ctx, int create) {
  int fd = nzm_io_private_dir_at(parent, name, ctx);
  if (fd != -ENOENT || !create) return fd;
  /* Exclusive absence only. The creating domain must receive the exact
   * metadata and label; no chmod, chown, relabel, or cleanup. */
  int rc = cold();
  if (rc) return rc;
  if (mkdirat(parent, name, 0700)) return -errno;
  fd = nzm_io_private_dir_at(parent, name, ctx);
  if (fd >= 0 && (fsync(fd) || fsync(parent))) {
    rc = -errno; close(fd); return rc;
  }
  return fd;
}
int nzm_state_bound(int startupfd, int statefd) {
  if (startupfd < 0 || statefd < 0) return -EPERM;
  int parent = nzm_vendor_parent();
  if (parent < 0) return parent;
  const struct nzm_meta root_meta = {0, 0, 0700, 0, DCTX};
  const struct nzm_meta state_meta = {0, 0, 0700, 0, WCTX};
  int rc = nzm_io_check(startupfd, S_IFDIR, &root_meta);
  if (!rc) rc = nzm_io_check(statefd, S_IFDIR, &state_meta);
  if (!rc) rc = nzm_bound_dir(parent, "extm", startupfd);
  if (!rc) rc = nzm_bound_dir(startupfd, "state", statefd);
  close(parent);
  return rc;
}
static int open_state(int create, int lock, int *lockfd, int *statefd) {
  int parent = nzm_vendor_parent();
  if (parent < 0) return parent;
  int rc = create ? NZM_BOOTSTRAP_POLICY(parent) : 0;
  int extm = rc ? rc : directory(parent, "extm", DCTX, create);
  if (extm < 0) { close(parent); return extm; }
  if (lock && flock(extm, LOCK_EX | LOCK_NB)) rc = -errno;
  if (!rc) rc = nzm_bound_dir(parent, "extm", extm);
  if (!rc && create) rc = NZM_BOOTSTRAP_POLICY(extm);
  int state = rc ? -1 : directory(extm, "state", WCTX, create);
  if (!rc && state < 0) rc = state;
  if (!rc) rc = nzm_state_bound(extm, state);
  if (!rc && create) rc = NZM_BOOTSTRAP_POLICY(state);
  if (!rc && create && fsync(extm)) rc = -errno;
  close(parent);
  if (rc) { if (state >= 0) close(state); close(extm); return rc; }
  *lockfd = extm;
  *statefd = state;
  return 0;
}
int nzm_state_open(int *startupfd, int *statefd) {
  if (!startupfd || !statefd) return -EINVAL;
  *startupfd = *statefd = -1;
  return open_state(0, 1, startupfd, statefd);
}
int nzm_state_readonly(void) {
  int extm, state;
  int rc = open_state(0, 0, &extm, &state);
  if (rc) return rc;
  close(extm);
  return state;
}
int nzm_state_bootstrap(void) {
  /* Exact creation modes; never repair an existing object with setattr. */
  mode_t saved_umask = umask(0);
  int rc = NZM_BOOTSTRAP_CRYPTO();
  if (!rc) rc = cold();
  if (rc) { umask(saved_umask); return rc; }
  int extm = -1, state = -1;
  rc = open_state(1, 1, &extm, &state);
  if (rc) { umask(saved_umask); return rc; }
  /* Repeat cold ownership checks while holding the shared startup lock. */
  rc = cold();
  if (!rc) rc = nzm_state_bound(extm, state);
  static const char record[] = "extm-wear-v1\n";
  const struct nzm_meta meta = {0, 0, 0600, 1,
                              "u:object_r:extm_wear_authority:s0"};
  int fd = -1;
  if (!rc) {
    fd = openat(state, "authority", O_WRONLY | O_CREAT | O_EXCL |
                O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    if (fd < 0 && errno != EEXIST) rc = -errno;
    else if (fd >= 0) {
      rc = nzm_io_check(fd, S_IFREG, &meta);
      if (!rc) rc = nzm_io_sysfs_write(fd, record, sizeof record - 1);
      if (!rc && fsync(fd)) rc = -errno;
      close(fd);
      if (!rc && fsync(state)) rc = -errno;
    }
  }
  /* Existing authority is never overwritten; partial writes remain poison. */
  struct nzm_boot_state boot;
  nzm_boot_init(&boot);
  if (!rc) rc = nzm_state_bound(extm, state);
  if (!rc) rc = nzm_boot_lock(&boot, state);
  nzm_boot_close(&boot);
  close(state);
  close(extm);
  umask(saved_umask);
  return rc;
}
