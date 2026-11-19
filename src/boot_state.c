#define _GNU_SOURCE
#include "boot_state.h"
#include "nzm_io.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>
#ifdef NZM_CORE_ADAPTER
#include NZM_CORE_ADAPTER
#endif

#define STATE_CTX "u:object_r:extm_wear_ledger:s0"
#define AUTH_CTX "u:object_r:extm_wear_authority:s0"
#define DIR_CTX "u:object_r:extm_wear_dir:s0"
#define STATE_MAX 8192
static const char *const keys[NZM_BOOT_ENTRIES] = {
  "minfree", "vm-attempted-boot", "vm-complete-boot",
  "vm-attempted-completed", "vm-complete-completed", "acquire-intent",
  "bootstrap-result", "managed", "ram-only-attempted", "ram-only"
};
static const char transaction[] = "nzm-boot-txn-v1\n";
static int key_index(const char *name) {
  for (int i = 0; i < NZM_BOOT_ENTRIES; ++i)
    if (!strcmp(keys[i], name)) return i;
  return -1;
}
void nzm_boot_init(struct nzm_boot_state *s) {
  memset(s, 0, sizeof *s);
  s->dirfd = s->authority_fd = -1;
}
static int absent(int dirfd, const char *name) {
  struct stat st;
  if (!fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW)) return -EEXIST;
  return errno == ENOENT ? 0 : -errno;
}
static int authority(int fd) {
  static const char expected[] = "extm-wear-v1\n";
  const struct nzm_meta m = {0, 0, 0600, 1, AUTH_CTX};
  int rc = nzm_io_check(fd, S_IFREG, &m);
  char b[sizeof expected];
  if (rc) return rc;
  ssize_t n = pread(fd, b, sizeof b, 0);
  return n < 0 ? -errno : n != sizeof expected - 1 ||
      memcmp(b, expected, sizeof expected - 1) ? -EPERM : 0;
}
int nzm_boot_assert(const struct nzm_boot_state *s) {
  if (s->authority_fd < 0 || s->dirfd < 0 || s->poisoned) return -EPERM;
  const struct nzm_meta dm = {0, 0, 0700, 0, DIR_CTX};
  int rc = nzm_io_check(s->dirfd, S_IFDIR, &dm);
  if (!rc) rc = authority(s->authority_fd);
  if (rc) return rc;
  int fd = openat(s->dirfd, "authority", O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) return -errno;
  struct stat st;
  rc = authority(fd);
  if (!rc && fstat(fd, &st)) rc = -errno;
  if (!rc && (st.st_dev != s->authority_dev || st.st_ino != s->authority_ino)) rc = -ESTALE;
  close(fd);
  return rc;
}
int nzm_boot_lock(struct nzm_boot_state *s, int dirfd) {
  if (s->authority_fd >= 0) return -EALREADY;
  int fd = openat(dirfd, "authority", O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) return -errno;
  int rc = authority(fd);
  if (!rc && flock(fd, LOCK_EX | LOCK_NB)) rc = -errno;
  struct stat st;
  if (!rc && fstat(fd, &st)) rc = -errno;
  if (rc) { close(fd); return rc; }
  s->dirfd = dirfd;
  s->authority_fd = fd;
  s->authority_dev = st.st_dev;
  s->authority_ino = st.st_ino;
  rc = nzm_boot_assert(s);
  if (rc) nzm_boot_close(s);
  return rc;
}
void nzm_boot_close(struct nzm_boot_state *s) {
  /* Closing releases flock; never unlink, truncate or replace the authority. */
  if (s->authority_fd >= 0) close(s->authority_fd);
  s->authority_fd = -1;
}
static int uuid(const char *b) {
  if (strlen(b) != 36) return 0;
  for (int i = 0; i < 36; ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (b[i] != '-') return 0;
    } else if (!((b[i] >= '0' && b[i] <= '9') || (b[i] >= 'a' && b[i] <= 'f'))) return 0;
  }
  return 1;
}
static int binding_valid(const char *b) {
  size_t n = strlen(b);
  if (!n || n >= NZM_BOOT_BINDING_MAX) return 0;
  for (size_t i = 0; i < n; ++i)
    if (b[i] != ' ' && (b[i] < '0' || b[i] > '9')) return 0;
  return 1;
}
static int serialize(const struct nzm_boot_state *s, char *b, size_t *len) {
  int n = snprintf(b, STATE_MAX, "nzm-boot-v1\nboot %s\nbinding %s\n", s->boot, s->binding);
  if (n < 0 || n >= STATE_MAX) return -EOVERFLOW;
  size_t used = (size_t)n;
  static const char hex[] = "0123456789abcdef";
  for (int i = 0; i < NZM_BOOT_ENTRIES; ++i) {
    if (!(s->present & (1u << i))) continue;
    size_t k = strlen(keys[i]), v = strlen(s->value[i]);
    if (!v || used + k + 2 * v + 2 >= STATE_MAX) return -EOVERFLOW;
    memcpy(b + used, keys[i], k); used += k; b[used++] = ' ';
    for (size_t j = 0; j < v; ++j) {
      unsigned char c = (unsigned char)s->value[i][j];
      b[used++] = hex[c >> 4]; b[used++] = hex[c & 15];
    }
    b[used++] = '\n';
  }
  memcpy(b + used, "end\n", 4); used += 4;
  *len = used;
  return 0;
}
static int unhex(char c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
int nzm_boot_load(struct nzm_boot_state *s, int dirfd) {
  int rc = absent(dirfd, "boot-state.txn");
  if (!rc) rc = absent(dirfd, "boot-state.new");
  if (rc) return rc;
  char b[STATE_MAX + 1], original[STATE_MAX], canonical[STATE_MAX];
  size_t len, clen;
  const struct nzm_meta m = {0, 0, 0600, 1, STATE_CTX};
  rc = nzm_io_read_at(dirfd, "boot-state", &m, b, STATE_MAX, &len);
  if (rc) return rc;
  if (!len || memchr(b, 0, len)) return -EINVAL;
  memcpy(original, b, len);
  b[len] = 0;
  struct nzm_boot_state next = *s;
  next.present = 0;
  memset(next.value, 0, sizeof next.value);
  char *p = b, *line = strsep(&p, "\n");
  if (strcmp(line, "nzm-boot-v1")) return -EINVAL;
  line = strsep(&p, "\n");
  if (!line || strncmp(line, "boot ", 5) || !uuid(line + 5)) return -EINVAL;
  strcpy(next.boot, line + 5);
  line = strsep(&p, "\n");
  if (!line || strncmp(line, "binding ", 8) || !binding_valid(line + 8)) return -EINVAL;
  strcpy(next.binding, line + 8);
  int previous = -1, ended = 0;
  while ((line = strsep(&p, "\n"))) {
    if (!strcmp(line, "end")) { ended = p && !*p; break; }
    char *v = strchr(line, ' ');
    if (!v) return -EINVAL;
    *v++ = 0;
    int i = key_index(line);
    size_t n = strlen(v);
    if (i <= previous || i < 0 || !n || n % 2 || n / 2 >= NZM_BOOT_VALUE_MAX) return -EINVAL;
    for (size_t j = 0; j < n / 2; ++j) {
      int a = unhex(v[2*j]), z = unhex(v[2*j+1]);
      if (a < 0 || z < 0 || !(a || z)) return -EINVAL;
      next.value[i][j] = (char)((a << 4) | z);
    }
    next.present |= 1u << i; previous = i;
  }
  if (!ended) return -EINVAL;
  rc = serialize(&next, canonical, &clen);
  /* Parser is strict about ordering, line endings and bounded field encodings. */
  if (rc || clen != len || memcmp(canonical, original, len)) return rc ? rc : -EINVAL;
  next.loaded = 1; next.dirfd = dirfd;
  *s = next;
  return 0;
}
static int create(int dirfd, const char *name, const char *b, size_t len) {
  int fd = openat(dirfd, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) return -errno;
  const struct nzm_meta m = {0, 0, 0600, 1, STATE_CTX};
  int rc = nzm_io_check(fd, S_IFREG, &m);
  if (!rc) rc = nzm_io_sysfs_write(fd, b, len);
  if (!rc && fsync(fd)) rc = -errno;
  if (close(fd) && !rc) rc = -errno;
  return rc;
}
static int commit(struct nzm_boot_state *s, struct nzm_boot_state *next) {
  int rc = nzm_boot_assert(s);
  if (!rc) rc = absent(s->dirfd, "boot-state.txn");
  if (!rc) rc = absent(s->dirfd, "boot-state.new");
  if (rc) return rc;
  char b[STATE_MAX]; size_t len;
  rc = serialize(next, b, &len);
  if (!rc) rc = nzm_io_ledger_preflight(s->dirfd, len);
  if (rc) return rc;
  /* From here any error is uncertain. Never remove failure evidence or retry. */
  s->poisoned = 1;
  rc = create(s->dirfd, "boot-state.txn", transaction, sizeof transaction - 1);
  if (!rc && fsync(s->dirfd)) rc = -errno;
  if (!rc) rc = create(s->dirfd, "boot-state.new", b, len);
  if (!rc && fsync(s->dirfd)) rc = -errno;
  if (!rc && renameat(s->dirfd, "boot-state.new", s->dirfd, "boot-state")) rc = -errno;
  if (!rc && fsync(s->dirfd)) rc = -errno;
  if (!rc && unlinkat(s->dirfd, "boot-state.txn", 0)) rc = -errno;
  if (!rc && fsync(s->dirfd)) {
    rc = -errno;
    /* Rename is durable already; restore evidence if retirement is uncertain. */
    (void)create(s->dirfd, "boot-state.txn", transaction, sizeof transaction - 1);
    (void)fsync(s->dirfd);
  }
  if (!rc) { next->loaded = 1; next->poisoned = 0; *s = *next; }
  return rc;
}
int nzm_boot_reset(struct nzm_boot_state *s, const char *boot, const char *binding) {
  if (!uuid(boot) || !binding_valid(binding)) return -EINVAL;
  struct nzm_boot_state next = *s;
  strcpy(next.boot, boot); strcpy(next.binding, binding);
  next.present = 0; memset(next.value, 0, sizeof next.value);
  return commit(s, &next);
}
int nzm_boot_absent(const struct nzm_boot_state *s, const char *name) {
  int i = key_index(name);
  if (i < 0 || !s->loaded || s->poisoned) return -EINVAL;
  return s->present & (1u << i) ? -EEXIST : 0;
}
int nzm_boot_text(const struct nzm_boot_state *s, const char *name, char *out, size_t cap) {
  int i = key_index(name);
  int rc = nzm_boot_absent(s, name);
  if (rc != -EEXIST) return rc ? rc : -ENOENT;
  size_t n = strlen(s->value[i]);
  if (n >= cap) return -EOVERFLOW;
  memcpy(out, s->value[i], n + 1);
  return 0;
}
int nzm_boot_mark(struct nzm_boot_state *s, const char *name, const char *value) {
  int rc = nzm_boot_absent(s, name);
  if (rc) return rc;
  size_t n = strlen(value);
  if (!n || n >= NZM_BOOT_VALUE_MAX) return -E2BIG;
  struct nzm_boot_state next = *s;
  int i = key_index(name);
  next.present |= 1u << i;
  memcpy(next.value[i], value, n + 1);
  return commit(s, &next);
}
