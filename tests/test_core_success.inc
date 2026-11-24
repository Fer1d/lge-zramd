/* Explicit legacy-v1 scheduling regressions, never a production fallback.
 * Current production phase1/status behavior has independent fixture cases. */
#ifndef NZM_LEGACY_TEST
#error "Historical scheduling fixtures require NZM_LEGACY_TEST"
#endif
#include <dirent.h>
#include <linux/fs.h>
#include "uapi/linux/zram.h"
#include <sys/statvfs.h>
#include <time.h>
int test_close(int fd) {
  if(rb_mode && fd==rb_fd) {rb_closes++;rb_fd=-1;}
  return close(fd);
}
int test_unlinkat(int fd,const char *name,int flags) {
  if(!strcmp(name,"exec.intent") && intent_fault==5) {errno=EACCES;return -1;}
  int rc=unlinkat(fd,name,flags);
  if(!rc && !strcmp(name,"exec.intent"))intent_retire_sync=1;
  return rc;
}
int test_fsync(int fd) {
  char p[1024];
  fdpath(fd, p, sizeof p);
  if(intent_retire_sync && !strcmp(p+strlen(root),"/data/vendor/extm/state")) {
    intent_retire_sync=0;
    if(intent_fault==6) {errno=EIO;return -1;}
  }
  if(strstr(p,"/exec.intent")) {
    intent_syncs++;intent_dirsync=1;
    if(intent_fault==3) {errno=EIO;return -1;}
  } else if(intent_dirsync && !strcmp(p+strlen(root),"/data/vendor/extm/state")) {
    intent_dirsync=0;
    if(intent_fault==4) {errno=EIO;return -1;}
  }
  if (fail_snapshot_sync && strstr(p, "/last-event")) {
    errno = EIO;
    return -1;
  }
  return fsync(fd);
}
static int fail_readdir, data_sleeps;
static time_t monotonic_tick = 100;
static int advance_idle_clock;
static uint64_t helper_backing_mib=512;
static void capture_submit_tick(void) {captured_submit_tick=(uint64_t)monotonic_tick;}
int test_clock(clockid_t id, struct timespec *t) {
  assert(id == CLOCK_MONOTONIC);
  t->tv_sec = monotonic_tick;
  t->tv_nsec = 0;
  return 0;
}
struct dirent *test_readdir(DIR *d) {
  char p[1024];
  fdpath(dirfd(d), p, sizeof p);
  if (fail_readdir && !strcmp(p + strlen(root), "/sys/block")) {
    errno = EIO;
    return NULL;
  }
  return readdir(d);
}
static unsigned helper_ops[4], swap_calls, request_calls, sleeps;
static int timing_probe;
static int watermark_probe;
static int shrink_during_wait, grow_during_wait;
static const char *switch_power, *switch_screen;
static uint64_t observed_idle;
static uint64_t cycle_page=4096;
static int low_data_space, query_fail_at, query_counter, identity_on_wait;
static int bd_effect;
static uint64_t attempted_effect;
long test_sysconf(int name) { return name==_SC_PAGESIZE ? (long)cycle_page : sysconf(name); }
static void request_effect(void) {
  if(!watermark_probe || (!bd_effect && !attempted_effect))return;
  char p[1024];path(p,"/sys/block/zram0/bd_stat");
  FILE *f=fopen(p,"r");assert(f);unsigned long long b,r,w;
  assert(fscanf(f,"%llu %llu %llu",&b,&r,&w)==3);fclose(f);
  if(bd_effect>0)b+=readnum("/sys/block/zram0/writeback_limit");
  if(bd_effect<0)b=0;
  f=fopen(p,"w");assert(f);fprintf(f,"%llu %llu %llu\n",b,r,w+attempted_effect);fclose(f);
}

static uint64_t expected_charge=4096;
static unsigned chunk_calls;
static void verify_precharge_at_request(void) {
  char p[1024], b[4096];
  path(p, "/data/vendor/extm/state/ledger");
  int f = open(p, O_RDONLY);
  assert(f >= 0);
  ssize_t n = read(f, b, sizeof b - 1);
  assert(n > 0);
  b[n] = 0;
  close(f);
  struct nzm_ledger l = {0};
  assert(!nzm_ledger_parse(b, (size_t)n, (uint64_t)time(NULL), &l));
  if(watermark_probe)assert(l.count && l.records[l.count-1].kib==expected_charge);
  else assert(l.generation == 1 && l.cumulative == expected_charge && l.count == 1);
  nzm_ledger_free(&l);
  path(p, "/data/vendor/extm/state/ledger.txn");
  assert(access(p, F_OK) < 0 && errno == ENOENT);
  path(p, "/data/vendor/extm/state/ledger.new");
  assert(access(p, F_OK) < 0 && errno == ENOENT);
  request_calls++;
}
int nzm_entry(int, char **);
/* Keep the original Stage 3 module regressions without exposing their
 * write-capable startup via the Phase 1 CLI, which has its own fixtures. */
