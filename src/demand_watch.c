#define _GNU_SOURCE
#include "nzm_internal.h"
#include "demand_watch.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

int nzm_demand_parse(const char *text, struct nzm_demand_view *out) {
  if (!text || !out) return -EINVAL;
  struct nzm_demand_view v={0};
  int used=0;
  int n=sscanf(text,"sequence %"SCNu64" instance %"SCNu64" generation %"SCNu64
      " enabled %"SCNu64" initialized %"SCNu64" backing %"SCNu64" active %"SCNu64
      " physical_pool_bytes %"SCNu64" low_mib %"SCNu64" high_mib %"SCNu64
      " min_age_seconds %"SCNu64"\n%n", &v.sequence,&v.instance,&v.generation,
      &v.enabled,&v.initialized,&v.backing,&v.active,&v.physical_pool_bytes,
      &v.low_mib,&v.high_mib,&v.min_age_seconds,&used);
  if(n!=11 || !used || text[used] || !v.instance || !v.generation || v.enabled>1 ||
     v.initialized>1 || v.backing>1 || v.active>1 || !v.low_mib ||
     v.high_mib<=v.low_mib || !v.min_age_seconds ||
     (v.active && (!v.enabled || !v.initialized || !v.backing))) return -EPROTO;
  char canonical[768];
  int length=snprintf(canonical,sizeof canonical,"sequence %"PRIu64" instance %"PRIu64" generation %"PRIu64
      " enabled %"PRIu64" initialized %"PRIu64" backing %"PRIu64" active %"PRIu64
      " physical_pool_bytes %"PRIu64" low_mib %"PRIu64" high_mib %"PRIu64
      " min_age_seconds %"PRIu64"\n",v.sequence,v.instance,v.generation,v.enabled,
      v.initialized,v.backing,v.active,v.physical_pool_bytes,v.low_mib,v.high_mib,v.min_age_seconds);
  if(length<0 || (size_t)length>=sizeof canonical || strcmp(text,canonical))return -EPROTO;
  *out=v;return 0;
}
int nzm_demand_read(int fd, struct nzm_demand_view *out) {
  char text[768];size_t len=0;
  if(lseek(fd,0,SEEK_SET)<0)return -errno;
  int rc=nzm_io_read_fd(fd,text,sizeof text-1,&len);
  if(rc)return rc;
  if(!len || memchr(text,0,len))return -EPROTO;
  text[len]=0;return nzm_demand_parse(text,out);
}

/* The runtime adapter is deliberately not enabled in an unqualified policy
 * image. The real consumer is compiled by the candidate fixture below. */
#ifndef NZM_DEMAND_RUNTIME_QUALIFIED
int nzm_demand_watch(struct nzm_core *c) {
  (void)c;
  return -EOPNOTSUPP;
}
#else
#include "demand_exec.h"
static volatile sig_atomic_t demand_stop;
static void stop_signal(int sig) {(void)sig; demand_stop=1;}
int nzm_demand_watch(struct nzm_core *c) {
  if(!c->config.demand_enabled || !c->config.enable_writeback)return 0;
  int paused,rc=nzm_pause_verified(c,&paused);
  if(rc || paused)return rc;
  if((rc=nzm_managed(c)) || (rc=nzm_dm_quota()))return rc;
  int fd=open(Z "pool_demand",O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
  if(fd<0)return -errno;
  const struct nzm_meta meta={0,0,0400,1,"u:object_r:lge_zram_policy_sysfs:s0"};
  rc=nzm_io_check(fd,S_IFREG,&meta);
  char config[128];
  int n=snprintf(config,sizeof config,"1 %"PRIu64" %"PRIu64" %"PRIu64"\n",
      c->config.pool_low_mib,c->config.pool_high_mib,c->config.cold_age_seconds);
  if(!rc && (n<0 || (size_t)n>=sizeof config))rc=-ERANGE;
  demand_stop=0;
  struct sigaction action={0};action.sa_handler=stop_signal;
  if(!rc && (sigemptyset(&action.sa_mask)||sigaction(SIGTERM,&action,NULL)||sigaction(SIGINT,&action,NULL)))rc=-errno;
  if(!rc && !demand_stop)rc=nzm_put(Z "pool_policy",config);
  uint64_t seen=0;unsigned episode=0;int attempted=0;
  struct nzm_demand_exec_state exec_state={.next_request_id=1};
  while(!rc && !demand_stop) {
    struct nzm_demand_view v;
    rc=nzm_demand_read(fd,&v);if(rc)break;
    if(v.low_mib!=c->config.pool_low_mib || v.high_mib!=c->config.pool_high_mib ||
       v.min_age_seconds!=c->config.cold_age_seconds || !v.enabled || !v.initialized || !v.backing){rc=-ESTALE;break;}
    if((rc=nzm_pause_verified(c,&paused)) || paused){if(!rc)rc=-ECANCELED;break;}
    if(v.active && (!attempted || v.sequence!=seen)) {
      if(seen!=v.sequence)episode=0;
      seen=v.sequence;attempted=1;
      exec_state.expected_sequence=v.sequence;
      rc=nzm_core_demand_exec(c,&exec_state,NULL,NULL);
      if(!rc && exec_state.known && exec_state.reply.exec.installed_pages && ++episode<4){
        attempted=0; /* Bounded continuation while the same demand remains high. */
      }
      if(rc==-EAGAIN || rc==-ESTALE || rc==-EDQUOT)rc=0;
      if(rc || exec_state.halted)break;
      continue;
    }
    struct pollfd pollfd={.fd=fd,.events=POLLPRI|POLLERR};
    int ready=poll(&pollfd,1,30000); /* Read-only health resync, never a trigger. */
    if(ready<0){if(errno==EINTR)continue;rc=-errno;break;}
    if(pollfd.revents&(POLLHUP|POLLNVAL)){rc=-ENODEV;break;}
  }
  int zero=nzm_putnum(Z "writeback_limit",0);
  int disable=nzm_put(Z "pool_policy","0 512 768 60\n");
  close(fd);return rc?rc:zero?zero:disable;
}
#endif
