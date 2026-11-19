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

struct recovery_state { struct nzm_core *core; int data; };
static int wear_authority(struct nzm_core *c) {
  if (c->boot_state.authority_fd >= 0) return nzm_core_authority(c);
  static const char authority[] = "extm-wear-v1\n";
  char auth[sizeof authority - 1];
  size_t len;
  const struct nzm_meta meta = {0, 0, 0600, 1, "u:object_r:extm_wear_authority:s0"};
  if (c->wearfd < 0) {
    c->wearfd = nzm_private(WEAR, WCTX);
    if (c->wearfd < 0)
      return c->wearfd;
  }
  int rc = nzm_io_read_at(c->wearfd, "authority", &meta, auth, sizeof auth, &len);
  if (rc)
    return rc;
  return len != sizeof auth || memcmp(auth, authority, sizeof auth) ? -EPERM : 0;
}
int nzm_pause_verified(struct nzm_core *c, int *paused) {
  int rc = wear_authority(c);
  if (rc)
    return rc;
  *paused = 0;
  rc = nzm_absent(c->wearfd, "pause");
  if (rc != -EEXIST)
    return rc;
  const struct nzm_meta meta = {0, 0, 0600, 1, PCTX};
  char record[sizeof NZM_PAUSE_RECORD - 1];
  size_t len;
  rc = nzm_io_read_at(c->wearfd, "pause", &meta, record, sizeof record, &len);
  if (rc)
    return rc;
  if (len != sizeof record || memcmp(record, NZM_PAUSE_RECORD, sizeof record))
    return -EPERM;
  *paused = 1;
  return 0;
}
#ifdef NZM_LEGACY_TEST
/* Historical v1 scheduling fixture only; never a production fallback. */
int nzm_wear(struct nzm_core *c, struct nzm_ledger *l) {
  int rc = wear_authority(c);
  if (rc)
    return rc;
  if ((rc = nzm_absent(c->wearfd, "pause")) ||
      (rc = nzm_absent(c->wearfd, "ledger.new")) ||
      (rc = nzm_absent(c->wearfd, "ledger.txn")) ||
      (rc = nzm_intent_check(c)))
    return rc;
  char *b = malloc(NZM_LEDGER_MAX_BYTES);
  if (!b)
    return -ENOMEM;
  size_t n;
  const struct nzm_meta m = {0, 0, 0600, 1, LCTX};
  rc = nzm_io_read_at(c->wearfd, "ledger", &m, b, NZM_LEDGER_MAX_BYTES, &n);
  if (!rc)
    rc = nzm_ledger_parse(b, n, nzm_nowtime(), l);
  free(b);
  return rc;
}
#endif
static int data_open(struct nzm_core *c, int allow_create) {
  (void)allow_create; /* Fresh-data provisioning requires a separate
                         policy-reviewed helper. */
  int root = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (root < 0)
    return -errno;
  int d = nzm_io_data_dir(root);
  close(root);
  if (d < 0)
    return d;
  int fd = nzm_io_private_dir_at(d, "extm", DCTX);
  close(d);
  if (fd < 0)
    return fd;
  if (c->datafd >= 0)
    close(c->datafd);
  c->datafd = fd;
  return 0;
}
static int same_inode(int a,int b) {
  struct stat x,y;
  if(fstat(a,&x)||fstat(b,&y))return -errno;
  return x.st_dev==y.st_dev&&x.st_ino==y.st_ino?0:-ESTALE;
}
static int recovery_verify(void *arg) {
  struct recovery_state *state=arg;
  struct nzm_core *c=state->core;
  if(c->lock.fd<0)return -EPERM;
  int rc=nzm_mounts_verify();
  if(rc)return rc;
  int wd=nzm_private(WEAR,WCTX);
  if(wd<0)return wd;
  if(c->wearfd>=0)rc=same_inode(wd,c->wearfd);
  close(wd);if(rc)return rc;
  if(state->data>=0) {
    int root=open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(root<0)return -errno;
    int d=nzm_io_data_dir(root);close(root);
    if(d<0)return d;
    rc=same_inode(d,state->data);close(d);if(rc)return rc;
  }
  /* Revalidate authorization and unresolved evidence, never historical budget.
   * Recovery must not bypass a pause even if startup was previously allowed. */
  int paused = 0;
  if(!rc)rc=nzm_phase1_policy(c,&paused);
  if(!rc && paused)rc=-ECANCELED;
  if(!rc)rc=nzm_cold();
  if(!rc)rc=nzm_no_mapping();
  if(!rc)rc=nzm_state_absent(c,"acquire-intent");
  if(!rc)rc=nzm_state_absent(c,"bootstrap-result");
  return rc;
}
static int recovery_request(void *arg) {
#ifdef __ANDROID__
  struct nzm_core *c=((struct recovery_state *)arg)->core;
  char b[PROP_VALUE_MAX];
  __system_property_get("ro.crypto.type",b);
  int crypto_rc=nzm_recovery_crypto_allowed(b);
  if(crypto_rc)return crypto_rc;
  if(__system_property_get("sys.extm.provision.request",b) ||
     __system_property_get("sys.extm.provision.ack",b))return -EEXIST;
  if(__system_property_set("sys.extm.provision.request",c->boot))return -EIO;
  struct timespec start,tick;
  if(clock_gettime(CLOCK_MONOTONIC,&start))return -errno;
  for(;;) {
    int n=__system_property_get("sys.extm.provision.ack",b);
    if(n)return !strcmp(b,c->boot)?0:-EPERM;
    if(clock_gettime(CLOCK_MONOTONIC,&tick))return -errno;
    if(tick.tv_sec < start.tv_sec || start.tv_sec < 0)return -ERANGE;
    if((uint64_t)tick.tv_sec-(uint64_t)start.tv_sec >= c->config.data_wait_seconds)return -ETIMEDOUT;
    struct timespec delay={0,100000000};
    if(c->control) {
      struct timespec deadline;
      int rc=nzm_runtime_deadline_after(&tick,UINT64_C(100000000),&deadline);
      if(rc)return rc;
      rc=nzm_runtime_control_wait_until(c->control,NZM_BOUNDARY_OBSERVING,&deadline);
      if(rc<0)return rc;
      if(rc)return nzm_core_control(c,NZM_BOUNDARY_OBSERVING);
    } else if(nanosleep(&delay,NULL))return -errno;
  }
#else
  (void)arg;return -ENOTSUP;
#endif
}
int nzm_core_provision(struct nzm_core *c) {
  struct recovery_state state={c,-1};
  int rc=recovery_verify(&state);
  if(rc)return rc;
  int root=open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
  if(root<0)return -errno;
  int data=nzm_io_data_dir(root);close(root);
  if(data<0)return data;
  state.data=data;
  struct nzm_recovery_ops ops={recovery_verify,recovery_request,&state};
  int fd=-1;rc=nzm_recovery_owner(data,&ops,&fd);close(data);
  if(!rc){if(c->datafd>=0)close(c->datafd);c->datafd=fd;}
  return rc;
}
int nzm_existing_identity(struct nzm_core *c, struct nzm_helper_identity *id, int fallback) {
  /* Reopen by pathname every time: a removed/replaced active directory is NOT
   * repaired or accepted just because an old directory fd remains pinned. */
  int rc = data_open(c, 0);
  if (rc)
    return rc;
  char b[64];
  if ((rc = nzm_text(c->datafd, "owner", OCTX, b, sizeof b)))
    return rc;
  if (strcmp(b, "extm-dm-owner-v1\n"))
    return -EPERM;
  int f =
      openat(c->datafd, "file", O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
  if (f < 0)
    return fallback && errno == ENOENT ? -ENODATA : -errno;
  const struct nzm_meta m = {0, 0, 0600, 1, FCTX};
  struct stat st;
  rc = nzm_io_check(f, S_IFREG, &m);
  if (!rc && fstat(f, &st))
    rc = -errno;
  close(f);
  if (rc)
    return rc;
  if (st.st_size < 0 || (uint64_t)st.st_size != c->config.expected_bytes)
    return fallback ? -ENODATA : -EPERM;
  memset(id, 0, sizeof *id);
  id->bytes = c->config.expected_bytes;
  id->inode = st.st_ino;
  id->fs_dev = st.st_dev;
  id->backing_major = major(st.st_dev);
  id->backing_minor = minor(st.st_dev);
  return 0;
}
int nzm_identity(struct nzm_core *c, struct nzm_helper_identity *id) {
  return nzm_existing_identity(c, id, 0);
}
int nzm_helper(struct nzm_core *c, enum nzm_helper_op op,
                  struct nzm_helper_identity *id, struct nzm_helper_result *r) {
  struct nzm_helper_run run = {0};
  int rc = nzm_helper_call_with_mask(op, id, 120000, NULL,
      c->helper_exec_mask_valid ? &c->helper_exec_mask : NULL, &run, r);
  if (op != NZM_HELPER_QUERY || rc)
    nzm_helper_diagnostic(stderr, op, &run, rc, c->attempted);
  return nzm_diag(c,
              op == NZM_HELPER_PREPARE  ? "preparefile"
              : op == NZM_HELPER_CREATE ? "create"
              : op == NZM_HELPER_QUERY  ? "query"
                                        : "bind",
              rc);
}
int nzm_managed(struct nzm_core *c) {
  char b[256], expected[256], path[128], dev[64];
  struct nzm_helper_identity id;
  struct nzm_helper_result r;
  int rc = nzm_identity(c, &id);
  if (rc)
    return rc;
  if ((rc = nzm_state_text(c, "managed", b, sizeof b)))
    return rc;
  unsigned ma, mi;
  uint64_t fs, ino, bytes;
  char boot[37], extra;
  if (sscanf(b, "native-v1 %36s %" SCNu64 " %" SCNu64 " %" SCNu64 " %u:%u %c",
             boot, &fs, &ino, &bytes, &ma, &mi, &extra) != 6)
    return -EINVAL;
  snprintf(expected, sizeof expected,
           "native-v1 %s %" PRIu64 " %" PRIu64 " %" PRIu64 " %u:%u\n", c->boot,
           id.fs_dev, id.inode, id.bytes, ma, mi);
  if (strcmp(b, expected))
    return -EPERM;
  id.require_dm = 1;
  id.dm_major = ma;
  id.dm_minor = mi;
  if ((rc = nzm_helper(c, NZM_HELPER_QUERY, &id, &r)))
    return rc;
  if ((rc = nzm_rawread(Z "backing_dev", path, sizeof path)))
    return rc;
  unsigned dm;
  int n = 0;
  if (sscanf(path, "/dev/block/dm-%u%n", &dm, &n) != 1 ||
      strcmp(path + n, "\n") || dm > 1048575)
    return -EPERM;
  snprintf(path, sizeof path, "/sys/block/dm-%u/dev", dm);
  if ((rc = nzm_rawread(path, dev, sizeof dev)))
    return rc;
  snprintf(expected, sizeof expected, "%u:%u\n", ma, mi);
  if (strcmp(dev, expected))
    return -EPERM;
  rc = nzm_active();
  return rc == 1 ? 0 : rc < 0 ? rc : -EPERM;
}
int nzm_no_mapping(void) {
  int fd = nzm_directory("/sys/block");
  if (fd < 0)
    return fd;
  DIR *d = fdopendir(fd);
  if (!d) {
    int e = -errno;
    close(fd);
    return e;
  }
  struct dirent *e;
  int rc = 0;
  for (;;) {
    errno = 0;
    e = readdir(d);
    if (!e) {
      if (errno)
        rc = -errno;
      break;
    }
    if (strncmp(e->d_name, "dm-", 3))
      continue;
    char p[512], b[128];
    snprintf(p, sizeof p, "/sys/block/%s/dm/name", e->d_name);
    rc = nzm_rawread(p, b, sizeof b);
    if (rc)
      break;
    if (!strcmp(b, "extm-file\n")) {
      rc = -EEXIST;
      break;
    }
  }
  closedir(d);
  return rc;
}
