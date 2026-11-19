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

int nzm_space(struct nzm_core *c) {
  struct statvfs s;
  if (fstatvfs(c->datafd, &s))
    return -errno;
  uint64_t need = c->config.min_data_free_mib * MIB;
  if (!s.f_frsize || s.f_bavail < need / s.f_frsize + (need % s.f_frsize != 0))
    return -ENOSPC;
  return 0;
}
int nzm_health(struct nzm_core *c) {
  char b[8192];
  int rc = nzm_rawread("/proc/meminfo", b, sizeof b);
  if (rc)
    return rc;
  char *line = b;
  uint64_t mem = 0;
  int found = 0;
  while (*line) {
    char *end = strchr(line, '\n');
    if (!end)
      return -EINVAL;
    *end = 0;
    if (!strncmp(line, "MemAvailable:", 13)) {
      char unit[8], extra;
      if (found++ ||
          sscanf(line + 13, "%" SCNu64 " %7s %c", &mem, unit, &extra) != 2 ||
          strcmp(unit, "kB"))
        return -EINVAL;
    }
    line = end + 1;
  }
  if (!found)
    return -EINVAL;
  if (mem < c->config.min_mem_available_kib)
    return -EAGAIN;
  if ((rc = nzm_space(c))) return rc;
  return 0;
}
/* Only the documented screen and power enum values are authority. */
int nzm_power_policy(int *off, int *charging) {
  char b[128];
  int rc=nzm_rawread("/sys/class/backlight/panel0-backlight/bl_power",b,sizeof b);
  if(rc)return rc;
  if(!strcmp(b,"4\n"))*off=1;
  else if(!strcmp(b,"0\n"))*off=0;
  else return -EAGAIN;
  rc=nzm_rawread("/sys/class/power_supply/battery/status",b,sizeof b);
  if(rc)return rc;
  if(!strcmp(b,"Charging\n") || !strcmp(b,"Full\n"))*charging=1;
  else if(!strcmp(b,"Discharging\n") || !strcmp(b,"Not charging\n"))*charging=0;
  else return -EAGAIN;
  return 0;
}
