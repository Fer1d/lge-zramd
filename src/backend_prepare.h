#ifndef NZM_BACKEND_PREPARE_H
#define NZM_BACKEND_PREPARE_H
struct nzm_core;
struct nzm_helper_identity;
/* Cold, single-attempt preparation only. No DM acquisition or writeback.
 * Caller may fall back to RAM only after independently verifying cold state
 * and absence of acquisition evidence. Errors never remove partial storage. */
int nzm_backend_prepare(struct nzm_core *c, struct nzm_helper_identity *id);
#endif
