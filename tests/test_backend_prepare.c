#include "nzm_internal.h"
#include "backend_prepare.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
static int marked, provisioned, prepared, identity_error, provision_error;
static int helper_error, paused, acquire, mapping, file, policy_error, drift;
int nzm_core_authority(struct nzm_core *c) { (void)c; return policy_error; }
int nzm_pause_verified(struct nzm_core *c,int *p) { (void)c; *p=paused; return 0; }
int nzm_state_absent(struct nzm_core *c,const char *name) {
  (void)c;
  return ((!strcmp(name,"prepare-attempted") && marked) ||
          (!strcmp(name,"acquire-intent") && acquire)) ? -EEXIST : 0;
}
int nzm_no_mapping(void) { return mapping ? -EEXIST : 0; }
int nzm_cold(void) { return drift ? -EBUSY : 0; }
int nzm_existing_identity(struct nzm_core *c,struct nzm_helper_identity *id,int allow) {
  (void)c; (void)id; assert(allow); return identity_error;
}
int nzm_mark(struct nzm_core *c,const char *name,const char *record) {
  (void)c; (void)record; assert(!strcmp(name,"prepare-attempted"));
  assert(!provisioned && !prepared); marked=1; return 0;
}
int nzm_core_provision(struct nzm_core *c) {
  (void)c; assert(marked); provisioned++; return provision_error;
}
int nzm_absent(int fd,const char *name) {
  (void)fd; assert(provisioned && !strcmp(name,"file")); return file ? -EEXIST : 0;
}
int nzm_helper(struct nzm_core *c,enum nzm_helper_op op,
               struct nzm_helper_identity *id,struct nzm_helper_result *r) {
  (void)r; assert(marked && provisioned && op==NZM_HELPER_PREPARE);
  assert(id->bytes==c->config.expected_bytes && !id->inode && !id->require_dm);
  prepared++; file=1; return helper_error;
}
int nzm_identity(struct nzm_core *c,struct nzm_helper_identity *id) {
  (void)c; assert(prepared && file); id->inode=7; return 0;
}
static void reset(void) {
  marked=provisioned=prepared=provision_error=helper_error=paused=acquire=0;
  mapping=file=policy_error=drift=0; identity_error=-ENODATA;
}
int main(void) {
  struct nzm_core c={0}; struct nzm_helper_identity id={0};
  c.config.expected_bytes=UINT64_C(1073741824);
  reset(); assert(!nzm_backend_prepare(&c,&id));
  assert(marked && provisioned==1 && prepared==1 && id.inode==7);
  assert(nzm_backend_prepare(&c,&id)==-EEXIST && prepared==1);
  reset(); paused=1; assert(nzm_backend_prepare(&c,&id)==-ECANCELED && !marked);
  reset(); acquire=1; assert(nzm_backend_prepare(&c,&id)==-EEXIST && !marked);
  reset(); mapping=1; assert(nzm_backend_prepare(&c,&id)==-EEXIST && !marked);
  reset(); drift=1; assert(nzm_backend_prepare(&c,&id)==-EBUSY && !marked);
  reset(); policy_error=-EPERM; assert(nzm_backend_prepare(&c,&id)==-EPERM && !marked);
  const int errors[]={-EACCES,-EPERM,-EIO,-ENOENT,0};
  for(unsigned i=0;i<sizeof errors/sizeof errors[0];i++) {
    reset(); identity_error=errors[i];
    assert(nzm_backend_prepare(&c,&id)==(errors[i]?errors[i]:-EEXIST));
    assert(!marked && !provisioned && !prepared);
  }
  reset(); provision_error=-EACCES;
  assert(nzm_backend_prepare(&c,&id)==-EACCES && marked && !prepared);
  assert(nzm_backend_prepare(&c,&id)==-EEXIST);
  reset(); file=1; assert(nzm_backend_prepare(&c,&id)==-EEXIST && marked && !prepared);
  reset(); helper_error=-ETIMEDOUT;
  assert(nzm_backend_prepare(&c,&id)==-ETIMEDOUT && file && marked && prepared==1);
  assert(nzm_backend_prepare(&c,&id)==-EEXIST && file && prepared==1);
  puts("PASS cold preparation: exact absence, authorization, exclusive file, durable no-replay and partial preservation");
  return 0;
}
