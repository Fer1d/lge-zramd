#define _GNU_SOURCE
#include "nzm_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#ifdef NZM_CORE_ADAPTER
#include NZM_CORE_ADAPTER
#endif
#define INTENT "exec.intent"
/* Same exact ledger type, private wear directory. Policy needs named transition
 * for exec.intent; never relabel or accept a default/wider context here. */
static const struct nzm_meta intent_meta={0,0,0600,1,LCTX};
int nzm_intent_check(struct nzm_core *c) {
  struct stat st;
  if(!c->intent_owned)return nzm_absent(c->wearfd,INTENT);
  int fd=openat(c->wearfd,INTENT,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
  if(fd<0)return -errno;
  int rc=nzm_io_check(fd,S_IFREG,&intent_meta);
  if(!rc && fstat(fd,&st))rc=-errno;
  if(!rc && (st.st_dev!=c->intent_dev || st.st_ino!=c->intent_ino))rc=-ESTALE;
  char text[sizeof c->intent_record];size_t size=0;
  if(!rc)rc=nzm_io_read_fd(fd,text,sizeof text,&size);
  if(!rc && (size!=strlen(c->intent_record) || memcmp(text,c->intent_record,size)))rc=-EPROTO;
  if(close(fd) && !rc)rc=-errno;
  return rc;
}
static int create_record(struct nzm_core *c) {
  int fd=openat(c->wearfd,INTENT,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
  if(fd<0)return -errno;
  int rc=nzm_io_check(fd,S_IFREG,&intent_meta);struct stat st;
  if(!rc && fstat(fd,&st))rc=-errno;
  if(!rc)rc=nzm_io_sysfs_write(fd,c->intent_record,strlen(c->intent_record));
  if(!rc && fsync(fd))rc=-errno;
  if(close(fd) && !rc)rc=-errno;
  if(!rc && fsync(c->wearfd))rc=-errno;
  if(!rc) {c->intent_dev=st.st_dev;c->intent_ino=st.st_ino;c->intent_owned=1;}
  /* Any failure retains even partial evidence. NEVER unlink on create failure. */
  return rc;
}
int nzm_intent_create(struct nzm_core *c,const struct nzm_plan *plan) {
  if(c->execution.backend!=RB_BACKEND_REQUEST)return 0;
  if(c->intent_owned || !c->execution.next_request_id)return -EEXIST;
  int n=snprintf(c->intent_record,sizeof c->intent_record,
      "nzm-exec-intent-v1 boot=%s request=%" PRIu64 " instance=%" PRIu64
      " generation=%" PRIu64 " rdev=%u:%u pages=%" PRIu64 " planned_kib=%" PRIu64 "\n",
      c->boot,c->execution.next_request_id,c->execution.identity.instance,
      c->execution.identity.generation,major(c->execution.transport.expected_rdev),
      minor(c->execution.transport.expected_rdev),plan->pages,plan->reserve_kib);
  if(n<0 || (size_t)n>=sizeof c->intent_record)return -EOVERFLOW;
  return create_record(c);
}
int nzm_intent_retire(struct nzm_core *c) {
  if(!c->intent_owned || !c->execution.result.known || c->execution.result.op_errno)
    return -EPERM;
  int rc=nzm_intent_check(c);
  if(rc)return rc;
  if(unlinkat(c->wearfd,INTENT,0))return -errno;
  c->intent_owned=0;
  if(fsync(c->wearfd)) {
    rc=-errno;
    /* The request is already known successful. Restore evidence best-effort;
     * never promise durability after media errors or repeat the request. */
    int restore=create_record(c);
    fprintf(stderr,"intent_retire_uncertain errno=%d evidence_restore_errno=%d\n",-rc,-restore);
    return rc;
  }
  c->intent_record[0]=0;return 0;
}
