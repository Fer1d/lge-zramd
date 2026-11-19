#include "nzm_internal.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
static int intent, managed, ram, attempt, mapping, identity_error, fail_op, drift;
static int helpers, mutations, activated, fallback, enabled, limit, bound;
int nzm_absent(int fd, const char *name) {
  (void)fd;
  int exists = !strcmp(name,"managed") ? managed :
      !strcmp(name,"acquire-intent") ? intent :
      !strcmp(name,"ram-only") ? ram :
      !strcmp(name,"ram-only-attempted") ? attempt : 0;
  return exists ? -EEXIST : 0;
}
int nzm_no_mapping(void) { return mapping ? -EEXIST : 0; }
int nzm_cold(void) { return bound ? -EBUSY : 0; }
int nzm_existing_identity(struct nzm_core *c, struct nzm_helper_identity *id, int allow) {
  (void)c; (void)allow;
  if (identity_error) return identity_error;
  memset(id,0,sizeof *id); id->bytes=4096; id->inode=7; id->fs_dev=8;
  return 0;
}
int nzm_identity(struct nzm_core *c, struct nzm_helper_identity *id) {
  return nzm_existing_identity(c,id,0);
}
int nzm_mark(struct nzm_core *c, const char *name, const char *record) {
  (void)c; (void)record;
  if (!strcmp(name,"acquire-intent")) { assert(!mutations); intent=1; }
  else { assert(activated); managed=1; }
  return 0;
}
int nzm_putnum(const char *path, uint64_t value) {
  assert(intent); mutations++;
  if (!strcmp(path,Z "writeback_limit_enable")) { assert(value==1); enabled=1; }
  else { assert(!strcmp(path,Z "writeback_limit") && value==0); limit=0; }
  return 0;
}
int nzm_number(const char *path, uint64_t *v) {
  *v = !strcmp(path,Z "writeback_limit_enable") ? (uint64_t)enabled : (uint64_t)limit;
  return 0;
}
int nzm_rawread(const char *path, char *buf, size_t cap) {
  snprintf(buf,cap,"%s",!strcmp(path,Z "backing_dev") ?
           bound ? "/dev/block/dm-3\n" : "none\n" : "253:3\n"); return 0;
}
int nzm_helper(struct nzm_core *c, enum nzm_helper_op op,
               struct nzm_helper_identity *id, struct nzm_helper_result *r) {
  (void)c; assert(intent && enabled==1 && limit==0); helpers++;
  assert(op!=NZM_HELPER_PREPARE && id->inode==7);
  if (op!=NZM_HELPER_CREATE) assert(id->require_dm && id->dm_major==253 && id->dm_minor==3);
  if ((int)op==fail_op) return -EIO;
  r->dm_major=253; r->dm_minor=3;
  if(op==NZM_HELPER_BIND) bound=1;
  if(drift) limit=1;
  return 0;
}
int nzm_dm_activate(struct nzm_core *c, const struct nzm_helper_identity *id) {
  (void)c; assert(!nzm_dm_bound(id)); activated++; return 0;
}
int nzm_managed(struct nzm_core *c) { (void)c; assert(managed); return 0; }
int nzm_ram_boot(struct nzm_core *c) { (void)c; assert(!mutations); fallback++; return 0; }
static void reset(void) {
  intent=managed=ram=attempt=mapping=identity_error=drift=0;
  helpers=mutations=activated=fallback=enabled=bound=0; limit=9; fail_op=-1;
}
int main(void) {
  struct nzm_core c={0}; c.config.enable_writeback=1;
  reset(); assert(!nzm_dm_boot(&c)); assert(helpers==3 && activated==1 && managed && !fallback);
  assert(!nzm_dm_boot(&c)); assert(helpers==3 && activated==1);
  reset(); identity_error=-ENODATA; assert(!nzm_dm_boot(&c)); assert(fallback==1 && !intent);
  reset(); identity_error=-EACCES; assert(nzm_dm_boot(&c)==-EACCES && !mutations && !fallback);
  reset(); intent=1; assert(nzm_dm_boot(&c)==-EEXIST && !helpers && !fallback);
  reset(); attempt=1; assert(nzm_dm_boot(&c)==-EEXIST && !helpers);
  reset(); ram=1; assert(!nzm_dm_boot(&c) && fallback==1);
  reset(); mapping=1; assert(nzm_dm_boot(&c)==-EEXIST && !mutations);
  for(int op=NZM_HELPER_CREATE;op<=NZM_HELPER_BIND;op++) {
    reset(); fail_op=op; assert(nzm_dm_boot(&c)==-EIO && intent && !fallback && !activated);
    assert(nzm_dm_boot(&c)==-EEXIST);
  }
  reset(); drift=1; assert(nzm_dm_boot(&c)==-EPERM && helpers==1 && !activated && !fallback);
  reset(); c.config.enable_writeback=0; assert(!nzm_dm_boot(&c) && fallback==1 && !intent);
  puts("PASS actual DM backend: acquisition/resume/fallback/error/no-replay/quota drift");
  return 0;
}
