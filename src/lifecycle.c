#define _GNU_SOURCE
#include "demand_watch.h"
#include "nzm_internal.h"
#include "recovery.h"
#include "state_bootstrap.h"
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
#include <sys/vfs.h>
#include <linux/magic.h>
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

void nzm_core_init(struct nzm_core *c) {
  memset(c, 0, sizeof *c);
  c->runfd = c->wearfd = c->datafd = c->startupfd = c->lock.fd = -1;
  nzm_boot_init(&c->boot_state);
  (void)nzm_memory_init(&c->memory_budget, 0);
  c->execution.next_request_id = 1;
  nzm_config_default(&c->config);
}
void nzm_core_close(struct nzm_core *c) {
  nzm_boot_close(&c->boot_state);
  if (c->runfd >= 0)
    close(c->runfd);
  if (c->wearfd >= 0)
    close(c->wearfd);
  if (c->datafd >= 0)
    close(c->datafd);
  if (c->startupfd >= 0)
    close(c->startupfd);
  c->runfd=c->wearfd=c->datafd=c->startupfd=c->lock.fd=-1;
}
int nzm_core_config(struct nzm_core *c) {
  int d = nzm_directory("/vendor/etc");
  if (d < 0)
    return nzm_diag_config_error("parent","<file>","open exact /vendor/etc failed",d);
  int f = openat(d, "lge-zram.conf", O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
  close(d);
  if (f < 0)
    return nzm_diag_config_error("open","<file>","open exact config failed",-errno);
  const struct nzm_meta meta = {0, 0, 0644, 1,
                                "u:object_r:vendor_configs_file:s0"};
  int rc = 0;
  char *b = malloc(NZM_CONFIG_MAX_BYTES);
  size_t n;
  if (!b) {
    close(f);
    return -ENOMEM;
  }
  rc = nzm_io_check(f, S_IFREG, &meta);
  if (rc) nzm_diag_config_error("metadata","<file>","requires regular single-link exact metadata",rc);
  if (!rc) {
    rc = nzm_io_read_fd(f, b, NZM_CONFIG_MAX_BYTES, &n);
    if (rc) nzm_diag_config_error("read","<file>","bounded config read failed",rc);
  }
  if (!rc) {
    struct nzm_config_error e;
    rc = nzm_config_parse_ex(b, n, &c->config, &e);
    if (rc) nzm_diag_config_error("parse",e.key,e.reason,rc);
  }
  free(b);
  close(f);
  return rc;
}
int nzm_core_bootid(struct nzm_core *c) {
  char b[64];
  int rc = nzm_rawread("/proc/sys/kernel/random/boot_id", b, sizeof b);
  if (rc)
    return rc;
  if (strlen(b) != 37 || b[36] != '\n')
    return -EINVAL;
  for (int i = 0; i < 36; i++)
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (b[i] != '-')
        return -EINVAL;
    } else if (!((b[i] >= '0' && b[i] <= '9') || (b[i] >= 'a' && b[i] <= 'f')))
      return -EINVAL;
  b[36] = 0;
  strcpy(c->boot, b);
  return 0;
}
static int start(struct nzm_core *c, int phase1) {
  (void)phase1;
  int rc = nzm_core_bootid(c);
  if (!rc) rc = nzm_state_open(&c->startupfd, &c->wearfd);
  if (rc) return rc;
  if ((rc = nzm_boot_lock(&c->boot_state, c->wearfd))) return rc;
  if ((rc = nzm_core_authority(c))) return rc;
  c->lock.fd = c->boot_state.authority_fd;
  c->lock.dev = c->boot_state.authority_dev;
  c->lock.ino = c->boot_state.authority_ino;
  /* Bind startup-mutating configuration, not scheduler knobs or structure
   * padding. Same-boot config changes never acquire or retune a live device. */
  char binding[NZM_BOOT_BINDING_MAX];
  int n = snprintf(binding, sizeof binding,
      "%" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64
      " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64,
      c->config.enable_writeback, c->config.expected_bytes, c->config.disksize_bytes,
      c->config.vm_swappiness, c->config.vm_page_cluster,
      c->config.vm_watermark_scale_factor, c->config.vm_extra_free_kbytes,
      c->config.vm_min_free_kbytes, c->config.vm_minfree_mult,
      c->config.vm_overcommit_memory, c->config.vm_laptop_mode);
  if (n < 0 || (size_t)n >= sizeof binding) return -EOVERFLOW;
  rc = nzm_boot_load(&c->boot_state, c->wearfd);
  if (!rc && !strcmp(c->boot_state.boot, c->boot))
    return strcmp(c->boot_state.binding, binding) ? -ESTALE : 0;
  if (rc && rc != -ENOENT) return rc;
  /* A missing or previous-boot checkpoint is never proof of ownership. */
  if ((rc = nzm_cold()) || (rc = nzm_no_mapping())) return rc;
  if ((rc = nzm_core_authority(c))) return rc;
  return nzm_boot_reset(&c->boot_state, c->boot, binding);
}
#ifdef NZM_LEGACY_TEST
static int data_ready(struct nzm_core *c) {
  /* One recovery attempt only. Handshake/storage uncertainty never retries. */
  return nzm_core_provision(c);
}
int nzm_core_boot(struct nzm_core *c) {
  int rc = start(c, 0);
  if (rc)
    return nzm_diag(c, "startup_lock", rc);
  if ((rc = nzm_mounts_verify()))
    return nzm_diag(c, "mounted_provenance", rc);
  /* Historical module entry only: its frozen executor consumes wear-v1.
   * Production CLI uses nzm_phase1_start/demand_watch and the wear-v2 store. */
  struct nzm_ledger ledger = {0};
  rc = nzm_wear(c, &ledger);
  nzm_ledger_free(&ledger);
  if (rc)
    return nzm_stop(c, "authority_ledger", rc);
  rc = nzm_state_absent(c, "managed");
  if (rc == -EEXIST) {
    if ((rc = nzm_managed(c)))
      return nzm_stop(c, "resume_identity", rc);
    rc = nzm_core_vm(c, 0);
    if (rc) return nzm_stop(c, "resume_vm", rc);
    rc = nzm_execution_select(c);
    return rc ? nzm_stop(c, "backend_select", rc) : 0;
  }
  if (rc)
    return nzm_stop(c, "marker", rc);
  if ((rc = nzm_state_absent(c, "acquire-intent")) ||
      (rc = nzm_state_absent(c, "bootstrap-result")) || (rc = nzm_cold()) ||
      (rc = nzm_no_mapping()))
    return nzm_stop(c, "cold_ownership", rc);
  if((rc=nzm_core_control(c,NZM_BOUNDARY_OBSERVING)))return rc;
  if ((rc = data_ready(c)))
    return c->cooperative_stop ? rc : nzm_stop(c, "data_provision", rc);
  if((rc=nzm_core_control(c,NZM_BOUNDARY_OBSERVING)))return rc;
  /* Once acquire-intent is durable, finish conservative acquisition or retain
   * all evidence on failure; stop never undoes a partially managed device. */
  if ((rc = nzm_mark(c, "acquire-intent", c->boot)))
    return nzm_stop(c, "acquire_intent", rc);
  struct nzm_helper_identity id = {.bytes = c->config.expected_bytes};
  struct nzm_helper_result r;
  rc = nzm_absent(c->datafd, "file");
  if (!rc) {
    if ((rc = nzm_helper(c, NZM_HELPER_PREPARE, &id, &r)))
      return nzm_stop(c, "prepare_uncertain", rc);
  } else if (rc != -EEXIST)
    return nzm_stop(c, "file", rc);
  if ((rc = nzm_identity(c, &id)) || (rc = nzm_space(c)) || (rc = nzm_cold()))
    return nzm_stop(c, "file_space_cold", rc);
  if ((rc = nzm_helper(c, NZM_HELPER_CREATE, &id, &r)))
    return nzm_stop(c, "create_uncertain_retained", rc);
  id.require_dm = 1;
  id.dm_major = r.dm_major;
  id.dm_minor = r.dm_minor;
  if ((rc = nzm_helper(c, NZM_HELPER_QUERY, &id, &r)) || (rc = nzm_cold()) ||
      (rc = nzm_helper(c, NZM_HELPER_BIND, &id, &r)))
    return nzm_stop(c, "bind_uncertain_retained", rc);
  if ((rc = nzm_activate(c)))
    return nzm_stop(c, "activate_retained", rc);
  char b[256];
  snprintf(b, sizeof b,
           "native-v1 %s %" PRIu64 " %" PRIu64 " %" PRIu64 " %u:%u\n", c->boot,
           id.fs_dev, id.inode, id.bytes, r.dm_major, r.dm_minor);
  if ((rc = nzm_mark(c, "managed", b)) || (rc = nzm_managed(c)) ||
      (rc = nzm_core_vm(c, 0)))
    return nzm_stop(c, "marker_vm", rc);
  if ((rc = nzm_execution_select(c)))
    return nzm_stop(c, "backend_select", rc);
  return nzm_diag(c, "boot_verified", 0);
}
#endif
static int completed(void) {
#ifdef __ANDROID__
  char b[PROP_VALUE_MAX];
  int n = __system_property_get("sys.boot_completed", b);
  return n == 1 && b[0] == '1';
#else
  return 0;
#endif
}
int nzm_phase1_start(struct nzm_core *c, int daemon) {
  int rc = start(c, 1);
  if (rc)
    return nzm_diag(c, "startup_lock", rc);
  if (daemon) {
    fputs("config_scope=startup_loaded next_boot_only=1 reload=unsupported\n", stderr);
    nzm_config_snapshot(c, stderr, 1);
  }
  if ((rc = nzm_data_verify()))
    return nzm_diag(c, "data_provenance", rc);
  int paused;
  if ((rc = nzm_phase1_policy(c, &paused)))
    return nzm_diag(c, "authority_or_pending", rc);
  if ((rc = nzm_dm_boot(c)))
    return nzm_diag(c, "dm_or_ram_activation", rc);
  if ((rc = nzm_core_vm(c, 0)))
    return nzm_diag(c, "vm_boot", rc);
  int bound = nzm_state_absent(c, "managed");
  if (bound && bound != -EEXIST) return bound;
  nzm_diag(c, bound == -EEXIST ? (paused ? "paused_dm_bound_disarmed" : "dm_bound_disarmed")
                             : (paused ? "paused_ram_only_verified" : "ram_only_verified"), 0);
  if (!daemon)
    return 0;
  while (!c->trusted_boot_completed && !completed()) {
    if ((rc = nzm_sleep_seconds(5)))
      return nzm_diag(c, "waiting_completed", rc);
  }
  if ((rc = nzm_core_vm(c, 1)))
    return nzm_diag(c, "vm_completed", rc);
  c->boot_completed = 1;
  int done=nzm_diag(c, bound == -EEXIST ? "dm_bound_disarmed_completed" : "ram_only_completed", 0);
  if(done || bound!=-EEXIST || !c->config.demand_enabled || paused)return done;
  int demand=nzm_demand_watch(c);
  if(demand)nzm_diag(c,"demand_disabled_swap_retained",demand);
  return 0;
}
#ifdef NZM_LEGACY_TEST
int nzm_core_ready(struct nzm_core *c) {
  int rc = nzm_core_boot(c);
  if (rc)
    return rc;
  while (!c->trusted_boot_completed && !completed()) {
    if((rc=nzm_core_observe(c,5)))return rc;
    if ((rc = nzm_boundary(c)))
      return nzm_stop(c, "waiting_completed_boundary", rc);
  }
  if((rc=nzm_core_control(c,NZM_BOUNDARY_OBSERVING)))return rc;
  if ((rc = nzm_core_vm(c, 1)))
    return nzm_stop(c, "vm_completed", rc);
  c->boot_completed = 1;
  return 0;
}
int nzm_core_daemon(struct nzm_core *c) {
  if(c->config.runtime_event_enabled)return -EINVAL; /* Use event owner API. */
  fputs("config_scope=startup_loaded next_boot_only=1 reload=unsupported\n",stderr);
  nzm_config_snapshot(c,stderr,1);
  int rc=nzm_core_ready(c);
  if(rc)return rc;
  if (!c->config.enable_writeback)
    return nzm_diag(c, "writeback_disabled", 0);
  for (;;) {
    if ((rc = nzm_core_cycle(c)))
      return rc;
    if ((rc = nzm_sleep_seconds(c->config.sample_interval_seconds)))
      return rc;
  }
}

#endif /* NZM_LEGACY_TEST */
