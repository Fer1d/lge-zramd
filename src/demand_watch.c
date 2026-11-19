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

static uint64_t deadline_add(uint64_t now,uint64_t delta) {
  return now>UINT64_MAX-delta ? UINT64_MAX : now+delta;
}
struct nzm_demand_decision nzm_demand_decide(struct nzm_demand_schedule *s,
    const struct nzm_demand_view *v,uint64_t now) {
  struct nzm_demand_decision d={NZM_DEMAND_WAIT,0};
  if(!s || !v) {d.action=NZM_DEMAND_FATAL;return d;}
  if(s->fatal) {d.action=NZM_DEMAND_FATAL;return d;}
  if(s->sequence!=v->sequence) {
    s->sequence=v->sequence;s->successes=0;
    /* Age refreshes advance sequence, but never bypass existing deadlines. */
  }
  if(!v->active || !v->enabled || !v->initialized || !v->backing)return d;
  uint64_t wait=s->retry_ns>s->budget_ns ? s->retry_ns : s->budget_ns;
  if(wait==UINT64_MAX || wait>now) {
    if(!d.deadline_ns || wait<d.deadline_ns)d.deadline_ns=wait;
    return d;
  }
  if(s->successes>=4)return d;
  d.action=NZM_DEMAND_EXECUTE;return d;
}
void nzm_demand_complete(struct nzm_demand_schedule *s,uint64_t now,
    uint64_t age,int rc,int halted,int known,uint64_t installed,uint64_t budget) {
  if(!s)return;
  if(halted || (rc && rc!=-EAGAIN && rc!=-ESTALE && rc!=-EDQUOT)) {s->fatal=1;return;}
  s->budget_ns=0;
  if(rc==-EDQUOT) {
    s->retry_ns=0;
    /* Expiry can pass during EXEC/closure. Reevaluate the budget on the next
     * bounded read, not a fatal latch or an immediate EXEC busy loop. */
    if(!budget || budget<=now) s->retry_ns=deadline_add(now,UINT64_C(1000000000));
    else s->budget_ns=budget;
    return;
  }
  if(!rc && known && installed) {
    s->successes++;s->zero_progress=0;s->retry_ns=0;return;
  }
  uint64_t delay=age>60 ? age : 60;
  if(delay>1800)delay=1800;
  for(unsigned i=0;i<s->zero_progress && delay<1800;i++)delay=delay>900 ? 1800 : delay*2;
  if(s->zero_progress<32)s->zero_progress++;
  s->retry_ns=deadline_add(now,delay*UINT64_C(1000000000));
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
static int watch_now(uint64_t *now) {return nzm_memory_now(now);}
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
  struct nzm_demand_schedule schedule={0};
  struct nzm_demand_exec_state exec_state={0};
  while(!rc && !demand_stop) {
    struct nzm_demand_view v;
    rc=nzm_demand_read(fd,&v);if(rc)break;
    if(v.low_mib!=c->config.pool_low_mib || v.high_mib!=c->config.pool_high_mib ||
       v.min_age_seconds!=c->config.cold_age_seconds || !v.enabled || !v.initialized || !v.backing){rc=-ESTALE;break;}
    if((rc=nzm_pause_verified(c,&paused)) || paused){if(!rc)rc=-ECANCELED;break;}
    uint64_t now;
    rc=watch_now(&now);if(rc)break;
    struct nzm_demand_decision decision=nzm_demand_decide(&schedule,&v,now);
    if(decision.action==NZM_DEMAND_FATAL) {rc=-EIO;break;}
    if(decision.action==NZM_DEMAND_EXECUTE) {
      exec_state.expected_sequence=v.sequence;
      int result=nzm_core_demand_exec(c,&exec_state,NULL,NULL);
      rc=watch_now(&now);
      if(rc)break;
      nzm_demand_complete(&schedule,now,v.min_age_seconds,result,exec_state.halted,
          exec_state.known,exec_state.reply.exec.installed_pages,exec_state.budget_deadline_ns);
      if(schedule.fatal) {rc=result ? result : -EIO;break;}
      continue;
    }
    int timeout=1000; /* Bounded read resync, not authorization or persistence. */
    if(decision.deadline_ns && decision.deadline_ns>now) {
      uint64_t delta=decision.deadline_ns-now;
      uint64_t ms=delta/1000000+(delta%1000000!=0);
      if(ms<(uint64_t)timeout)timeout=(int)ms;
    }
    struct pollfd pollfd={.fd=fd,.events=POLLPRI|POLLERR};
    int ready=poll(&pollfd,1,timeout);
    if(ready<0){if(errno==EINTR)continue;rc=-errno;break;}
    if(pollfd.revents&(POLLHUP|POLLNVAL)){rc=-ENODEV;break;}
  }
  int zero=nzm_putnum(Z "writeback_limit",0);
  int disable=nzm_put(Z "pool_policy","0 512 768 60\n");
  close(fd);return rc?rc:zero?zero:disable;
}
#endif
