#define _GNU_SOURCE
#include "nzm_internal.h"
#include "wear_store.h"
#include "log.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sys/sysmacros.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int real_absent(void *p) {
  struct nzm_core *c=p;
  int rc=nzm_absent(c->wearfd,"exec.intent");
  if(!rc)rc=nzm_absent(c->wearfd,"ledger.new");
  if(!rc)rc=nzm_absent(c->wearfd,"ledger.txn");
  return rc;
}
static int real_sample(void *p,struct nzm_budget_sample *s) {
  return nzm_budget_sample_clock(((struct nzm_core *)p)->boot,NULL,NULL,s);
}
static int real_read(void *p,const char *name,char **out,size_t *len) {
  struct nzm_core *c=p;
  *out=NULL;*len=0;
  char *b=malloc(NZM_LEDGER_MAX_BYTES);
  if(!b)return -ENOMEM;
  const struct nzm_meta m={0,0,0600,1,LCTX};
  int rc=nzm_io_read_at(c->wearfd,name,&m,b,NZM_LEDGER_MAX_BYTES,len);
  if(rc)free(b);else *out=b;
  return rc;
}
static int real_backup(void *p,const char *text,size_t len) {
  struct nzm_core *c=p;
  int rc=nzm_core_authority(c);
  if(rc)return rc;
  /* Reserve both complete files plus the existing IO metadata reserve. */
  if(len>SIZE_MAX-NZM_WEAR_MAX_BYTES)return -EOVERFLOW;
  rc=nzm_io_ledger_preflight(c->wearfd,len+NZM_WEAR_MAX_BYTES);
  if(rc)return rc;
  char *old=NULL;size_t n=0;
  int existing=openat(c->wearfd,"ledger.v1.bak",O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
  if(existing>=0) {
    const struct nzm_meta m={0,0,0600,1,LCTX};
    rc=nzm_io_check(existing,S_IFREG,&m);
    old=malloc(NZM_LEDGER_MAX_BYTES);
    if(!old && !rc)rc=-ENOMEM;
    if(!rc)rc=nzm_io_read_fd(existing,old,NZM_LEDGER_MAX_BYTES,&n);
    if(!rc && (n!=len || memcmp(old,text,n)))rc=-EPROTO;
    free(old);
    if(!rc && fsync(existing))rc=-errno;
    if(close(existing) && !rc)rc=-errno;
    if(!rc && fsync(c->wearfd))rc=-errno;
    return rc;
  }
  if(errno!=ENOENT)return -errno;
  int fd=openat(c->wearfd,"ledger.v1.bak",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
  if(fd<0)return -errno;
  const struct nzm_meta m={0,0,0600,1,LCTX};
  rc=nzm_io_check(fd,S_IFREG,&m);
  size_t pos=0;
  while(!rc && pos<len) {
    ssize_t w=write(fd,text+pos,len-pos);
    if(w<0) {rc=-errno;break;}
    if(!w) {rc=-EIO;break;}
    pos+=(size_t)w;
  }
  if(!rc && fsync(fd))rc=-errno;
  if(close(fd) && !rc)rc=-errno;
  if(!rc && fsync(c->wearfd))rc=-errno;
  if(rc)return rc; /* Retain even partial evidence; never unlink/overwrite. */
  rc=real_read(p,"ledger.v1.bak",&old,&n);
  if(!rc && (n!=len || memcmp(old,text,n)))rc=-EPROTO;
  free(old);return rc;
}
static int real_commit(void *p,const char *text,size_t len) {
  struct nzm_core *c=p;
  int rc=nzm_core_authority(c);
  return rc ? rc : nzm_io_ledger_commit(c->wearfd,text,len,WCTX,LCTX);
}
static int real_owned(void *p,const struct nzm_budget *expected) {
  struct nzm_core *c=p;
  const struct nzm_budget_reservation *r=&expected->pending;
  if(!c->intent_owned || !r->request_id || strcmp(r->boot,c->boot) ||
      c->execution.backend!=RB_BACKEND_REQUEST || !c->reserved ||
      r->reserved_kib!=c->last_plan.reserve_kib)return -EPERM;
  char record[sizeof c->intent_record];
  int n=snprintf(record,sizeof record,
      "nzm-exec-intent-v1 boot=%s request=%" PRIu64 " instance=%" PRIu64
      " generation=%" PRIu64 " rdev=%u:%u pages=%" PRIu64 " planned_kib=%" PRIu64 "\n",
      r->boot,r->request_id,c->execution.identity.instance,c->execution.identity.generation,
      major(c->execution.transport.expected_rdev),minor(c->execution.transport.expected_rdev),
      c->last_plan.pages,r->reserved_kib);
  if(n<0 || (size_t)n>=sizeof record || strcmp(record,c->intent_record))return -ESTALE;
  int rc=nzm_intent_check(c);
  if(!rc)rc=nzm_absent(c->wearfd,"ledger.new");
  if(!rc)rc=nzm_absent(c->wearfd,"ledger.txn");
  return rc;
}
static const struct nzm_wear_store_ops real_ops={real_absent,real_sample,real_read,real_backup,real_commit,real_owned};

int nzm_wear_store_load_ops(const struct nzm_wear_store_ops *o,void *p,int readonly,
    struct nzm_budget *out,struct nzm_budget_sample *sample,int *migration_required) {
  if(migration_required)*migration_required=0;
  if(!o || !o->absent || !o->sample || !o->read || !out || !sample ||
      (!readonly && (!o->backup || !o->commit)))return -EINVAL;
  int rc=o->absent(p),migrated=0;
  struct nzm_budget b,next;struct nzm_budget_sample s;
  char *original=NULL,*text=NULL;size_t len=0,n=0;
  if(!rc)rc=o->sample(p,&s);
  if(!rc)rc=o->read(p,"ledger",&original,&len);
  if(!rc)rc=nzm_budget_parse(original,len,&s,&b,&migrated);
  if(!rc)rc=o->absent(p);
  if(!rc && !readonly && (migrated || strcmp(b.boot,s.boot))) {
    /* Format-only v2 conversion MUST retain the persisted clock and buckets.
     * A later ordinary load can reanchor a changed boot independently. */
    struct nzm_budget_sample anchor=s;
    if(migrated==NZM_BUDGET_MIGRATED_V2) {
      memcpy(anchor.boot,b.boot,sizeof anchor.boot);anchor.boottime_ns=b.boottime_ns;
    }
    rc=nzm_budget_checkpoint_text(&b,&anchor,&next,&text,&n);
    if(!rc && migrated==NZM_BUDGET_MIGRATED_V1)rc=o->backup(p,original,len);
    if(!rc)rc=o->absent(p);
    if(!rc)rc=o->commit(p,text,n);
    if(!rc) {
      b=next;
      nzm_log("wear_store event=%s logical_ns=%llu next_request_id=%llu",
          migrated ? "migrated" : "newboot_reanchor",
          (unsigned long long)b.logical_ns,(unsigned long long)b.next_request_id);
    }
  }
  free(original);free(text);
  if(!rc) {*out=b;*sample=s;if(migration_required)*migration_required=migrated;}
  return rc;
}
static int next_checkpoint(const struct nzm_budget *b,const struct nzm_budget_sample *s,uint64_t *deadline) {
  uint64_t used,available,expiry;
  int rc=nzm_budget_budget(b,s,UINT64_MAX,&used,&available,&expiry);
  if(rc)return rc;
  *deadline=0;
  if(!used)return 0;
  *deadline=b->boottime_ns>UINT64_MAX-NZM_WEAR_CHECKPOINT_NS ? UINT64_MAX :
      b->boottime_ns+NZM_WEAR_CHECKPOINT_NS;
  return 0;
}
int nzm_wear_store_checkpoint_ops(const struct nzm_wear_store_ops *o,void *p,uint64_t *deadline) {
  if(!deadline)return -EINVAL;
  *deadline=0;
  struct nzm_budget b,next;struct nzm_budget_sample s;
  int rc=nzm_wear_store_load_ops(o,p,0,&b,&s,NULL);
  if(rc)return rc;
  /* Persist the final interval even if it expires all buckets. Persisted count
   * (not merely projected used) keeps a final checkpoint eligible. */
  if(b.count && s.boottime_ns>=b.boottime_ns &&
      s.boottime_ns-b.boottime_ns>=NZM_WEAR_CHECKPOINT_NS) {
    char *text=NULL;size_t n=0;
    rc=nzm_budget_checkpoint_text(&b,&s,&next,&text,&n);
    if(!rc)rc=o->absent(p);
    if(!rc)rc=o->commit(p,text,n);
    free(text);
    if(rc)return rc;
    b=next;
  }
  return next_checkpoint(&b,&s,deadline);
}
static int same_reservation(const struct nzm_budget_reservation *a,
    const struct nzm_budget_reservation *b) {
  return !strcmp(a->boot,b->boot) && a->request_id==b->request_id &&
      a->generation==b->generation && a->reserved_kib==b->reserved_kib;
}
int nzm_wear_store_settle_ops(const struct nzm_wear_store_ops *o,void *p,
    const struct nzm_budget *expected,const struct nzm_budget_sample *sample,
    uint64_t submitted) {
  if(!o || !o->owned || !o->read || !o->commit || !expected || !sample ||
      !expected->pending.request_id || strcmp(expected->boot,sample->boot) ||
      strcmp(expected->pending.boot,sample->boot) ||
      sample->boottime_ns<expected->boottime_ns ||
      submitted>expected->pending.reserved_kib)return -EINVAL;
  int rc=o->owned(p,expected),migration=0;
  char *original=NULL,*snapshot=NULL,*text=NULL;size_t len=0,n=0,length=0;
  struct nzm_budget current,next,copy;
  if(!rc)rc=o->read(p,"ledger",&original,&len);
  if(!rc)rc=nzm_budget_parse(original,len,NULL,&current,&migration);
  if(!rc && migration)rc=-ESTALE;
  if(!rc && !current.pending.request_id) {
    /* A confirmed receipt is the ONLY idempotent case. Never rewrite it or
     * project time again, and never use it to settle a newer reservation. */
    if(!same_reservation(&current.receipt.reservation,&expected->pending) ||
        current.receipt.submitted_kib!=submitted ||
        current.generation!=expected->generation ||
        current.cumulative_kib!=expected->cumulative_kib ||
        current.next_request_id!=expected->next_request_id ||
        submitted>UINT64_MAX-expected->known_submitted_kib ||
        current.known_submitted_kib!=expected->known_submitted_kib+submitted)rc=-ESTALE;
    if(!rc)rc=o->owned(p,expected);
    free(original);return rc;
  }
  /* Canonical serialization at the OLD anchor compares every state field,
   * including buckets, receipt and IDs, not just a caller's pending key. */
  struct nzm_budget_sample anchor={0};
  memcpy(anchor.boot,expected->boot,sizeof anchor.boot);anchor.boottime_ns=expected->boottime_ns;
  if(!rc)rc=nzm_budget_checkpoint_text(expected,&anchor,&copy,&snapshot,&n);
  if(!rc && (n!=len || memcmp(snapshot,original,n)))rc=-ESTALE;
  if(!rc)rc=nzm_budget_settle_text(&current,sample,&expected->pending,submitted,&next,&text,&length);
  if(!rc)rc=o->owned(p,expected);
  if(!rc)rc=o->commit(p,text,length); /* One mutation; errors remain ambiguous. */
  free(original);free(snapshot);free(text);return rc;
}
int nzm_wear_store_settle(struct nzm_core *c,const struct nzm_budget *expected,
    const struct nzm_budget_sample *sample,uint64_t submitted) {
  if(!c)return -EINVAL;
  int rc=nzm_core_authority(c);
  return rc ? rc : nzm_wear_store_settle_ops(&real_ops,c,expected,sample,submitted);
}
int nzm_wear_store_load(struct nzm_core *c,struct nzm_budget *b,struct nzm_budget_sample *s) {
  if(!c)return -EINVAL;
  int rc=nzm_core_authority(c);
  return rc ? rc : nzm_wear_store_load_ops(&real_ops,c,0,b,s,NULL);
}
int nzm_wear_store_read(struct nzm_core *c,struct nzm_budget *b,struct nzm_budget_sample *s,int *migration) {
  if(!c)return -EINVAL;
  return nzm_wear_store_load_ops(&real_ops,c,1,b,s,migration);
}
int nzm_wear_store_checkpoint(struct nzm_core *c,uint64_t *deadline) {
  if(!c)return -EINVAL;
  int rc=nzm_core_authority(c);
  return rc ? rc : nzm_wear_store_checkpoint_ops(&real_ops,c,deadline);
}
