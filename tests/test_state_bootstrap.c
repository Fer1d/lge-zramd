#define _GNU_SOURCE
#include "nzm_internal.h"
#include "state_bootstrap.h"
#include "recovery.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

/* Real temporary directories/files/locks, mocked external mount, crypto,
 * fscrypt and SELinux boundaries. No runtime path override in production. */
static int parentfd, mount_error, cold_error, dm_error, crypto_error;
static int policy_error_at, policy_error, metadata_error, write_error, policy_calls, writes;
static int test_policy(int fd) {
  struct stat st;
  assert(!fstat(fd, &st) && S_ISDIR(st.st_mode));
  ++policy_calls;
  return policy_calls == policy_error_at ? policy_error : 0;
}
static int test_crypto(void) { return crypto_error; }
#define NZM_BOOTSTRAP_POLICY test_policy
#define NZM_BOOTSTRAP_CRYPTO test_crypto
#include "../src/state_bootstrap.c"
int nzm_vendor_parent(void) { return mount_error ? mount_error : dup(parentfd); }
int nzm_cold(void) { return cold_error; }
int nzm_no_mapping(void) { return dm_error; }
int nzm_bound_dir(int parent, const char *name, int fd) {
  struct stat named, held;
  if (fstatat(parent, name, &named, AT_SYMLINK_NOFOLLOW) || fstat(fd, &held)) return -errno;
  return named.st_dev == held.st_dev && named.st_ino == held.st_ino ? 0 : -ESTALE;
}
int nzm_io_check(int fd, mode_t type, const struct nzm_meta *m) {
  struct stat st;
  assert(m->context && !strncmp(m->context, "u:object_r:extm_", 16));
  if (fstat(fd, &st)) return -errno;
  return metadata_error || (st.st_mode & S_IFMT) != type ||
      (st.st_mode & 07777) != m->mode ||
      (m->links && st.st_nlink != m->links) ? -EPERM : 0;
}
int nzm_io_private_dir_at(int p, const char *name, const char *ctx) {
  int fd = openat(p, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return -errno;
  const struct nzm_meta m = {0, 0, 0700, 0, ctx};
  int rc = nzm_io_check(fd, S_IFDIR, &m);
  if (rc) { close(fd); return rc; }
  return fd;
}
int nzm_io_sysfs_write(int fd, const void *b, size_t n) {
  ++writes;
  ssize_t got = write(fd, b, write_error ? n / 2 : n);
  return got < 0 ? -errno : (size_t)got != n ? -EIO : 0;
}
int nzm_io_read_at(int d, const char *name, const struct nzm_meta *m,
                   void *b, size_t cap, size_t *len) {
  int fd = openat(d, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return -errno;
  int rc = nzm_io_check(fd, S_IFREG, m);
  ssize_t n = rc ? -1 : read(fd, b, cap);
  if (!rc && n < 0) rc = -errno;
  if (!rc) *len = (size_t)n;
  close(fd);
  return rc;
}
int nzm_io_ledger_preflight(int fd, size_t n) { (void)fd; (void)n; return 0; }
static void fixture(void) {
  char p[] = "/tmp/nzm-state-bootstrap-XXXXXX";
  assert(mkdtemp(p));
  if (parentfd > 0) close(parentfd);
  parentfd = open(p, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  assert(parentfd >= 0);
  mount_error = cold_error = dm_error = crypto_error = policy_error = 0;
  metadata_error = write_error = policy_calls = writes = policy_error_at = 0;
}
static int exists(const char *name) {
  struct stat st;
  return !fstatat(parentfd, name, &st, AT_SYMLINK_NOFOLLOW);
}
int main(void) {
  fixture();
  int extm, state;
  assert(nzm_state_open(&extm, &state) == -ENOENT);
  assert(!exists("extm")); /* daemon cannot create */
  mount_error = -ENODEV;
  assert(nzm_state_bootstrap() == -ENODEV && !exists("extm"));
  mount_error = 0; cold_error = -EBUSY;
  assert(nzm_state_bootstrap() == -EBUSY && !exists("extm"));
  cold_error = 0; dm_error = -EEXIST;
  assert(nzm_state_bootstrap() == -EEXIST && !exists("extm"));
  dm_error = 0; crypto_error = -EPERM;
  assert(nzm_state_bootstrap() == -EPERM && !exists("extm"));
  crypto_error = 0;
  assert(!nzm_state_bootstrap() && policy_calls == 3 && writes == 1);
  assert(!nzm_state_bootstrap() && writes == 1); /* existing never rewritten */
  assert(!nzm_state_open(&extm, &state));
  int other, other_state;
  assert(nzm_state_open(&other, &other_state) == -EWOULDBLOCK);
  assert(nzm_state_bootstrap() == -EWOULDBLOCK);
  struct nzm_boot_state boot;
  nzm_boot_init(&boot);
  assert(!nzm_boot_lock(&boot, state));
  nzm_boot_close(&boot); close(state); close(extm);
  assert(!nzm_state_open(&extm, &state));
  int readonly = nzm_state_readonly();
  assert(readonly >= 0); close(readonly);
  assert(policy_calls == 7); /* No daemon/diagnostic fscrypt ioctl. */
  assert(!renameat(extm, "state", extm, "old-state"));
  assert(!mkdirat(extm, "state", 0700));
  assert(nzm_state_bound(extm, state) == -ESTALE);
  assert(!unlinkat(extm, "state", AT_REMOVEDIR));
  assert(!renameat(extm, "old-state", extm, "state"));
  assert(!renameat(parentfd, "extm", parentfd, "old-extm"));
  assert(!mkdirat(parentfd, "extm", 0700));
  assert(nzm_state_bound(extm, state) == -ESTALE);
  assert(!unlinkat(parentfd, "extm", AT_REMOVEDIR));
  assert(!renameat(parentfd, "old-extm", parentfd, "extm"));
  metadata_error = 1;
  assert(nzm_state_bootstrap() == -EPERM);
  metadata_error = 0; close(state); close(extm);

  fixture(); policy_error_at = 1; policy_error = -ENOTTY;
  assert(nzm_state_bootstrap() == -ENOTTY && !exists("extm"));
  fixture(); policy_error_at = 2; policy_error = -ENOTTY;
  assert(nzm_state_bootstrap() == -ENOTTY);
  assert(exists("extm") && !exists("extm/state"));
  fixture(); policy_error_at = 3; policy_error = -EPERM;
  assert(nzm_state_bootstrap() == -EPERM && exists("extm/state") && !writes);
  fixture(); write_error = 1;
  assert(nzm_state_bootstrap() == -EIO);
  write_error = 0;
  assert(nzm_state_bootstrap() == -EPERM && writes == 1); /* partial poison */
  fixture();
  assert(!symlinkat("/tmp", parentfd, "extm"));
  assert(nzm_state_bootstrap() < 0 && !exists("extm/state"));
  fixture();
  assert(!mkdirat(parentfd, "extm", 0755));
  assert(nzm_state_bootstrap() == -EPERM); /* existing metadata is not repaired */
  struct stat st;
  assert(!fstatat(parentfd, "extm", &st, AT_SYMLINK_NOFOLLOW));
  assert((st.st_mode & 07777) == 0755);
  fixture();
  assert(!mkdirat(parentfd, "extm", 0700));
  int d = openat(parentfd, "extm", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  assert(d >= 0 && !symlinkat("/tmp", d, "state"));
  assert(nzm_state_bootstrap() < 0 && !writes);
  close(d);
  close(parentfd);
  puts("state bootstrap tests passed");
  return 0;
}
