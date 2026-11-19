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

int nzm_core_vm(struct nzm_core *c, int completed) {
  const char *intent =
      completed ? "vm-attempted-completed" : "vm-attempted-boot";
  const char *done = completed ? "vm-complete-completed" : "vm-complete-boot";
  char b[256];
  int rc = nzm_absent(c->runfd, intent);
  if (rc == -EEXIST) {
    rc = nzm_text(c->runfd, done, RCTX, b, sizeof b);
    return rc ? rc : strcmp(b, "verified\n") ? -EPERM : 0;
  }
  if (rc)
    return rc;
  uint64_t base, target;
  rc = nzm_text(c->runfd, "minfree", RCTX, b, sizeof b);
  if (rc == -ENOENT) {
    if ((rc = nzm_number("/proc/sys/vm/min_free_kbytes", &base)))
      return rc;
    if (!base || base > INT_MAX)
      return -ERANGE;
    target = c->config.vm_min_free_kbytes;
    if (!target) {
      target = base;
      if (c->config.vm_minfree_mult) {
        if (base > INT_MAX / c->config.vm_minfree_mult)
          return nzm_diag_config_error("runtime","vm_minfree_mult","baseline * multiplier exceeds kernel INT_MAX",-ERANGE);
        target = base * c->config.vm_minfree_mult;
      }
    }
    snprintf(b, sizeof b, "%" PRIu64 " %" PRIu64 "\n", base, target);
    if ((rc = nzm_mark(c, "minfree", b)))
      return rc;
  } else if (rc)
    return rc;
  else {
    char tail;
    int n = 0;
    if (sscanf(b, "%" SCNu64 " %" SCNu64 "%n%c", &base, &target, &n, &tail) !=
            3 ||
        strcmp(b + n, "\n") || !base || base > INT_MAX || !target ||
        target > INT_MAX)
      return -EINVAL;
  }
  if ((rc = nzm_mark(c, intent, "attempted\n")))
    return rc;
  const char *names[] = {
      "swappiness",        "page-cluster",    "watermark_scale_factor",
      "extra_free_kbytes", "min_free_kbytes", "overcommit_memory",
      "laptop_mode"};
  uint64_t values[] = {c->config.vm_swappiness,
                       c->config.vm_page_cluster,
                       c->config.vm_watermark_scale_factor,
                       c->config.vm_extra_free_kbytes,
                       target,
                       c->config.vm_overcommit_memory,
                       c->config.vm_laptop_mode};
  for (size_t i = 0; i < 7; i++) {
    char p[128];
    uint64_t before, after;
    snprintf(p, sizeof p, "/proc/sys/vm/%s", names[i]);
    if ((rc = nzm_number(p, &before)))
      return nzm_diag(c, names[i], rc);
    if (before != values[i] && (rc = nzm_putnum(p, values[i])))
      return nzm_diag(c, names[i], rc);
    if ((rc = nzm_number(p, &after)))
      return nzm_diag(c, names[i], rc);
    if (after != values[i])
      return nzm_diag(c, names[i], -EIO);
  }
  return nzm_mark(c, done, "verified\n");
}
