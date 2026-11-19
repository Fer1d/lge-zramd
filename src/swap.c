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
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
#ifdef NZM_CORE_ADAPTER
#include NZM_CORE_ADAPTER
#endif

int nzm_active(void) {
  char b[8192];
  int rc = nzm_rawread("/proc/swaps", b, sizeof b);
  if (rc)
    return rc;
  struct stat z;
  if (stat("/dev/block/zram0", &z))
    return -errno;
  if (!S_ISBLK(z.st_mode))
    return -EPERM;
  char *line = strchr(b, '\n');
  if (!line)
    return -EINVAL;
  while (*++line) {
    char path[512], kind[32];
    int used = 0;
    if (sscanf(line, "%511s %31s%n", path, kind, &used) != 2)
      return -EINVAL;
    struct stat st;
    if (stat(path, &st))
      return -errno;
    if (!S_ISBLK(st.st_mode))
      return -EPERM;
    if (st.st_rdev == z.st_rdev)
      return 1;
    line = strchr(line, '\n');
    if (!line)
      return -EINVAL;
  }
  return 0;
}
int nzm_cold(void) {
  uint64_t n;
  char b[128];
  int rc = nzm_active();
  if (rc)
    return rc < 0 ? rc : -EBUSY;
  if ((rc = nzm_number(Z "disksize", &n)))
    return rc;
  if (n)
    return -EBUSY;
  if ((rc = nzm_rawread(Z "backing_dev", b, sizeof b)))
    return rc;
  return strcmp(b, "none\n") ? -EBUSY : 0;
}
static int ram_device(int fd, const struct stat *st) {
  char actual[64], expected[64], label[sizeof RB_DEVICE_LABEL];
  int rc = nzm_rawread(Z "dev", actual, sizeof actual);
  if (rc)
    return rc;
  snprintf(expected, sizeof expected, "%u:%u\n", major(st->st_rdev),
           minor(st->st_rdev));
  if (strcmp(actual, expected))
    return -ENODEV;
  ssize_t n = fgetxattr(fd, "security.selinux", label, sizeof label);
  if (n < 0)
    return -errno;
  return (n == (ssize_t)sizeof label || n == (ssize_t)sizeof label - 1) &&
      !memcmp(label, RB_DEVICE_LABEL, (size_t)n) ? 0 : -EACCES;
}
static int ram_unbacked(void) {
  char b[128];
  int rc = nzm_rawread(Z "backing_dev", b, sizeof b);
  return rc ? rc : strcmp(b, "none\n") ? -EBUSY : 0;
}
static const char ram_attempt[] = "ram-only-attempted";
static int activate(struct nzm_core *c, int ram_only, const struct nzm_helper_identity *id) {
  int rc;
  long ps = sysconf(_SC_PAGESIZE);
  if (ps != 4096 && ps != 16384 && ps != 65536)
    return -ENOTSUP;
  if (!ram_only && !id && (rc = nzm_putnum(Z "disksize", c->config.disksize_bytes)))
    return rc;
  int fd = open("/dev/block/zram0", O_RDWR | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0)
    return -errno;
  uint64_t bytes = 0;
  struct stat st;
  rc = fstat(fd, &st) ? -errno : !S_ISBLK(st.st_mode) ? -EPERM : 0;
  if (!rc && ram_only) {
    rc = ram_device(fd, &st);
    if (!rc) rc = nzm_cold();
    if (!rc) rc = nzm_mark(c, ram_attempt, c->boot);
    if (!rc) rc = nzm_putnum(Z "disksize", c->config.disksize_bytes);
    /* The kernel will not bind a backing device once disksize is set. Check
     * after initialization as well: a competing bind before it must not
     * allow even the header write or swapon on a backed zram. */
    if (!rc) rc = ram_unbacked();
  }
  if (!rc && id) {
    rc = ram_device(fd, &st);
    if (!rc) rc = nzm_dm_bound(id);
    if (!rc) rc = nzm_putnum(Z "disksize", c->config.disksize_bytes);
    if (!rc) rc = nzm_dm_bound(id);
  }
  if (!rc && ioctl(fd, BLKGETSIZE64, &bytes))
    rc = -errno;
  if (!rc &&
      (bytes / (uint64_t)ps < 2 || bytes / (uint64_t)ps - 1 > UINT32_MAX))
    rc = -ERANGE;
  unsigned char *header = calloc(1, (size_t)ps);
  if (!header && !rc)
    rc = -ENOMEM;
  if (!rc && id) rc = nzm_dm_bound(id);
  if (!rc) {
    uint32_t v = 1, last = (uint32_t)(bytes / (uint64_t)ps - 1);
    memcpy(header + 1024, &v, 4);
    memcpy(header + 1028, &last, 4);
    memcpy(header + ps - 10, "SWAPSPACE2", 10);
    ssize_t n = pwrite(fd, header, (size_t)ps, 0);
    if (n != ps)
      rc = n < 0 ? -errno : -EIO;
  }
  free(header);
  if (!rc && fsync(fd))
    rc = -errno;
  close(fd);
  if (!rc && ram_only)
    rc = ram_unbacked();
  if (!rc && id) rc = nzm_dm_bound(id);
  if (!rc && swapon("/dev/block/zram0", 0))
    rc = -errno;
  if (!rc) {
    int a = nzm_active();
    rc = a == 1 ? 0 : a < 0 ? a : -EPERM;
  }
  if (!rc && id) rc = nzm_dm_bound(id);
  return !rc && ram_only ? ram_unbacked() : rc;
}
int nzm_activate(struct nzm_core *c) {
  return activate(c, 0, NULL);
}
int nzm_dm_activate(struct nzm_core *c, const struct nzm_helper_identity *id) {
  return activate(c, 0, id);
}
int nzm_ram_boot(struct nzm_core *c) {
  int owned = nzm_core_authority(c);
  if (owned) return owned;
  static const char marker[] = "ram-only";
  char record[sizeof c->boot];
  int rc = nzm_state_absent(c, marker);
  if (rc == -EEXIST) {
    rc = nzm_state_text(c, marker, record, sizeof record);
    if (rc) return rc;
    if (strcmp(record, c->boot)) return -ESTALE;
    rc = nzm_state_text(c, ram_attempt, record, sizeof record);
    if (rc) return rc;
    if (strcmp(record, c->boot)) return -ESTALE;
    int fd = open("/dev/block/zram0", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
      return -errno;
    struct stat st;
    rc = fstat(fd, &st) ? -errno : !S_ISBLK(st.st_mode) ? -EPERM : ram_device(fd, &st);
    close(fd);
    if (!rc) rc = ram_unbacked();
    if (!rc) {
      rc = nzm_active();
      rc = rc == 1 ? 0 : rc < 0 ? rc : -ESTALE;
    }
    return rc;
  }
  if (rc || (rc = nzm_state_absent(c, ram_attempt)))
    return rc;
  rc = activate(c, 1, NULL);
  /* Keep the attempt fence even on failure. Only verified success can resume. */
  return rc ? rc : nzm_mark(c, marker, c->boot);
}
