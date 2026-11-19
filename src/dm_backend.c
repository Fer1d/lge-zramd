#include "nzm_internal.h"
#include <errno.h>
#include <inttypes.h>
#include <string.h>

/* No quota reservation, writeback, provisioning, or acquisition replay. */
int nzm_dm_quota(void) {
  uint64_t enabled, limit;
  int rc = nzm_number(Z "writeback_limit_enable", &enabled);
  if (!rc) rc = nzm_number(Z "writeback_limit", &limit);
  return rc ? rc : enabled == 1 && limit == 0 ? 0 : -EPERM;
}
int nzm_dm_bound(const struct nzm_helper_identity *id) {
  char path[128], dev[64], expected[64];
  unsigned dm; int n = 0;
  int rc = nzm_dm_quota();
  if (!rc) rc = nzm_rawread(Z "backing_dev", path, sizeof path);
  if (rc) return rc;
  if (sscanf(path, "/dev/block/dm-%u%n", &dm, &n) != 1 ||
      strcmp(path + n, "\n") || dm > 1048575) return -EPERM;
  snprintf(path, sizeof path, "/sys/block/dm-%u/dev", dm);
  rc = nzm_rawread(path, dev, sizeof dev);
  snprintf(expected, sizeof expected, "%u:%u\n", id->dm_major, id->dm_minor);
  return rc ? rc : strcmp(dev, expected) ? -EPERM : 0;
}
static int step(struct nzm_core *c, enum nzm_helper_op op,
                struct nzm_helper_identity *id, struct nzm_helper_result *r) {
  struct nzm_helper_identity current;
  int rc = nzm_dm_quota();
  if (!rc) rc = nzm_identity(c, &current);
  if (!rc && (current.bytes != id->bytes || current.inode != id->inode ||
              current.fs_dev != id->fs_dev)) rc = -ESTALE;
  if (!rc) rc = nzm_helper(c, op, id, r);
  if (!rc) rc = nzm_dm_quota();
  return rc;
}
int nzm_dm_boot(struct nzm_core *c) {
  struct nzm_helper_identity id;
  struct nzm_helper_result r;
  int rc = nzm_absent(c->runfd, "managed");
  if (rc == -EEXIST) {
    rc = nzm_dm_quota();
    if (!rc) rc = nzm_managed(c);
    return rc ? rc : nzm_dm_quota();
  }
  if (rc) return rc;
  /* RAM ownership resumes only as RAM. Incomplete DM ownership never replays. */
  if ((rc = nzm_absent(c->runfd, "acquire-intent")) ||
      (rc = nzm_absent(c->runfd, "bootstrap-result")) ||
      (rc = nzm_no_mapping())) return rc;
  rc = nzm_absent(c->runfd, "ram-only");
  if (rc == -EEXIST) return nzm_ram_boot(c);
  if (rc || (rc = nzm_absent(c->runfd, "ram-only-attempted")) ||
      (rc = nzm_cold())) return rc;
  if (!c->config.enable_writeback) return nzm_ram_boot(c);
  rc = nzm_existing_identity(c, &id, 1);
  if (rc == -ENODATA) return nzm_ram_boot(c);
  if (rc) return rc;
  /* Fence before the first mutation, including quota writes. */
  if ((rc = nzm_mark(c, "acquire-intent", c->boot)) ||
      (rc = nzm_putnum(Z "writeback_limit_enable", 1)) ||
      (rc = nzm_putnum(Z "writeback_limit", 0)) ||
      (rc = nzm_dm_quota()) || (rc = nzm_cold())) return rc;
  if ((rc = step(c, NZM_HELPER_CREATE, &id, &r))) return rc;
  id.require_dm = 1; id.dm_major = r.dm_major; id.dm_minor = r.dm_minor;
  if ((rc = step(c, NZM_HELPER_QUERY, &id, &r)) ||
      (rc = nzm_cold()) || (rc = step(c, NZM_HELPER_BIND, &id, &r)) ||
      (rc = nzm_dm_bound(&id)) || (rc = nzm_dm_activate(c, &id))) return rc;
  char record[256];
  snprintf(record, sizeof record,
           "native-v1 %s %" PRIu64 " %" PRIu64 " %" PRIu64 " %u:%u\n",
           c->boot, id.fs_dev, id.inode, id.bytes, id.dm_major, id.dm_minor);
  if ((rc = nzm_dm_bound(&id)) || (rc = nzm_mark(c, "managed", record)) ||
      (rc = nzm_managed(c))) return rc;
  return nzm_dm_quota();
}
