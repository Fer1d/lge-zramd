#define _GNU_SOURCE
#include "nzm_internal.h"
#include "recovery.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/fs.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/statvfs.h>
#include <sys/swap.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
#ifdef NZM_CORE_ADAPTER
#include NZM_CORE_ADAPTER
#endif

/* Keep each nanosleep well within signed time_t and kernel nanoseconds.
 * This is an API chunk, not a config limit. No unsigned sleep() narrowing. */
int nzm_sleep_seconds(uint64_t seconds) {
  while (seconds) {
    uint64_t chunk = seconds > 86400 ? 86400 : seconds;
    struct timespec t = {(time_t)chunk, 0};
    if (nanosleep(&t, NULL)) return -errno;
    seconds -= chunk;
  }
  return 0;
}
static int path_error(const char *op, const char *path, int rc) {
  fprintf(stderr, "directory syscall=%s path=%s errno=%d (%s)\n",
          op, path, -rc, strerror(-rc));
  return rc;
}
/* Ancestors need traversal/metadata only, never directory-content read.
 * Root-owned nonwritable ancestors need no exact gid/mode. The persist
 * mount alone permits its platform system group to write (never other).
 * Leaves stay O_RDONLY for existing exact metadata/xattr and fsync users. */
static int directory_meta(const char *path, const struct stat *st) {
  if (!S_ISDIR(st->st_mode)) return -ENOTDIR;
  if (st->st_uid || (st->st_mode & 0002)) return -EPERM;
  if (!(st->st_mode & 0020)) return 0;
  /* Required Android persist trust, not a generic group-writable exception. */
  return !strcmp(path,"/mnt/vendor/persist") && st->st_gid==1000 &&
         !(st->st_mode & 07000) ? 0 : -EPERM;
}
int nzm_directory(const char *path) {
  char b[256], current[256] = "", *cursor, *p;
  struct stat st;
  if (strlen(path) >= sizeof b || path[0] != '/')
    return path_error("validate-path", path, -EINVAL);
  strcpy(b, path);
  int fd = open("/", O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return path_error("open-O_PATH", "/", -errno);
  int rc = fstat(fd, &st) ? -errno : directory_meta("/", &st);
  if (rc) { close(fd); return path_error("fstat/metadata", "/", rc); }
  cursor = b;
  while (*cursor) {
    while (*cursor == '/') cursor++;
    if (!*cursor) break;
    p = cursor;
    char *end = strchr(cursor, '/');
    if (end) { *end = '\0'; cursor = end + 1; }
    else cursor += strlen(cursor);
    while (*cursor == '/') cursor++;
    if (!strcmp(p, ".") || !strcmp(p, "..")) {
      close(fd); return path_error("validate-component", path, -EINVAL);
    }
    strcat(current, "/"); strcat(current, p);
    int leaf = *cursor == '\0';
    int n = openat(fd, p, (leaf ? O_RDONLY : O_PATH) |
                   O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int saved_errno = errno;
    close(fd);
    if (n < 0) return path_error(leaf ? "openat-O_RDONLY" : "openat-O_PATH",
                                 current, -saved_errno);
    fd = n;
    rc = fstat(fd, &st) ? -errno : directory_meta(current, &st);
    if (rc) { close(fd); return path_error("fstat/metadata", current, rc); }
  }
  return fd;
}
int nzm_private(const char *path, const char *ctx) {
  int fd = nzm_directory(path);
  if (fd < 0)
    return fd;
  const struct nzm_meta m = {0, 0, 0700, 0, ctx};
  int rc = nzm_io_check(fd, S_IFDIR, &m);
  if (rc) {
    close(fd);
    return path_error("exact-metadata/xattr", path, rc);
  }
  return fd;
}
int nzm_rawread(const char *p, char *b, size_t cap) {
  int fd = open(p, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
  size_t n;
  if (fd < 0)
    return -errno;
  int rc = nzm_io_read_fd(fd, b, cap - 1, &n);
  close(fd);
  if (rc)
    return rc;
  b[n] = 0;
  return 0;
}
int nzm_text(int fd, const char *name, const char *ctx, char *b,
                size_t cap) {
  const struct nzm_meta m = {0, 0, 0600, 1, ctx};
  size_t n;
  int rc = nzm_io_read_at(fd, name, &m, b, cap - 1, &n);
  if (rc)
    return rc;
  b[n] = 0;
  return 0;
}
int nzm_absent(int fd, const char *name) {
  struct stat st;
  if (!fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW))
    return -EEXIST;
  return errno == ENOENT ? 0 : -errno;
}
int nzm_create_text(int fd, const char *name, const char *ctx,
                       const char *s) {
  int f = openat(fd, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                 0600);
  if (f < 0)
    return -errno;
  const struct nzm_meta m = {0, 0, 0600, 1, ctx};
  int rc = nzm_io_check(f, S_IFREG, &m);
  if (!rc)
    rc = nzm_io_sysfs_write(f, s, strlen(s));
  if (!rc && fsync(f))
    rc = -errno;
  if (close(f) && !rc)
    rc = -errno;
  if (!rc && fsync(fd))
    rc = -errno;
  return rc;
}
int nzm_mark(struct nzm_core *c, const char *name, const char *s) {
  return nzm_create_text(c->runfd, name, RCTX, s);
}
int nzm_put(const char *p, const char *s) {
  int fd = open(p, O_WRONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0)
    return -errno;
  int rc = nzm_io_sysfs_write(fd, s, strlen(s));
  close(fd);
  return rc;
}
int nzm_putnum(const char *p, uint64_t v) {
  char b[32];
  snprintf(b, sizeof b, "%" PRIu64 "\n", v);
  return nzm_put(p, b);
}
int nzm_number(const char *p, uint64_t *v) {
  char b[128];
  int rc = nzm_rawread(p, b, sizeof b);
  if (rc)
    return rc;
  size_t n = strlen(b);
  if (n && b[n - 1] == '\n')
    b[--n] = 0;
  return nzm_u64(b, v);
}
int nzm_fields(const char *p, uint64_t *v, size_t count) {
  char b[2048], *s, *end;
  int rc = nzm_rawread(p, b, sizeof b);
  if (rc)
    return rc;
  s = b;
  for (size_t i = 0; i < count; i++) {
    while (*s == ' ' || *s == '\t')
      s++;
    end = s;
    while (*end >= '0' && *end <= '9')
      end++;
    if (end == s || (*end && *end != ' ' && *end != '\t' && *end != '\n'))
      return -EINVAL;
    char ch = *end;
    *end = 0;
    rc = nzm_u64(s, &v[i]);
    *end = ch;
    if (rc)
      return rc;
    s = end;
  }
  while (*s == ' ' || *s == '\t' || *s == '\n')
    s++;
  return *s ? -EINVAL : 0;
}
uint64_t nzm_nowtime(void) {
  time_t t = time(NULL);
  return t < 0 ? 0 : (uint64_t)t;
}
/* Monotonic subtraction only after ordering; never construct now+interval. */
int nzm_monotonic_now(uint64_t *tick) {
  struct timespec t;
  if(clock_gettime(CLOCK_MONOTONIC,&t))return -errno;
  if(t.tv_sec<0)return -ERANGE;
  *tick=(uint64_t)t.tv_sec;return 0;
}
