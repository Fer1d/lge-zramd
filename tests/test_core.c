#define _GNU_SOURCE
#include "nzm_core.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/xattr.h>
#include <unistd.h>
static char root[256];
static int writes, opens_write, deny_vm, wrong_data, wrong_owner, short_write;
static int config_fault;
#ifdef NZM_INTEGRATION
static void verify_precharge_at_request(void);
static int fail_wb, fail_snapshot_short, fail_snapshot_sync, fail_attempt_open;
static uint64_t captured_submit_tick;
static int fail_pause_create, intent_fault, intent_dirsync, intent_retire_sync;
static unsigned intent_syncs;
static int rb_mode, rb_status_errno, rb_exec_errno, rb_bad_status, rb_bad_exec;
static int rb_invalid_samples, rb_global_enabled, rb_op_errno, rb_page_mismatch;
static int rb_wrong_device, rb_wrong_label, rb_fail_open, rb_quota_writes, rb_idle_writes;
static int rb_fd=-1;
static unsigned rb_status_calls, rb_exec_calls, rb_legacy_calls, rb_closes;
static uint64_t rb_remaining=UINT64_MAX, rb_generation=7, rb_installed=1;
static uint64_t rb_last_id, rb_status_page=4096, rb_disk=67108864;
static uint32_t rb_stop_reason=1;
static int request_fixture_ioctl(int, unsigned long, void *);
static void capture_submit_tick(void);
static void request_effect(void);
#endif
static void fdpath(int fd, char *p, size_t cap) {
  char q[64];
  snprintf(q, sizeof q, "/proc/self/fd/%d", fd);
  ssize_t n = readlink(q, p, cap - 1);
  assert(n >= 0);
  p[n] = 0;
}
int test_open(const char *p, int flags, ...) {
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list a;
    va_start(a, flags);
    mode = (mode_t)va_arg(a, int);
    va_end(a);
  }
  char b[1024];
  if (p[0] == '/') {
    snprintf(b, sizeof b, "%s%s", root, p);
    p = b;
  }
  if (flags & (O_WRONLY | O_RDWR | O_CREAT))
    opens_write++;
  if (deny_vm && strstr(p, "/proc/sys/vm/")) {
    errno = EACCES;
    return -1;
  }
#ifdef NZM_INTEGRATION
  if(rb_fail_open && strstr(p,"/dev/block/zram0")) {errno=EACCES;return -1;}
  if(fail_attempt_open && (flags&O_WRONLY) && strstr(p,"/sys/block/zram0/writeback") &&
      !strcmp(p+strlen(p)-10,"/writeback")) {errno=EACCES;return -1;}
