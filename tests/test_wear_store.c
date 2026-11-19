#define _GNU_SOURCE
#include "wear_store.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char boot[]="11111111-2222-3333-4444-555555555555";
static const char other[]="aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";
static const char legacy[]="wear-v1 2 100000 12\n1 4\n100000 8\n";
struct fixture {
  char *ledger,*backup;
  struct nzm_budget_sample sample;
  unsigned backups,commits,steps,fail_at;
  int residue,commit_error,owned,owned_error,commit_after_error;
  unsigned owned_checks;
};
static int step(struct fixture *f) {return ++f->steps==f->fail_at ? -EIO : 0;}
static int absent(void *p) {struct fixture *f=p;int rc=step(f);return rc ? rc : f->residue ? -EEXIST : 0;}
static int sample(void *p,struct nzm_budget_sample *s) {struct fixture *f=p;int rc=step(f);if(!rc)*s=f->sample;return rc;}
static int read_file(void *p,const char *name,char **out,size_t *len) {
  struct fixture *f=p;assert(!strcmp(name,"ledger"));int rc=step(f);if(rc)return rc;
  if(!f->ledger)return -ENOENT;
  *len=strlen(f->ledger);*out=strdup(f->ledger);assert(*out);return 0;
}
static int backup(void *p,const char *s,size_t n) {
  struct fixture *f=p;assert(n==strlen(f->ledger) && !memcmp(s,f->ledger,n));
  int rc=step(f);if(rc)return rc;
  if(f->backup)return strlen(f->backup)==n && !memcmp(f->backup,s,n) ? 0 : -EPROTO;
  f->backups++;f->backup=strndup(s,n);assert(f->backup);return 0;
}
static int commit(void *p,const char *s,size_t n) {
  struct fixture *f=p;int rc=step(f);if(rc)return rc;
  if(f->commit_error && !f->commit_after_error)return f->commit_error;
  if(!strncmp(f->ledger,"wear-v1 ",8))assert(f->backup && !strcmp(f->backup,f->ledger));
  struct nzm_budget b;int migrated;
  assert(!nzm_budget_parse(s,n,&f->sample,&b,&migrated) && !migrated);
  free(f->ledger);f->ledger=strndup(s,n);assert(f->ledger);f->commits++;return f->commit_error;
}
static int owned(void *p,const struct nzm_budget *expected) {
  struct fixture *f=p;assert(expected->pending.request_id);f->owned_checks++;
  int rc=step(f);return rc ? rc : f->owned_error ? f->owned_error :
      (!f->owned || f->residue==2) ? -EPERM : 0;
}
static const struct nzm_wear_store_ops ops={absent,sample,read_file,backup,commit,owned};
static void init(struct fixture *f) {
  memset(f,0,sizeof *f);f->ledger=strdup(legacy);assert(f->ledger);
  memcpy(f->sample.boot,boot,sizeof boot);f->sample.boottime_ns=100;
}
static void finish(struct fixture *f) {free(f->ledger);free(f->backup);}
static int load(struct fixture *f,int readonly,struct nzm_budget *b,int *migration) {
  struct nzm_budget_sample s;
  return nzm_wear_store_load_ops(&ops,f,readonly,b,&s,migration);
}
static void reserve(struct fixture *f,struct nzm_budget *pending) {
  struct nzm_budget b;assert(!load(f,0,&b,NULL));
  char *text=NULL;size_t n=0;
  assert(!nzm_budget_reserve_text(&b,&f->sample,80,1000,b.next_request_id,pending,&text,&n));
  free(f->ledger);f->ledger=text;f->owned=1;f->residue=1;
  f->steps=f->commits=f->owned_checks=0;
  f->sample.boottime_ns+=NZM_WEAR_BUCKET_NS;
}
static int settle(struct fixture *f,const struct nzm_budget *pending,uint64_t submitted) {
  return nzm_wear_store_settle_ops(&ops,f,pending,&f->sample,submitted);
}
int main(void) {
  struct fixture f;struct nzm_budget b;int migration;uint64_t deadline,used,available,expiry;
  init(&f);assert(!load(&f,1,&b,&migration) && migration && !f.commits && !f.backups);
  assert(!nzm_budget_budget(&b,&f.sample,100,&used,&available,&expiry) && used==12);
  f.steps=0;
  assert(!load(&f,0,&b,&migration) && migration && f.commits==1 && f.backups==1);
  assert(!strcmp(f.backup,legacy) && b.cumulative_kib==12 && b.next_request_id==1);
  unsigned steps=f.steps; /* Fault every migration boundary, never fabricate success. */
  finish(&f);
  for(unsigned i=1;i<=steps;i++) {
    init(&f);f.fail_at=i;assert(load(&f,0,&b,NULL)<0);assert(!strcmp(f.ledger,legacy));finish(&f);
  }
  init(&f);f.backup=strdup("partial");assert(load(&f,0,&b,NULL)==-EPROTO && !f.commits);finish(&f);
  init(&f);f.backup=strdup(legacy);assert(!load(&f,0,&b,NULL) && !f.backups && f.commits==1);finish(&f);
  init(&f);f.residue=1;assert(load(&f,0,&b,NULL)==-EEXIST && !f.backups && !f.commits);finish(&f);
  init(&f);free(f.ledger);f.ledger=NULL;assert(load(&f,0,&b,NULL)==-ENOENT && !f.commits);finish(&f);
  init(&f);f.commit_error=-ENOSPC;assert(load(&f,0,&b,NULL)==-ENOSPC && f.backups && !strcmp(f.ledger,legacy));finish(&f);
  init(&f);assert(!load(&f,0,&b,NULL));
  unsigned commits=f.commits;
  assert(!nzm_wear_store_checkpoint_ops(&ops,&f,&deadline) && deadline==100+NZM_WEAR_CHECKPOINT_NS);
  for(unsigned i=1;i<60;i++) {
    f.sample.boottime_ns=100+i*UINT64_C(30000000000);
    assert(!nzm_wear_store_checkpoint_ops(&ops,&f,&deadline) && f.commits==commits);
  }
  f.sample.boottime_ns=100+NZM_WEAR_CHECKPOINT_NS;
  assert(!nzm_wear_store_checkpoint_ops(&ops,&f,&deadline) && f.commits==commits+1);
  assert(!load(&f,1,&b,NULL));uint64_t persisted=b.logical_ns;
  memcpy(f.sample.boot,other,sizeof other);f.sample.boottime_ns=UINT64_C(999999999999);
  assert(!load(&f,1,&b,NULL) && f.commits==commits+1);
  uint64_t logical;assert(!nzm_budget_project(&b,&f.sample,&logical) && logical==persisted);
  assert(!load(&f,0,&b,NULL) && f.commits==commits+2 && b.logical_ns==persisted);
  assert(!strcmp(b.boot,other) && b.boottime_ns==f.sample.boottime_ns);
  /* No unchanged-boot load writes; time projection does not mutate anchors. */
  assert(!load(&f,0,&b,NULL) && f.commits==commits+2);
  f.sample.boottime_ns+=NZM_WEAR_WINDOW_NS+NZM_WEAR_BUCKET_NS;
  assert(!nzm_wear_store_checkpoint_ops(&ops,&f,&deadline) && !deadline);
  commits=f.commits;f.sample.boottime_ns+=NZM_WEAR_CHECKPOINT_NS;
  assert(!nzm_wear_store_checkpoint_ops(&ops,&f,&deadline) && !deadline && f.commits==commits);
  finish(&f);
  /* v2 format-only upgrade keeps every anchor/bucket, even when now expired. */
  init(&f);free(f.ledger);f.ledger=strdup("wear-v2 2 12 11111111-2222-3333-4444-555555555555 900000000000 100 9 1\n1 12\n");
  f.sample.boottime_ns+=NZM_WEAR_WINDOW_NS+NZM_WEAR_BUCKET_NS;
  assert(!load(&f,1,&b,&migration) && migration==NZM_BUDGET_MIGRATED_V2);
  assert(!load(&f,0,&b,&migration) && migration==NZM_BUDGET_MIGRATED_V2);
  assert(!f.backups && f.commits==1 && b.logical_ns==NZM_WEAR_BUCKET_NS && b.boottime_ns==100);
  assert(b.count==1 && b.buckets[0].index==1 && b.buckets[0].kib==12 && b.next_request_id==9);
  assert(!nzm_budget_budget(&b,&f.sample,100,&used,&available,&expiry) && !used);
  finish(&f);
  struct nzm_budget pending;
  init(&f);reserve(&f,&pending);assert(!settle(&f,&pending,4));
  assert(f.commits==1 && f.owned_checks==2);unsigned settle_steps=f.steps;
  /* Ordinary reads/checkpoints reject even an OWNED intent. */
  assert(load(&f,0,&b,NULL)==-EEXIST);
  assert(nzm_wear_store_checkpoint_ops(&ops,&f,&deadline)==-EEXIST);
  assert(!settle(&f,&pending,4) && f.commits==1); /* receipt, no double credit */
  assert(settle(&f,&pending,8)==-ESTALE && f.commits==1);
  f.residue=0;assert(!load(&f,1,&b,NULL) && !b.pending.request_id && b.known_submitted_kib==4);
  assert(b.cumulative_kib==92 && b.receipt.submitted_kib==4);
  assert(b.buckets[b.count-1].index==1); /* fresh completion, not reservation */
  finish(&f);
  for(unsigned i=1;i<=settle_steps;i++) {
    init(&f);reserve(&f,&pending);f.fail_at=i;
    assert(settle(&f,&pending,4)<0 && !f.commits);finish(&f);
  }
  init(&f);reserve(&f,&pending);f.owned=0;assert(settle(&f,&pending,4)==-EPERM && !f.commits);finish(&f);
  init(&f);reserve(&f,&pending);f.owned_error=-ESTALE;assert(settle(&f,&pending,4)==-ESTALE && !f.commits);finish(&f);
  init(&f);reserve(&f,&pending);f.residue=2;assert(settle(&f,&pending,4)==-EPERM && !f.commits);finish(&f);
  init(&f);reserve(&f,&pending);struct nzm_budget stale=pending;stale.cumulative_kib++;
  assert(settle(&f,&stale,4)==-ESTALE && !f.commits);finish(&f);
  init(&f);reserve(&f,&pending);stale=pending;stale.pending.request_id++;
  assert(settle(&f,&stale,4)==-ESTALE && !f.commits);finish(&f);
  init(&f);reserve(&f,&pending);assert(settle(&f,&pending,81)==-EINVAL && !f.commits);finish(&f);
  for(int post=0;post<=1;post++) {
    init(&f);reserve(&f,&pending);f.commit_error=-EIO;f.commit_after_error=post;
    assert(settle(&f,&pending,4)==-EIO && f.commits==(unsigned)post);
    /* Caller must halt rather than retry this ambiguous mutation. */
    finish(&f);
  }
  init(&f);reserve(&f,&pending);assert(!settle(&f,&pending,0) && f.commits==1);
  assert(!settle(&f,&pending,0) && f.commits==1);finish(&f);
  init(&f);reserve(&f,&pending);assert(!settle(&f,&pending,80));
  f.residue=0;assert(!load(&f,1,&b,NULL) && b.known_submitted_kib==80 && !b.pending.request_id);finish(&f);
  puts("PASS wear store: v1 backup, exact v2 conversion, narrow owned settlement, receipt idempotence, fault matrix");
  return 0;
}