/* Test-only historical timing profile. Production parsing remains unchanged. */
static int legacy_module_config(struct nzm_core *c) {
  int rc=nzm_core_config(c);
  if (!rc) {
    c->config.idle_observation_seconds=301;
    c->config.sample_interval_seconds=503;
    c->config.watermark_charging_batch_mib=4;
    c->config.watermark_charging_idle_observation_seconds=301;
    c->config.watermark_battery_idle_observation_seconds=301;
    c->config.watermark_charging_min_interval_seconds=0;
  }
  return rc;
}
static char stage3_last_event[1200];
/* Read persisted checkpoints through the production parser, never marker files. */
static int checkpoint_present(const char *name) {
  char p[1024]; path(p,"/data/vendor/extm/state");
  int fd=open(p,O_RDONLY|O_DIRECTORY|O_CLOEXEC); assert(fd>=0);
  struct nzm_boot_state state; nzm_boot_init(&state);
  assert(!nzm_boot_load(&state,fd));
  int rc=nzm_boot_absent(&state,name); assert(!rc || rc==-EEXIST);
  assert(!close(fd)); return rc==-EEXIST;
}
static int stage3_start(int daemon, int trusted) {
  struct nzm_core c;
  nzm_core_init(&c);
  c.trusted_boot_completed=trusted;
  int rc=legacy_module_config(&c);
  if (!rc) rc=daemon ? nzm_core_daemon(&c) : nzm_core_boot(&c);
  snprintf(stage3_last_event,sizeof stage3_last_event,"%s",c.last_event);
  nzm_core_close(&c);
  return !!rc;
}
static uid_t command_euid;
uid_t test_geteuid(void) { return command_euid; }
static int boot_prop_unreadable, boot_prop_reads, boot_waits;
int __system_property_get(const char *name, char *value) {
  if(!strcmp(name,"ro.crypto.type")) { value[0]=0; return 0; }
  assert(!strcmp(name, "sys.boot_completed"));
  boot_prop_reads++;
  if (boot_prop_unreadable) { value[0]=0; return 0; }
  strcpy(value, "1");
  return 1;
}
int test_ioctl(int fd, unsigned long op, ...) {
  (void)fd;
  if(op==ZRAM_IOC_WB_STATUS || op==ZRAM_IOC_WB_EXEC) {
    va_list args;va_start(args,op);void *arg=va_arg(args,void *);va_end(args);
    if(rb_mode)return request_fixture_ioctl(fd,op,arg);
    assert(op==ZRAM_IOC_WB_STATUS);rb_status_calls++;errno=ENOTTY;return -1;
  }
  assert(op == BLKGETSIZE64);
  va_list a;
  va_start(a, op);
  uint64_t *bytes = va_arg(a, uint64_t *);
  *bytes = 64 * 1048576ULL;
  va_end(a);
  return 0;
}
int test_space(int fd, struct statvfs *s) {
  (void)fd;
  memset(s, 0, sizeof *s);
  s->f_frsize = 4096;
  s->f_bavail = low_data_space ? 0 : 1048576;
  return 0;
}
int test_swapon(const char *name, int flags) {
  assert(!strcmp(name, "/dev/block/zram0") && !flags);
  char p[1024], header[4096];
  path(p, name);
  int f = open(p, O_RDONLY);
  assert(f >= 0);
  assert(read(f, header, sizeof header) == sizeof header);
  close(f);
  assert(!memcmp(header + 4096 - 10, "SWAPSPACE2", 10));
  uint32_t version, last;
  memcpy(&version, header + 1024, 4);
  memcpy(&last, header + 1028, 4);
  assert(version == 1 && last == 16383); /* native mkswap, not bypassed */
  swap_calls++;
  file("/proc/swaps", "Filename Type Size Used Priority\n/dev/block/zram0 "
                      "partition 65532 0 -2\n");
  return 0;
}
int test_helper_call(enum nzm_helper_op op,
                     const struct nzm_helper_identity *id, unsigned timeout,
                     struct nzm_helper_run *run,
                     struct nzm_helper_result *result) {
  assert(timeout == 120000 && id->bytes == helper_backing_mib * 1048576ULL);
  helper_ops[op]++;
  if(op==NZM_HELPER_QUERY && query_fail_at && ++query_counter==query_fail_at)return -ESTALE;
  memset(run, 0, sizeof *run);
  memset(result, 0, sizeof *result);
  run->attempted = run->reaped = 1;
  if (op == NZM_HELPER_PREPARE) {
    char p[1024];
    path(p, "/data/vendor/extm/file");
    int f = open(p, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(f >= 0);
    assert(!ftruncate(f, (off_t)id->bytes));
    close(f);
    return 0;
  }
  assert(id->inode && id->fs_dev);
  result->dm_major = 253;
  result->dm_minor = 0;
  result->extents = 1;
  if (op == NZM_HELPER_CREATE) {
    assert(!id->require_dm);
    dir("/sys/block/dm-0", 0755);
    dir("/sys/block/dm-0/dm", 0755);
    file("/sys/block/dm-0/dm/name", "extm-file\n");
    file("/sys/block/dm-0/dev", "253:0\n");
  } else {
    assert(id->require_dm && id->dm_major == 253 && !id->dm_minor);
    if (op == NZM_HELPER_BIND) {
      result->bound = 1;
      file("/sys/block/zram0/backing_dev", "/dev/block/dm-0\n");
    }
  }
  return 0;
}
int test_helper_call_with_mask(enum nzm_helper_op op,
    const struct nzm_helper_identity *id, unsigned timeout,
    const struct nzm_helper_stop *stop, const sigset_t *mask,
    struct nzm_helper_run *run, struct nzm_helper_result *result) {
  assert(!stop);
  if(mask)assert(sigismember(mask,SIGTERM)==0);
  return test_helper_call(op,id,timeout,run,result);
}
int test_sleep(const struct timespec *t, struct timespec *left) {
  (void)left;
  if(watermark_probe) {
    observed_idle+=(uint64_t)t->tv_sec;
    if(advance_idle_clock)monotonic_tick+=t->tv_sec;
    if(switch_power)file("/sys/class/power_supply/battery/status",switch_power);
    if(switch_screen)file("/sys/class/backlight/panel0-backlight/bl_power",switch_screen);
    if(identity_on_wait) {query_counter=0;query_fail_at=1;}
    if(grow_during_wait)file("/sys/block/zram0/mm_stat","67108864 0 0 0 0 0 0 0 0 4096\n");
    if(shrink_during_wait)file("/sys/block/zram0/mm_stat","4096 0 0 0 0 0 0 0 0 1\n");
    return 0;
  }
  if(t->tv_sec==5) { boot_waits++; errno=EINTR; return -1; }
  if (timing_probe == 2) {
    assert(t->tv_sec == (chunk_calls++ ? 1 : 86400)); return 0;
  }
  if (t->tv_sec == 1) {
    data_sleeps++;
    monotonic_tick++;
    return 0;
  }
  if (timing_probe) { assert(t->tv_sec == 301); return 0; }
  sleeps++;
  if (sleeps == 1) {
    assert(t->tv_sec == 301); /* inner idle wait, not poll or log */
    return 0;
  }
  assert(sleeps == 2 && t->tv_sec == 503); /* outer loop, not idle or log */
  errno = EINTR;
  return -1; /* bounded daemon termination */
}
static void success_fixture(void) {
  setup();
  file("/data/vendor/extm/owner", "extm-dm-owner-v1\n");
  dir("/vendor", 0755);
  dir("/vendor/etc", 0755);
  file("/vendor/etc/lge-zram.conf", "idle_observation_seconds=301\nsample_interval_seconds=503\nwait_log_interval_seconds=709\nwatermark_charging_batch_mib=4\nwatermark_charging_idle_observation_seconds=301\nwatermark_battery_idle_observation_seconds=301\nwatermark_charging_min_interval_seconds=0\n");
  dir("/proc/sys/kernel", 0755);
  dir("/proc/sys/kernel/random", 0755);
  file("/proc/sys/kernel/random/boot_id",
       "01234567-89ab-cdef-0123-456789abcdef\n");
  dir("/sys", 0755);
  dir("/sys/block", 0755);
  dir("/sys/block/zram0", 0755);
  file("/sys/block/zram0/disksize", "0\n");
  file("/sys/block/zram0/dev", "0:1\n");
  file("/sys/block/zram0/backing_dev", "none\n");
  file("/proc/swaps", "Filename Type Size Used Priority\n");
  dir("/dev/block", 0755);
  file("/dev/block/zram0", "");
  dir("/sys/class", 0755);
  dir("/sys/class/backlight", 0755);
  dir("/sys/class/backlight/panel0-backlight", 0755);
  file("/sys/class/backlight/panel0-backlight/bl_power", "4\n");
  dir("/sys/class/power_supply", 0755);
  dir("/sys/class/power_supply/battery", 0755);
  file("/sys/class/power_supply/battery/status", "Charging\n");
  file("/proc/meminfo", "MemAvailable: 524288 kB\n");
  file("/sys/block/zram0/mm_stat", "67108864 0 134217728 0 0 0 0 0 0 4096\n");
  file("/sys/block/zram0/bd_stat", "0 0 0\n");
  file("/sys/block/zram0/io_stat", "0 0 0 0\n");
  file("/sys/block/zram0/idle", "");
  file("/sys/block/zram0/writeback_limit_enable", "");
  file("/sys/block/zram0/writeback_limit", "");
  file("/sys/block/zram0/writeback", "");
}
static uint64_t ledger_generation(void) {
  char p[1024],b[4096];path(p,"/data/vendor/extm/state/ledger");
  FILE *f=fopen(p,"r");assert(f);
  size_t n=fread(b,1,sizeof b,f);assert(!ferror(f));fclose(f);
  struct nzm_ledger l={0};assert(!nzm_ledger_parse(b,n,(uint64_t)time(NULL),&l));
  uint64_t generation=l.generation;nzm_ledger_free(&l);return generation;
}
#include "test_execution.inc"
int main(void) {
  /* Keep shared fixture helpers compiled/checked in this variant as well. */
  (void)init;
  (void)remove_file;
  (void)request_calls;
  success_fixture();
  /* Exact path, metadata, label, and leaf/parent nofollow use production loader. */
  struct nzm_core cfgtest;
  nzm_core_init(&cfgtest);
  assert(!nzm_core_config(&cfgtest));
  for (config_fault = 1; config_fault <= 7; config_fault++)
    assert(nzm_core_config(&cfgtest) == (config_fault == 6 ? -ENODATA : -EPERM));
  config_fault = 0;
  char cp[1024], oldcp[1024];
  path(cp, "/vendor/etc/lge-zram.conf");
  path(oldcp, "/vendor/etc/lge-zram-manager.conf");
  assert(!rename(cp, oldcp));
  assert(nzm_core_config(&cfgtest) == -ENOENT); /* old path never fallback */
  assert(!symlink("lge-zram-manager.conf", cp));
  assert(nzm_core_config(&cfgtest) == -ELOOP);
  assert(!unlink(cp) && !rename(oldcp, cp));
  assert(!chmod(cp, 0600));
  assert(nzm_core_config(&cfgtest) == -EPERM);
  assert(!chmod(cp, 0644));
  assert(!link(cp, oldcp));
  assert(nzm_core_config(&cfgtest) == -EPERM);
  assert(!unlink(oldcp));
  assert(!unlink(cp) && !mkfifo(cp, 0600));
  assert(nzm_core_config(&cfgtest) == -EPERM); /* nonblocking type reject */
  assert(!unlink(cp) && !mkdir(cp, 0600));
  assert(nzm_core_config(&cfgtest) == -EPERM);
  assert(!rmdir(cp));
  file("/vendor/etc/lge-zram.conf", "cooldown_seconds=300\n");
  assert(nzm_core_config(&cfgtest) == -EINVAL);
  file("/vendor/etc/lge-zram.conf", "idle_observation_seconds=301\nsample_interval_seconds=503\nwait_log_interval_seconds=709\nwatermark_charging_batch_mib=4\nwatermark_charging_idle_observation_seconds=301\nwatermark_battery_idle_observation_seconds=301\nwatermark_charging_min_interval_seconds=0\n");
  path(cp, "/vendor/etc"); path(oldcp, "/vendor/etc-real");
  assert(!rename(cp, oldcp) && !symlink("etc-real", cp));
  assert(nzm_core_config(&cfgtest) < 0);
  assert(!unlink(cp) && !rename(oldcp, cp));
  nzm_core_close(&cfgtest);
  puts("PASS config exact-path/no-fallback, legacy-key, metadata/label/type/link and parent/leaf nofollow");
  /* Read-only diagnostic rejects nonroot and extra arguments before all I/O.
   * In the core fixture mount evidence is intentionally incomplete: failure
   * must still have zero config/core writes, helper or provisioning effects. */
  char *mounts[] = {"native-zram-manager", "check-mounts", NULL};
  int diagnostic_writes=writes, diagnostic_opens=opens_write;
  command_euid=2000;
  assert(nzm_entry(2,mounts)==1);
  command_euid=0;
  assert(nzm_entry(3,mounts)==2);
  assert(nzm_entry(2,mounts)==1);
  char *preflight[] = {"native-zram-manager", "preflight-helper", NULL};
  command_euid=2000;assert(nzm_entry(2,preflight)==1);
  command_euid=0;assert(nzm_entry(3,preflight)==2);
  /* Unadapted host filesystem lacks approved Android helper tuple. */
  assert(nzm_entry(2,preflight)==1);
  assert(writes==diagnostic_writes && opens_write==diagnostic_opens);
  assert(!request_calls && !swap_calls);
  for(unsigned i=0;i<4;i++)assert(!helper_ops[i]);
  char *trusted[] = {"native-zram-manager", "daemon", "--confirmed", "--boot-completed", NULL};
  command_euid=2000; assert(nzm_entry(4,trusted)==1);
  command_euid=0; assert(nzm_entry(3,trusted+1)==2);
  assert(nzm_entry(5,trusted)==2);
  char *wrong[] = {"native-zram-manager", "boot", "--confirmed", "--boot-completed", NULL};
  assert(nzm_entry(4,wrong)==2);
  char *reverse[] = {"native-zram-manager", "daemon", "--boot-completed", "--confirmed", NULL};
  assert(nzm_entry(4,reverse)==2);
  assert(writes==diagnostic_writes && opens_write==diagnostic_opens);
  assert(stage3_start(0,0) == 0);
  assert(swap_calls == 1);
  assert(helper_ops[NZM_HELPER_PREPARE] == 1 &&
         helper_ops[NZM_HELPER_CREATE] == 1 &&
         helper_ops[NZM_HELPER_BIND] == 1);
  assert(readnum("/proc/sys/vm/min_free_kbytes") == 200);
  /* Full budget reaches actual waiting_budget diagnostic, no reservation/refund. */
  char ledger_text[128];
  unsigned long long now = (unsigned long long)time(NULL);
  snprintf(ledger_text, sizeof ledger_text, "wear-v1 1 %llu 65536\n%llu 65536\n", now, now);
  file("/data/vendor/extm/state/ledger", ledger_text);
  struct nzm_core timed;
  nzm_core_init(&timed);
  assert(!legacy_module_config(&timed));
  assert(!nzm_core_boot(&timed));
  timed.boot_completed=1;
  timing_probe = 1;
  monotonic_tick = 1000;
  char capture_path[1024]; path(capture_path, "/timing-log");
  FILE *captured = fopen(capture_path, "w+"); assert(captured);
  fflush(stderr);
  int stderr_saved = dup(STDERR_FILENO); assert(stderr_saved >= 0);
  assert(dup2(fileno(captured), STDERR_FILENO) >= 0);
  assert(!nzm_core_cycle(&timed) && timed.last_wait_log == 1000);
  monotonic_tick = 1708;
  assert(!nzm_core_cycle(&timed) && timed.last_wait_log == 1000);
  monotonic_tick = 1709;
  assert(!nzm_core_cycle(&timed) && timed.last_wait_log == 1709);
  fflush(stderr); rewind(captured);
  char logline[1024]; unsigned logcount = 0;
  while (fgets(logline, sizeof logline, captured)) {
    if (strstr(logline, "stage=waiting_budget ")) logcount++;
  }
  assert(logcount == 2); /* 709 throttle, not 301 idle or 503 polling */
  assert(dup2(stderr_saved, STDERR_FILENO) >= 0); close(stderr_saved); fclose(captured);
  timed.config.watermark_charging_idle_observation_seconds=86401;
  timing_probe=2; chunk_calls=0;
  assert(!nzm_core_cycle(&timed) && chunk_calls==2);
  timed.config.watermark_charging_idle_observation_seconds=0;
  assert(!nzm_core_cycle(&timed) && chunk_calls==2); /* zero: no nanosleep */
  timed.config.wait_log_interval_seconds=UINT64_MAX;
  monotonic_tick=1; /* regression must not underflow into a giant elapsed */
  assert(!nzm_core_cycle(&timed) && timed.last_wait_log==1);
  nzm_core_close(&timed);
  timing_probe = 0;
  file("/data/vendor/extm/state/ledger", "wear-v1 0 0 0\n");
  puts("PASS actual independent inner=301 outer=503 log=709 (log at 1000/1709, suppressed 1708)");
  char *check[] = {"native-zram-manager", "check-config", NULL};
  int check_writes=writes, check_opens=opens_write;
  unsigned check_helpers=helper_ops[NZM_HELPER_QUERY];
  assert(nzm_entry(2, check) == 0);
  assert(writes==check_writes && opens_write==check_opens && helper_ops[NZM_HELPER_QUERY]==check_helpers);
  unsigned before_queries = helper_ops[NZM_HELPER_QUERY];
  int before_writes = writes, before_opens = opens_write;
  char *status[] = {"native-zram-manager", "status", NULL};
  assert(nzm_entry(2, status) == 0);
  assert(helper_ops[NZM_HELPER_QUERY] == before_queries &&
         writes == before_writes && opens_write == before_opens);
  char p[1024];
  assert(checkpoint_present("managed"));
  boot_prop_unreadable=1;
  assert(stage3_start(1,0)==1);
  assert(boot_prop_reads==1 && boot_waits==1 && !request_calls);
  assert(!checkpoint_present("vm-complete-completed"));
  assert(stage3_start(1,1) == 1);
  assert(boot_prop_reads==1 && boot_waits==1);
  boot_prop_unreadable=0; /* EINTR only after first successful cycle */
  assert(sleeps == 2 && swap_calls == 1 && helper_ops[NZM_HELPER_CREATE] == 1 &&
         request_calls == 1);
  assert(readnum("/proc/sys/vm/min_free_kbytes") == 200);
  assert(checkpoint_present("vm-complete-completed"));
  path(p, "/sys/block/zram0/writeback");
  int f = open(p, O_RDONLY);
  assert(f >= 0);
  char b[128];
  ssize_t n = read(f, b, sizeof b - 1);
  assert(n > 0);
  b[n] = 0;
  close(f);
  assert(!strcmp(b, "idle 1024 1\n"));
  path(p, "/data/vendor/extm/state/ledger");
  f = open(p, O_RDONLY);
  assert(f >= 0);
  n = read(f, b, sizeof b - 1);
  assert(n > 0);
  b[n] = 0;
  close(f);
  struct nzm_ledger l = {0};
  assert(!nzm_ledger_parse(b, (size_t)n, (uint64_t)time(NULL), &l));
  assert(l.generation == 1 && l.cumulative == 4096 && l.count == 1);
  nzm_ledger_free(&l);
  before_queries = helper_ops[NZM_HELPER_QUERY];
  before_writes = writes;
  file("/data/vendor/extm/state/ledger.txn", "nzm-ledger-txn-v1\n");
  assert(nzm_entry(2, status) == 0);
  assert(helper_ops[NZM_HELPER_QUERY] == before_queries &&
         writes == before_writes);
  assert(stage3_start(1,0) == 1);
  assert(request_calls == 1 && helper_ops[NZM_HELPER_QUERY] == before_queries);
  /* Operator pause remains available with missing or malformed config. */
  remove_file("/data/vendor/extm/state/pause");
  file("/vendor/etc/lge-zram.conf", "not=config\n");
  char *pause_cmd[] = {"native-zram-manager", "pause", "--confirmed", NULL};
  assert(nzm_entry(3, pause_cmd) == 0);
  remove_file("/data/vendor/extm/state/pause");
  remove_file("/vendor/etc/lge-zram.conf");
  assert(nzm_entry(3, pause_cmd) == 0);
  /* Disabled writeback still completes second VM verification, with no request.
   */
  success_fixture();
  file("/vendor/etc/lge-zram.conf", "enable_writeback=0\n");
  unsigned old_requests = request_calls;
  assert(stage3_start(1,0) == 0);
  assert(request_calls == old_requests);
  assert(checkpoint_present("vm-complete-completed"));
  /* DM scan read failure must not be mistaken for EOF / no existing mapping. */
  success_fixture();
  fail_readdir = 1;
  before_queries = helper_ops[NZM_HELPER_QUERY];
  assert(stage3_start(0,0) == 1);
  fail_readdir = 0;
  assert(helper_ops[NZM_HELPER_QUERY] == before_queries);
  assert(strstr(stage3_last_event,"startup_lock") && strstr(stage3_last_event,"errno=5"));
  /* Startup never creates the tree or authority, even if legacy state exists. */
  for(int missing=0;missing<3;missing++) {
    success_fixture();
    unsigned prepares=helper_ops[NZM_HELPER_PREPARE];
    dir("/data/extm",0700);
    dir("/data/extm/state",0700);
    file("/data/extm/state/authority","extm-wear-v1\n");
    char legacy[1024]; path(legacy,"/data/extm/state/authority");
    struct stat saved, current;
    assert(!stat(legacy,&saved));
    remove_file("/data/vendor/extm/state/authority");
    if(missing) {
      remove_file("/data/vendor/extm/state/ledger");
      path(p,"/data/vendor/extm/state");assert(!rmdir(p));
    }
    if(missing==2) {
      remove_file("/data/vendor/extm/owner");
      path(p,"/data/vendor/extm");assert(!rmdir(p));
    }
    assert(stage3_start(0,0)==1);
    assert(!stat(legacy,&current) && current.st_ino==saved.st_ino && current.st_size==saved.st_size);
    assert(helper_ops[NZM_HELPER_PREPARE]==prepares);
    path(p,"/data/vendor/extm/state/authority");assert(access(p,F_OK)<0 && errno==ENOENT);
  }
  char *native_boot[]={"native-zram-manager","boot","--confirmed",NULL};
  /* Fully valid existing owner+file must retain inode and skip PREPARE. */
  success_fixture();path(p,"/data/vendor/extm/file");
  f=open(p,O_RDWR|O_CREAT|O_EXCL,0600);assert(f>=0);
  assert(!ftruncate(f,512*1048576LL));struct stat existing,after;
  assert(!fstat(f,&existing));close(f);
  unsigned old_prepare=helper_ops[NZM_HELPER_PREPARE];
  unsigned old_create=helper_ops[NZM_HELPER_CREATE];
  unsigned old_query=helper_ops[NZM_HELPER_QUERY];
  unsigned old_bind=helper_ops[NZM_HELPER_BIND];
  assert(!nzm_entry(3,native_boot));
  assert(helper_ops[NZM_HELPER_PREPARE]==old_prepare);
  assert(helper_ops[NZM_HELPER_CREATE]==old_create+1);
  assert(helper_ops[NZM_HELPER_QUERY]>old_query);
  assert(helper_ops[NZM_HELPER_BIND]==old_bind+1);
  path(p,"/data/vendor/extm/file");assert(!stat(p,&after));
  assert(existing.st_dev==after.st_dev && existing.st_ino==after.st_ino && existing.st_size==after.st_size);
  /* A new bootstrapped root has just state: create owner and prepare once. */
  success_fixture(); remove_file("/data/vendor/extm/owner");
  old_prepare=helper_ops[NZM_HELPER_PREPARE];
  assert(!nzm_entry(3,native_boot) && helper_ops[NZM_HELPER_PREPARE]==old_prepare+1);
  path(p,"/data/vendor/extm/owner"); assert(!stat(p,&after) && after.st_size==sizeof "extm-dm-owner-v1\n"-1);
  /* Existing partial, orphaned, or unlabelled file is never an absent file. */
  for(int invalid=0;invalid<3;invalid++) {
    success_fixture(); path(p,"/data/vendor/extm/file");
    f=open(p,O_RDWR|O_CREAT|O_EXCL,0600); assert(f>=0);
    assert(!ftruncate(f,invalid==0?4096:512*1048576LL));
    assert(!fstat(f,&existing) && !close(f));
    if(invalid==1) remove_file("/data/vendor/extm/owner");
    missing_backing_label=invalid==2;
    old_prepare=helper_ops[NZM_HELPER_PREPARE]; old_create=helper_ops[NZM_HELPER_CREATE];
    unsigned old_swaps=swap_calls;
    assert(nzm_entry(3,native_boot)==1);
    missing_backing_label=0;
    assert(helper_ops[NZM_HELPER_PREPARE]==old_prepare && helper_ops[NZM_HELPER_CREATE]==old_create);
    assert(!stat(p,&after) && after.st_ino==existing.st_ino && after.st_size==existing.st_size);
    assert(swap_calls==old_swaps);
  }
  success_fixture();
  file("/data/vendor/extm/owner", "foreign\n");
  data_sleeps = 0;
  assert(stage3_start(0,0) == 1 && data_sleeps == 0);
  /* Failed second owner cannot change startup evidence or owner's local event. */
  success_fixture();
  struct nzm_core owner;
  nzm_core_init(&owner); assert(!nzm_core_boot(&owner));
  char snapshot[1200]; snprintf(snapshot,sizeof snapshot,"%s",owner.last_event);
  unsigned checkpoint_bits=owner.boot_state.present;
  int checkpoint_writes=writes;
  assert(stage3_start(0,0)==1);
  assert(writes==checkpoint_writes && owner.boot_state.present==checkpoint_bits);
  assert(!strcmp(snapshot,owner.last_event));
  nzm_core_close(&owner);
  /* Writeback EIO remains the final local cause, with precharge and attempt. */
  success_fixture(); sleeps=0; fail_wb=1;
  assert(stage3_start(1,0)==1); fail_wb=0;
  assert(strstr(stage3_last_event,"stage=attempt_uncertain_no_refund errno=5"));
  assert(strstr(stage3_last_event,"reserved=1 attempted=1"));
  /* Logging is process-local: obsolete snapshot I/O failures cannot affect
   * pause, overwrite the error cause, or create a runtime filesystem. */
  for(int failure=0;failure<2;failure++) {
    success_fixture(); nzm_core_init(&owner); assert(!nzm_core_boot(&owner));
    fail_snapshot_short=!failure; fail_snapshot_sync=failure;
    owner.reserved=owner.attempted=1;
    assert(!nzm_core_pause(&owner,"snapshot_failure_test"));
    assert(!owner.snapshot_errno);
    assert(strstr(owner.last_event,"snapshot_failure_test") &&
           strstr(owner.last_event,"reserved=1 attempted=1"));
    path(p,"/dev/lge-zram-manager/last-event");
    assert(access(p,F_OK)<0 && errno==ENOENT);
    fail_snapshot_short=fail_snapshot_sync=0; nzm_core_close(&owner);
  }
  /* Explicit legacy-v1 cycle policy regressions: real core/planner/ledger/IO. */
  watermark_probe=1;
  const char *powers[]={"Charging\n","Full\n","Discharging\n","Not charging\n"};
  for(int screen=0;screen<2;screen++)for(int power=0;power<4;power++) {
    success_fixture();nzm_core_init(&owner);assert(!nzm_core_boot(&owner));
    owner.boot_completed=1;bd_effect=1;
    file("/sys/class/backlight/panel0-backlight/bl_power",screen?"4\n":"0\n");
    file("/sys/class/power_supply/battery/status",powers[power]);
    expected_charge=power<2?8192:2048;observed_idle=0;
    unsigned rq=request_calls;
    assert(!nzm_core_cycle(&owner) && request_calls==rq+1);
    assert(observed_idle==(power<2?30U:60U));
    assert(owner.last_attempt_valid && owner.last_attempt_tick==(uint64_t)monotonic_tick);
    assert(readnum("/sys/block/zram0/writeback_limit")==expected_charge/4);
    assert(ledger_generation()==1);
    nzm_core_close(&owner);
  }
  puts("PASS historical legacy-v1 screen 0/4 x Charging/Full/Discharging/Not charging: batch/idle/ledger/limit");
  for(int bad=0;bad<4;bad++) {
    success_fixture();nzm_core_init(&owner);assert(!nzm_core_boot(&owner));owner.boot_completed=1;
    if(bad==0)file("/sys/class/backlight/panel0-backlight/bl_power","1\n");
    if(bad==1)file("/sys/class/power_supply/battery/status","Unknown\n");
    if(bad==2)remove_file("/sys/class/backlight/panel0-backlight/bl_power");
    if(bad==3)remove_file("/sys/class/power_supply/battery/status");
    unsigned rq=request_calls,hq=helper_ops[NZM_HELPER_QUERY];
    uint64_t gen=ledger_generation();
    assert((nzm_core_cycle(&owner)<0)==(bad>=2));
    assert(request_calls==rq && helper_ops[NZM_HELPER_QUERY]==hq && ledger_generation()==gen && !owner.reserved);
    nzm_core_close(&owner);
  }
  /* Low boot has no first operation, no helper/ledger arbitration or attempt. */
  success_fixture();nzm_core_init(&owner);assert(!nzm_core_boot(&owner));owner.boot_completed=1;
  file("/sys/block/zram0/mm_stat","16777216 0 0 0 0 0 0 0 0 4096\n");
  unsigned rq=request_calls,hq=helper_ops[NZM_HELPER_QUERY];
  monotonic_tick=1000;
  assert(!nzm_core_cycle(&owner) && request_calls==rq && !owner.last_attempt_valid);
  monotonic_tick=1299;assert(!nzm_core_cycle(&owner) && request_calls==rq);
  assert(helper_ops[NZM_HELPER_QUERY]==hq && ledger_generation()==0);
  monotonic_tick=1300;expected_charge=4096;bd_effect=1;
  assert(!nzm_core_cycle(&owner) && request_calls==rq+1 && !strcmp(owner.trigger,"time"));
  nzm_core_close(&owner);
  /* Time track remains screen-off AND charge at all four known powers. */
  for(int screen=0;screen<2;screen++)for(int power=0;power<4;power++) {
    success_fixture();nzm_core_init(&owner);assert(!nzm_core_boot(&owner));owner.boot_completed=1;
    file("/sys/block/zram0/mm_stat","16777216 0 0 0 0 0 0 0 0 4096\n");
    file("/sys/class/backlight/panel0-backlight/bl_power",screen?"4\n":"0\n");
    file("/sys/class/power_supply/battery/status",powers[power]);
    owner.timer_valid=1;owner.timer_since=1;monotonic_tick=1000;expected_charge=4096;rq=request_calls;
    assert(!nzm_core_cycle(&owner) && request_calls==rq+(screen && power<2));
    nzm_core_close(&owner);
  }
  /* More candidates after idle cannot enlarge the prepared one-page plan. */
  success_fixture();nzm_core_init(&owner);assert(!nzm_core_boot(&owner));owner.boot_completed=1;
  file("/sys/block/zram0/mm_stat","67108864 0 0 0 0 0 0 0 0 1\n");
  grow_during_wait=1;expected_charge=4;rq=request_calls;
  assert(!nzm_core_cycle(&owner) && request_calls==rq+1 && owner.last_plan.pages==1);
  grow_during_wait=0;nzm_core_close(&owner);
  /* Time preparation cannot switch into a newly active watermark this round. */
  success_fixture();nzm_core_init(&owner);assert(!nzm_core_boot(&owner));owner.boot_completed=1;
  file("/sys/block/zram0/mm_stat","16777216 0 0 0 0 0 0 0 0 4096\n");
  owner.timer_valid=1;owner.timer_since=1;monotonic_tick=1000;grow_during_wait=1;rq=request_calls;
  assert(!nzm_core_cycle(&owner) && request_calls==rq && !owner.reserved);
  grow_during_wait=0;nzm_core_close(&owner);
  /* Time eligibility cannot change within an observation, or upgrade tracks. */
  for(int change=0;change<3;change++) {
    success_fixture();nzm_core_init(&owner);assert(!nzm_core_boot(&owner));owner.boot_completed=1;
    file("/sys/block/zram0/mm_stat","16777216 0 0 0 0 0 0 0 0 4096\n");
    owner.timer_valid=1;owner.timer_since=1;monotonic_tick=1000;
    switch_power=change==0?"Discharging\n":NULL;
    switch_screen=change==1?"0\n":NULL;
    if(change==2) {shrink_during_wait=1;expected_charge=4;}
    rq=request_calls;
    assert(!nzm_core_cycle(&owner));
    if(change<2)assert(request_calls==rq && !owner.reserved);
    switch_power=switch_screen=NULL;shrink_during_wait=0;nzm_core_close(&owner);
  }
  /* Shared actual-attempt clock, including cross-power 29/30 and 119/120. */
  for(int battery=0;battery<2;battery++) {
    success_fixture();nzm_core_init(&owner);assert(!nzm_core_boot(&owner));owner.boot_completed=1;
    bd_effect=1;expected_charge=battery?2048:8192;
    file("/sys/class/power_supply/battery/status",battery?"Discharging\n":"Charging\n");
    owner.last_attempt_valid=1;owner.last_attempt_tick=1000;
    owner.timer_valid=1;owner.timer_since=1;rq=request_calls;
    monotonic_tick=1000+(battery?119:29);
    assert(!nzm_core_cycle(&owner) && request_calls==rq && !strcmp(owner.trigger,"watermark"));
    monotonic_tick++;assert(!nzm_core_cycle(&owner) && request_calls==rq+1);
    assert(owner.timer_since==(uint64_t)monotonic_tick);
    file("/sys/class/power_supply/battery/status",battery?"Charging\n":"Discharging\n");
    expected_charge=battery?8192:2048;
    uint64_t last=owner.last_attempt_tick;
    monotonic_tick=(time_t)(last+(battery?29:119));
    assert(!nzm_core_cycle(&owner) && request_calls==rq+1);
    monotonic_tick++;assert(!nzm_core_cycle(&owner) && request_calls==rq+2);
    nzm_core_close(&owner);
  }
  /* Observation power switches: no reservation until stricter idle and
   * interval are actually satisfied; reverse switch retains smaller batch. */
  for(int change=0;change<3;change++) {
    success_fixture();nzm_core_init(&owner);assert(!nzm_core_boot(&owner));owner.boot_completed=1;
    file("/sys/class/power_supply/battery/status",change==2?"Discharging\n":"Charging\n");
    switch_power=change==2?"Charging\n":"Discharging\n";
    if(change==1) {
      owner.config.watermark_charging_idle_observation_seconds=60;
      owner.last_attempt_valid=1;owner.last_attempt_tick=1000;monotonic_tick=1030;
    }
    expected_charge=2048;rq=request_calls;observed_idle=0;
    assert(!nzm_core_cycle(&owner));
    if(change<2)assert(request_calls==rq && !owner.reserved && ledger_generation()==0);
    else assert(request_calls==rq+1 && observed_idle==60);
    switch_power=NULL;
    if(change<2) {
      monotonic_tick=1200;
      assert(!nzm_core_cycle(&owner) && request_calls==rq+1 && observed_idle>=90);
    }
    nzm_core_close(&owner);
  }
  puts("PASS historical legacy-v1 actual-attempt clocks 29/30,119/120, cross-power, conservative switches, time eligibility loss, no time relabel");
  /* Original 8MiB blocked plan cannot be diluted by a new 2MiB policy,
   * power changes, low crossing, or elapsed-only retry. */
  success_fixture();nzm_core_init(&owner);assert(!nzm_core_boot(&owner));owner.boot_completed=1;
  bd_effect=0;expected_charge=8192;monotonic_tick=20000;rq=request_calls;
  assert(!nzm_core_cycle(&owner) && request_calls==rq+1 && owner.no_progress);
  uint64_t blocked=owner.blocked_plan.pages;assert(blocked==2048);
  file("/sys/class/power_supply/battery/status","Discharging\n");
  file("/sys/block/zram0/mm_stat","67108864 0 0 0 0 0 0 0 0 4608\n");
  monotonic_tick=20600;
  for(int j=0;j<3;j++)assert(!nzm_core_cycle(&owner) && request_calls==rq+1 && owner.blocked_plan.pages==blocked);
  file("/sys/block/zram0/mm_stat","16777216 0 0 0 0 0 0 0 0 4096\n");
  file("/sys/class/power_supply/battery/status","Charging\n");
  assert(!nzm_core_cycle(&owner) && request_calls==rq+1 && owner.no_progress);
  file("/sys/block/zram0/mm_stat","67108864 0 0 0 0 0 0 0 0 6144\n");
  file("/sys/class/power_supply/battery/status","Discharging\n");expected_charge=2048;
  assert(!nzm_core_cycle(&owner) && request_calls==rq+2 && ledger_generation()==2);
  nzm_core_close(&owner);
  /* Plan/reservation/kernel 4KiB limit remain identical at 4/16/64KiB pages. */
  for(uint64_t pg=4096;pg<=65536;pg*=4) {
    success_fixture();nzm_core_init(&owner);assert(!nzm_core_boot(&owner));owner.boot_completed=1;
    cycle_page=pg;bd_effect=1;expected_charge=pg/1024;rq=request_calls;
    char line[160];snprintf(line,sizeof line,"%llu 0 0 0 0 0 0 0 0 1\n",(unsigned long long)(pg*2));
    file("/sys/block/zram0/mm_stat",line);
    snprintf(line,sizeof line,"%llu\n",(unsigned long long)(pg*4));file("/sys/block/zram0/disksize",line);
    owner.config.watermark_high_pct=20;owner.config.watermark_low_pct=0;
    assert(!nzm_core_cycle(&owner) && request_calls==rq+1);
    assert(owner.last_plan.pages==1 && owner.last_plan.reserve_kib==pg/1024 && readnum("/sys/block/zram0/writeback_limit")==pg/4096);
    nzm_core_close(&owner);cycle_page=4096;
  }
  /* Ownership uncertainty at post-idle/prewrite/postwrite is terminal. */
  for(int point=0;point<3;point++) {
    success_fixture();nzm_core_init(&owner);assert(!nzm_core_boot(&owner));owner.boot_completed=1;
    query_counter=0;query_fail_at=point==0?0:point==1?4:5;
    identity_on_wait=point==0;expected_charge=8192;rq=request_calls;
    assert(nzm_core_cycle(&owner)<0 && request_calls==rq+(point==2));
    assert(!strcmp(owner.stage,point==0?"post_idle_boundary":point==1?"pre_attempt_boundary":"post_attempt_identity"));
    query_fail_at=0;identity_on_wait=0;nzm_core_close(&owner);
  }
  /* Ledger IO failure before reserve cannot produce a request. */
  success_fixture();nzm_core_init(&owner);assert(!nzm_core_boot(&owner));owner.boot_completed=1;
  remove_file("/data/vendor/extm/state/ledger");rq=request_calls;
  assert(nzm_core_cycle(&owner)<0 && request_calls==rq && !owner.reserved);
  nzm_core_close(&owner);watermark_probe=0;
  puts("PASS historical legacy-v1 shared NO_PROGRESS original evidence, low/power/shrink resistance, 4/16/64K plan/ledger/limit, identity and ledger IO faults");
  /* Frozen historical profiles explicitly constructed after the live loader.
   * Retired policy keys in files must never reactivate production scheduling. */
  watermark_probe=1;advance_idle_clock=1;bd_effect=1;
  for(int alternate=0;alternate<2;alternate++) {
    success_fixture();
    file("/vendor/etc/lge-zram.conf","backing_mib=1024\n");
    nzm_core_init(&owner);assert(!nzm_core_config(&owner));helper_backing_mib=1024;
    owner.config.batch_mib=alternate?12:16;
    owner.config.idle_observation_seconds=alternate?7:60;
    owner.config.time_trigger_interval_seconds=alternate?41:300;
    owner.config.watermark_charging_batch_mib=alternate?6:8;
    owner.config.watermark_charging_idle_observation_seconds=alternate?9:30;
    owner.config.watermark_charging_min_interval_seconds=alternate?17:30;
    owner.config.watermark_battery_batch_mib=alternate?3:2;
    owner.config.watermark_battery_idle_observation_seconds=alternate?11:60;
    owner.config.watermark_battery_min_interval_seconds=alternate?23:120;
    assert(owner.config.backing_mib==1024);
    assert(owner.config.batch_mib==(alternate?12U:16U));
    assert(owner.config.idle_observation_seconds==(alternate?7U:60U));
    assert(owner.config.time_trigger_interval_seconds==(alternate?41U:300U));
    assert(owner.config.watermark_charging_batch_mib==(alternate?6U:8U));
    assert(owner.config.watermark_charging_idle_observation_seconds==(alternate?9U:30U));
    assert(owner.config.watermark_charging_min_interval_seconds==(alternate?17U:30U));
    assert(owner.config.watermark_battery_batch_mib==(alternate?3U:2U));
    assert(owner.config.watermark_battery_idle_observation_seconds==(alternate?11U:60U));
    assert(owner.config.watermark_battery_min_interval_seconds==(alternate?23U:120U));
    assert(!nzm_core_boot(&owner));owner.boot_completed=1;
    for(int track=0;track<3;track++) {
      file("/sys/block/zram0/bd_stat","0 0 0\n");
      file("/sys/block/zram0/mm_stat",track?"67108864 0 0 0 0 0 0 0 0 8192\n":"16777216 0 0 0 0 0 0 0 0 4096\n");
      file("/sys/class/power_supply/battery/status",track==2?"Discharging\n":"Charging\n");
      uint64_t interval=track?(track==1?owner.config.watermark_charging_min_interval_seconds:owner.config.watermark_battery_min_interval_seconds):owner.config.time_trigger_interval_seconds;
      uint64_t idle=track?(track==1?owner.config.watermark_charging_idle_observation_seconds:owner.config.watermark_battery_idle_observation_seconds):owner.config.idle_observation_seconds;
      expected_charge=1024*(track?(track==1?owner.config.watermark_charging_batch_mib:owner.config.watermark_battery_batch_mib):owner.config.batch_mib);
      owner.timer_valid=owner.last_attempt_valid=1;owner.timer_since=owner.last_attempt_tick=10000;
      monotonic_tick=(time_t)(10000+interval-1);rq=request_calls;
      assert(!nzm_core_cycle(&owner) && request_calls==rq);
      monotonic_tick++;observed_idle=0;uint64_t before_idle=(uint64_t)monotonic_tick;
      assert(!nzm_core_cycle(&owner) && request_calls==rq+1 && observed_idle==idle);
      assert(captured_submit_tick==before_idle+idle && captured_submit_tick>before_idle);
      assert(owner.last_attempt_tick==captured_submit_tick && owner.timer_since==captured_submit_tick);
    }
    nzm_core_close(&owner);helper_backing_mib=512;
  }
  puts("PASS historical module profiles explicitly constructed: time and all six watermark fields drive module cycles only");
  /* P2-2: syscall open failure is not an attempt; write failure is. */
  for(int write_failure=0;write_failure<2;write_failure++) {
    success_fixture();nzm_core_init(&owner);assert(!nzm_core_boot(&owner));owner.boot_completed=1;
    owner.timer_valid=owner.last_attempt_valid=1;owner.timer_since=owner.last_attempt_tick=100;
    monotonic_tick=1000;captured_submit_tick=0;rq=request_calls;expected_charge=8192;
    fail_attempt_open=!write_failure;fail_wb=write_failure;
    assert(nzm_core_cycle(&owner)<0);
    assert(request_calls==rq+write_failure);
    if(write_failure)assert(captured_submit_tick==1030 && owner.last_attempt_tick==1030 && owner.timer_since==1030);
    else assert(!captured_submit_tick && owner.last_attempt_tick==100 && owner.timer_since==100 && !owner.attempted);
    fail_attempt_open=fail_wb=0;nzm_core_close(&owner);
  }
  puts("PASS P2-2 advancing idle clock, captured actual write tick, open failure unchanged, failed write attempt updates clocks");
  /* P2-3: time-origin NO_PROGRESS, low/high/track switching and original
   * blocked-plan evidence must all retain the full 600-second retry gate. */
  success_fixture();nzm_core_init(&owner);assert(!nzm_core_boot(&owner));owner.boot_completed=1;
  file("/sys/block/zram0/mm_stat","16777216 0 0 0 0 0 0 0 0 2048\n");
  owner.timer_valid=1;owner.timer_since=1;monotonic_tick=1000;bd_effect=0;expected_charge=4096;rq=request_calls;
  assert(!nzm_core_cycle(&owner) && request_calls==rq+1 && owner.no_progress && !strcmp(owner.trigger,"time"));
  struct nzm_plan original=owner.blocked_plan;uint64_t blocked_tick=owner.no_progress_since;
  assert(original.pages==1024 && blocked_tick==captured_submit_tick);
  monotonic_tick=(time_t)(blocked_tick+600);
  assert(!nzm_core_cycle(&owner) && request_calls==rq+1 && owner.no_progress && owner.watermark_resume==NZM_ARMED);
  file("/sys/block/zram0/disksize","16777216\n"); /* cross high without new stock/candidates */
  file("/sys/class/power_supply/battery/status","Discharging\n");
  assert(!nzm_core_cycle(&owner) && request_calls==rq+1 && !strcmp(owner.trigger,"watermark"));
  assert(!memcmp(&original,&owner.blocked_plan,sizeof original));
  file("/sys/block/zram0/mm_stat","16777216 0 0 0 0 0 0 0 0 2560\n"); /* 2MiB is not original 4MiB */
  assert(!nzm_core_cycle(&owner) && request_calls==rq+1);
  file("/sys/block/zram0/mm_stat","16777216 0 0 0 0 0 0 0 0 3072\n");
  monotonic_tick=(time_t)(blocked_tick+599);
  assert(!nzm_core_cycle(&owner) && request_calls==rq+1 && !memcmp(&original,&owner.blocked_plan,sizeof original));
  monotonic_tick++;expected_charge=2048;
  assert(!nzm_core_cycle(&owner) && request_calls==rq+2 && ledger_generation()==2);
  nzm_core_close(&owner);
  /* A successful watermark write drops to low; time resets from actual submit. */
  success_fixture();nzm_core_init(&owner);assert(!nzm_core_boot(&owner));owner.boot_completed=1;
  bd_effect=1;expected_charge=8192;monotonic_tick=1000;rq=request_calls;
  assert(!nzm_core_cycle(&owner) && request_calls==rq+1 && !strcmp(owner.trigger,"watermark"));
  uint64_t wm_submit=captured_submit_tick;
  file("/sys/block/zram0/mm_stat","16777216 0 0 0 0 0 0 0 0 2048\n");
  monotonic_tick=(time_t)(wm_submit+299);assert(!nzm_core_cycle(&owner) && request_calls==rq+1);
  monotonic_tick++;expected_charge=4096;
  assert(!nzm_core_cycle(&owner) && request_calls==rq+2 && !strcmp(owner.trigger,"time"));
  assert(captured_submit_tick==wm_submit+300+owner.config.idle_observation_seconds);
  nzm_core_close(&owner);
  /* Overflow/rollback never turn either clock into immediate eligibility. */
  for(int attempt_clock=0;attempt_clock<2;attempt_clock++)for(int rollback=0;rollback<2;rollback++) {
    success_fixture();nzm_core_init(&owner);assert(!nzm_core_boot(&owner));owner.boot_completed=1;
    if(attempt_clock) {
      owner.last_attempt_valid=1;owner.last_attempt_tick=2000;
      if(!rollback)owner.config.watermark_charging_min_interval_seconds=UINT64_MAX;
    } else {
      file("/sys/block/zram0/mm_stat","16777216 0 0 0 0 0 0 0 0 2048\n");
      owner.timer_valid=1;owner.timer_since=2000;
      if(!rollback)owner.config.time_trigger_interval_seconds=UINT64_MAX;
    }
    monotonic_tick=rollback?1999:3000;rq=request_calls;
    assert(!nzm_core_cycle(&owner) && request_calls==rq && !owner.reserved && ledger_generation()==0);
    nzm_core_close(&owner);
  }
  /* Watermark active but empty: due time must not take over arbitration. */
  success_fixture();nzm_core_init(&owner);assert(!nzm_core_boot(&owner));owner.boot_completed=1;
  file("/sys/block/zram0/mm_stat","67108864 0 0 0 0 0 0 0 0 0\n");
  owner.timer_valid=1;owner.timer_since=1;monotonic_tick=1000;rq=request_calls;
  assert(!nzm_core_cycle(&owner) && request_calls==rq && !strcmp(owner.trigger,"watermark") && owner.watermark==NZM_DRAINING && !owner.reserved);
  nzm_core_close(&owner);advance_idle_clock=watermark_probe=0;
  puts("PASS P2-3 time-origin original blocked evidence across low/high/watermark and 599/600, watermark-to-time 299/300, UINT64_MAX/rollback clocks, active empty watermark no time relabel");
  execution_tests();
  return 0;
}
