#include "nzm_internal.h"
#include "backend_prepare.h"
#include <errno.h>
#include <string.h>

static int cold_authorized(struct nzm_core *c) {
  int paused = 0;
  int rc = nzm_core_authority(c);
  if (!rc) rc = nzm_pause_verified(c, &paused);
  if (!rc && paused) rc = -ECANCELED;
  if (!rc) rc = nzm_state_absent(c, "acquire-intent");
  if (!rc) rc = nzm_state_absent(c, "bootstrap-result");
  if (!rc) rc = nzm_state_absent(c, "managed");
  if (!rc) rc = nzm_cold();
  if (!rc) rc = nzm_no_mapping();
  return rc;
}
int nzm_backend_prepare(struct nzm_core *c, struct nzm_helper_identity *id) {
  int rc = cold_authorized(c);
  if (!rc) rc = nzm_state_absent(c, "prepare-attempted");
  if (rc) return rc;
  /* Require verified absence, not a failed metadata check or wrong size. */
  rc = nzm_existing_identity(c, id, 1);
  if (rc != -ENODATA) return rc ? rc : -EEXIST;
  if ((rc = nzm_mark(c, "prepare-attempted", c->boot))) return rc;
  if ((rc = nzm_core_provision(c)) || (rc = cold_authorized(c))) return rc;
  /* Owner verification is done by provision; absence must still be exact.
   * The pinned helper uses exclusive creation and validates actual FS policy. */
  if ((rc = nzm_absent(c->datafd, "file"))) return rc;
  memset(id, 0, sizeof *id);
  id->bytes = c->config.expected_bytes;
  struct nzm_helper_result result;
  if ((rc = nzm_helper(c, NZM_HELPER_PREPARE, id, &result))) return rc;
  if ((rc = cold_authorized(c))) return rc;
  return nzm_identity(c, id);
}