#endif
  return open(p, flags, mode);
}
int test_fstat(int fd, struct stat *s) {
  int r = fstat(fd, s);
  if (r)
    return r;
  char p[1024];
  fdpath(fd, p, sizeof p);
  s->st_uid = s->st_gid = 0;
  if (!strcmp(p + strlen(root), "/vendor/etc/lge-zram.conf")) {
    if (config_fault == 1) s->st_uid = 1;
    if (config_fault == 2) s->st_gid = 1;
    if (config_fault == 3) s->st_nlink = 2;
    if (config_fault == 4) s->st_mode = S_IFDIR | 0600;
  }
  if (!strcmp(p + strlen(root), "/mnt/vendor/persist")) {
    s->st_gid = 1000;
    s->st_mode = S_IFDIR | 0771;
  }
#ifdef NZM_INTEGRATION
  if (!strcmp(p + strlen(root), "/dev/block/zram0")) {
    s->st_mode = S_IFBLK | 0600;
    s->st_rdev = rb_wrong_device ? 2 : 1;
  }
#endif
  if (!strcmp(p + strlen(root), "/data")) {
    s->st_uid = s->st_gid = 1000;
    if (wrong_data)
      s->st_gid = 1001;
  }
  return 0;
}
ssize_t test_xattr(int fd, const char *name, void *buf, size_t cap) {
  (void)name;
  char p[1024];
  fdpath(fd, p, sizeof p);
  const char *type = "lge_zram_runtime_file";
  if (!strcmp(p+strlen(root),"/dev/block/zram0")) {
    type = "lge_zram0_device";
#ifdef NZM_INTEGRATION
    if(rb_wrong_label)type="wrong_zram_device";
#endif
  } else if (strstr(p, "/vendor/etc/lge-zram.conf")) {
    if (config_fault == 6) { errno = ENODATA; return -1; }
    type = config_fault == 5 ? "lge_zram_config_file" :
           config_fault == 7 ? "vendor_configs_file_suffix" : "vendor_configs_file";
  } else if (strstr(p, "/lge-zram-wear")) {
    type = "extm_wear_dir";
    if (strstr(p, "/authority"))
      type = "extm_wear_authority";
    else if (strstr(p, "/ledger") || strstr(p,"/exec.intent"))
      type = "extm_wear_ledger";
    else if (strstr(p, "/pause"))
      type = "extm_wear_pause";
  } else if (strstr(p, "/data/extm")) {
    type = "extm_root_data";
    if (strstr(p, "/owner"))
      type = wrong_owner ? "extm_backing_data" : "extm_owner_data";
    else if (strstr(p, "/file"))
      type = "extm_backing_data";
  }
  char b[128];
  snprintf(b, sizeof b, "u:object_r:%s:s0", type);
  size_t n = strlen(b) + 1;
  if (n > cap) {
    errno = ERANGE;
    return -1;
  }
  memcpy(buf, b, n);
  return (ssize_t)n;
}
ssize_t test_write(int fd, const void *b, size_t n) {
  writes++;
  if (short_write)
    return write(fd, b, n / 2);
  char p[1024];
  fdpath(fd, p, sizeof p);
#ifdef NZM_INTEGRATION
  if(intent_fault==2 && strstr(p,"/exec.intent"))return write(fd,b,n/2);
  if (!strcmp(p + strlen(root), "/sys/block/zram0/disksize"))
    return write(fd, "67108864\n", 9) == 9 ? (ssize_t)n : -1;
  if (fail_snapshot_short && strstr(p, "/last-event"))
    return write(fd, b, n / 2);
  if(strstr(p+strlen(root),"/sys/block/zram0/writeback_limit"))rb_quota_writes++;
  if(!strcmp(p+strlen(root),"/sys/block/zram0/idle"))rb_idle_writes++;
  if (!strcmp(p + strlen(root), "/sys/block/zram0/writeback")) {
    rb_legacy_calls++;
    capture_submit_tick();
    verify_precharge_at_request();
    request_effect();
    if (fail_wb) {
      errno = EIO;
      return -1;
    }
  }
#endif
  if (strstr(p, "/proc/sys/vm/") || strstr(p, "/sys/block/zram0/")) {
    assert(!ftruncate(fd, 0));
    assert(lseek(fd, 0, SEEK_SET) == 0);
  }
  return write(fd, b, n);
}
static void path(char *p, const char *s) { snprintf(p, 1024, "%s%s", root, s); }
static void dir(const char *s, mode_t mode) {
  char p[1024];
  path(p, s);
  assert(!mkdir(p, mode));
  assert(!chmod(p, mode));
}
static void file(const char *s, const char *value) {
  char p[1024];
  path(p, s);
  int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (!strcmp(s,"/vendor/etc/lge-zram.conf")) assert(!fchmod(fd,0644));
  assert(fd >= 0);
  assert(write(fd, value, strlen(value)) == (ssize_t)strlen(value));
  assert(!close(fd));
}
static void remove_file(const char *s) {
  char p[1024];
  path(p, s);
  assert(!unlink(p));
}
static uint64_t readnum(const char *s) {
  char p[1024], b[64];
  path(p, s);
  int f = open(p, O_RDONLY);
  assert(f >= 0);
  ssize_t n = read(f, b, sizeof b - 1);
  assert(n > 0);
  b[n] = 0;
  close(f);
  return strtoull(b, NULL, 10);
}
int test_stat(const char *p, struct stat *s) {
  if (!strcmp(p, "/dev/block/zram0")) {
    memset(s, 0, sizeof *s);
    s->st_mode = S_IFBLK | 0600;
    s->st_rdev = 1;
    return 0;
  }
  char b[1024];
  path(b, p);
  return stat(b, s);
}
static void setup(void) {
  strcpy(root, "/tmp/nzm-core-XXXXXX");
  assert(mkdtemp(root));
  dir("/dev", 0755);
  dir("/dev/lge-zram-manager", 0700);
  dir("/proc", 0755);
  dir("/proc/sys", 0755);
  dir("/proc/sys/vm", 0755);
  dir("/data", 0771);
  dir("/mnt", 0755);
  dir("/mnt/vendor", 0755);
  dir("/mnt/vendor/persist", 0755);
  dir("/mnt/vendor/persist/lge-zram-wear", 0700);
  file("/mnt/vendor/persist/lge-zram-wear/authority", "extm-wear-v1\n");
  file("/mnt/vendor/persist/lge-zram-wear/ledger", "wear-v1 0 0 0\n");
  const char *nodes[] = {
      "swappiness",        "page-cluster",    "watermark_scale_factor",
      "extra_free_kbytes", "min_free_kbytes", "overcommit_memory",
      "laptop_mode"};
  for (size_t i = 0; i < 7; i++) {
    char p[128];
    snprintf(p, sizeof p, "/proc/sys/vm/%s", nodes[i]);
    file(p, i == 4 ? "100\n" : "1\n");
  }
}
static void init(struct nzm_core *c) {
  nzm_core_init(c);
  char p[1024];
  path(p, "/dev/lge-zram-manager");
  c->runfd = open(p, O_RDONLY | O_DIRECTORY);
  assert(c->runfd >= 0);
}
#ifdef NZM_INTEGRATION
#include "test_core_success.inc"
#else
int main(void) {
  setup();
  struct nzm_core c;
  init(&c);
  assert(nzm_core_vm(&c, 0) == 0);
  assert(readnum("/proc/sys/vm/min_free_kbytes") == 200);
  assert(readnum("/proc/sys/vm/swappiness") == 180);
  int w = writes;
  assert(nzm_core_vm(&c, 0) == 0 && writes == w);
  file("/proc/sys/vm/min_free_kbytes", "700\n");
  assert(nzm_core_vm(&c, 1) == 0);
  assert(readnum("/proc/sys/vm/min_free_kbytes") == 200);
  w = writes;
  assert(nzm_core_vm(&c, 1) == 0 && writes == w);
  remove_file("/dev/lge-zram-manager/vm-complete-completed");
  assert(nzm_core_vm(&c, 1) < 0 &&
         writes == w); /* prior intent never retries */
  nzm_core_close(&c);
  init(&c);
  /* Direct provisioning without the startup owner lock is now forbidden.
   * Filesystem recovery positives/faults are covered in test_recovery.c. */
  assert(nzm_core_provision(&c) == -EPERM);
  FILE *out = tmpfile();
  assert(out);
  w = writes;
  int ow = opens_write;
  assert(nzm_core_status(&c, out) == 0);
  assert(w == writes && ow == opens_write);
  rewind(out);
  char snapshot[4096];
  size_t snapshot_len = fread(snapshot, 1, sizeof snapshot - 1, out);
  snapshot[snapshot_len] = 0;
  assert(strstr(snapshot, "config_source=/vendor/etc/lge-zram.conf scope=disk_snapshot"));
  assert(strstr(snapshot, "running_daemon_config=unknown"));
  const char *snapshot_fields[] = {
    "enable_writeback=1 unit=boolean", "backing_mib=512 unit=MiB",
    "wait_log_interval_seconds=300 unit=seconds",
    "daily_mib=64 unit=MiB/rolling_86400_seconds",
    "pool_low_mib=512 unit=MiB", "pool_high_mib=768 unit=MiB",
    "cold_age_seconds=60 unit=seconds", "demand_batch_mib=4 unit=MiB",
    "demand_enabled=0 unit=boolean", "demand_scan_limit=4096 unit=entries",
    "min_data_free_mib=2048 unit=MiB",
    "legacy_scheduling_keys=deprecated_ignored legacy_fallback=disabled"
  };
  for (size_t i=0; i<sizeof snapshot_fields/sizeof snapshot_fields[0]; ++i)
    assert(strstr(snapshot, snapshot_fields[i]));
  fclose(out);
  struct nzm_config cfg;
  nzm_config_default(&cfg);
  uint64_t mm[10] =
      {512 * 1048576ULL, 0, 128 * 1048576ULL, 0, 0, 0, 0, 0, 0, 4096},
           bd[3] = {0}, upper;
  assert(nzm_core_workload(&cfg, 4096, mm, bd, &upper) == 0 && upper == 4096);
  mm[9] = 1023;
  assert(nzm_core_workload(&cfg, 4096, mm, bd, &upper) == 0 && upper == 1023);
  mm[9] = 4096;
  mm[5] = UINT64_MAX;
  assert(nzm_core_workload(&cfg, 4096, mm, bd, &upper) == -EINVAL);
  mm[5] = 0;
  bd[0] = UINT64_MAX;
  assert(nzm_core_workload(&cfg, 4096, mm, bd, &upper) == -ERANGE);
  bd[0] = 0;
  assert(nzm_core_workload(&cfg, 8192, mm, bd, &upper) == -ENOTSUP);
  nzm_core_close(&c); /* new fixture for failure markers */
  setup();
  init(&c);
  deny_vm = 1;
  w = writes;
  assert(nzm_core_vm(&c, 0) == -EACCES && writes == w);
  deny_vm = 0;
  short_write = 1;
  assert(nzm_core_vm(&c, 0) < 0);
  short_write = 0;
  assert(nzm_core_vm(&c, 0) < 0);
  nzm_core_close(&c);
  /* Full boot command runs the production startup/lock/authority/cold/DM scan
   * and provisioning paths, then refuses an unknown child before helper exec.
   */
  setup();
  dir("/proc/sys/kernel", 0755);
  dir("/proc/sys/kernel/random", 0755);
  file("/proc/sys/kernel/random/boot_id",
       "01234567-89ab-cdef-0123-456789abcdef\n");
  dir("/sys", 0755);
  dir("/sys/block", 0755);
  dir("/sys/block/zram0", 0755);
  file("/sys/block/zram0/disksize", "0\n");
  file("/sys/block/zram0/backing_dev", "none\n");
  file("/proc/swaps", "Filename Type Size Used Priority\n");
  dir("/data/extm", 0700);
  file("/data/extm/foreign", "unknown\n");
  nzm_core_init(&c);
  assert(nzm_core_boot(&c) == -ENOTEMPTY);
  assert(!strcmp(c.stage, "data_provision"));
  char pause_path[1024];
  path(pause_path, "/mnt/vendor/persist/lge-zram-wear/pause");
  assert(access(pause_path, F_OK) == 0);
  nzm_core_close(&c);
  puts("core: VM real read/write/readback, once-only x2/completed, "
       "metadata/path/provision failures, unknown/partial, status no writes, "
       "workload units: PASS");
  return 0;
}

#endif
