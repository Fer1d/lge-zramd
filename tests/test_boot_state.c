#define _GNU_SOURCE
#include "boot_state.h"
#include "nzm_io.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

/* Metadata boundary is modeled here; real bounded files, flock, fsync, rename
 * and crash residues exercise the production checkpoint implementation. */
static int short_write, no_space, bad_meta;
int nzm_io_check(int fd, mode_t type, const struct nzm_meta *m) {
  struct stat st;
  assert(m->context && !strncmp(m->context, "u:object_r:extm_wear_", 21));
  if (fstat(fd, &st)) return -errno;
  return bad_meta || (st.st_mode & S_IFMT) != type || (st.st_mode & 0777) != m->mode ||
      (m->links && st.st_nlink != m->links) ? -EPERM : 0;
}
int nzm_io_read_at(int d, const char *name, const struct nzm_meta *m,
                   void *b, size_t cap, size_t *len) {
  int fd = openat(d, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) return -errno;
  int rc = nzm_io_check(fd, S_IFREG, m);
  ssize_t n = -1;
  if (!rc) {
    n = read(fd, b, cap);
    if (n < 0) rc = -errno;
    else { char extra; if (read(fd, &extra, 1)) rc = -EOVERFLOW; }
  }
  close(fd);
  if (!rc) *len = (size_t)n;
  return rc;
}
int nzm_io_sysfs_write(int fd, const void *b, size_t len) {
  ssize_t n = write(fd, b, short_write ? len / 2 : len);
  return n < 0 ? -errno : (size_t)n != len ? -EIO : 0;
}
int nzm_io_ledger_preflight(int d, size_t len) {
  assert(d >= 0 && len && len <= 8192);
  return no_space ? -ENOSPC : 0;
}
static void put(int d, const char *name, const char *text) {
  int fd = openat(d, name, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  assert(fd >= 0);
  assert(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
  assert(!close(fd));
}
static int fixture(void) {
  char path[] = "/tmp/nzm-boot-state-XXXXXX";
  assert(mkdtemp(path));
  int d = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  assert(d >= 0);
  put(d, "authority", "extm-wear-v1\n");
  return d;
}
int main(void) {
  const char *boot = "01234567-89ab-cdef-0123-456789abcdef";
  int d = fixture();
  struct nzm_boot_state s, other;
  nzm_boot_init(&s); nzm_boot_init(&other);
  assert(!nzm_boot_lock(&s, d));
  assert(nzm_boot_lock(&other, d) == -EWOULDBLOCK);
  assert(nzm_boot_load(&s, d) == -ENOENT);
  no_space = 1;
  assert(nzm_boot_reset(&s, boot, "1 2") == -ENOSPC && !s.poisoned);
  assert(faccessat(d, "boot-state.txn", F_OK, 0) && errno == ENOENT);
  no_space = 0;
  assert(!nzm_boot_reset(&s, boot, "1 2"));
  assert(nzm_boot_absent(&s, "last-event") == -EINVAL);
  assert(!nzm_boot_mark(&s, "minfree", "100 200\n"));
  assert(!nzm_boot_mark(&s, "vm-attempted-boot", "attempted\n"));
  assert(nzm_boot_mark(&s, "minfree", "200 400\n") == -EEXIST);
  assert(!nzm_boot_load(&other, d));
  char value[256];
  assert(!nzm_boot_text(&other, "minfree", value, sizeof value));
  assert(!strcmp(value, "100 200\n"));
  assert(nzm_boot_text(&other, "vm-complete-boot", value, sizeof value) == -ENOENT);
  /* Simulated lost owner: attempted-only evidence survives and isn't repaired. */
  nzm_boot_close(&s);
  assert(!nzm_boot_lock(&other, d));
  assert(!nzm_boot_mark(&other, "vm-complete-boot", "verified\n"));
  short_write = 1;
  assert(nzm_boot_mark(&other, "vm-attempted-completed", "attempted\n") == -EIO);
  assert(other.poisoned);
  short_write = 0;
  assert(nzm_boot_mark(&other, "vm-attempted-completed", "attempted\n") < 0);
  nzm_boot_close(&other);
  nzm_boot_init(&s);
  assert(!nzm_boot_lock(&s, d));
  assert(nzm_boot_load(&s, d) == -EEXIST); /* Never consume a partial transaction. */
  nzm_boot_close(&s); close(d);

  d = fixture(); nzm_boot_init(&s);
  assert(!nzm_boot_lock(&s, d));
  assert(!nzm_boot_reset(&s, boot, "1"));
  /* The authority contents are not sufficient: replacement must fail by inode. */
  assert(!renameat(d, "authority", d, "old-authority"));
  put(d, "authority", "extm-wear-v1\n");
  assert(nzm_boot_assert(&s) == -ESTALE);
  assert(nzm_boot_mark(&s, "acquire-intent", boot) == -ESTALE);
  nzm_boot_close(&s);
  bad_meta = 1;
  assert(nzm_boot_lock(&s, d) == -EPERM);
  bad_meta = 0;
  put(d, "authority", "wrong\n");
  assert(nzm_boot_lock(&s, d) == -EPERM);
  close(d);
  puts("boot_state: authority inode/flock, bounded persistence, no-space, uncertain no-replay: PASS");
  return 0;
}
