#ifndef NZM_STATE_BOOTSTRAP_H
#define NZM_STATE_BOOTSTRAP_H
/* Native init-only bootstrap. Never invoked by daemon or status. */
int nzm_state_bootstrap(void);
/* Open existing state only, taking extm's directory lock before authority.
 * Both descriptors belong to caller; keep startupfd until authority closes. */
int nzm_state_open(int *startupfd, int *statefd);
/* No lock or creation; diagnostics only. Returns the verified state fd. */
int nzm_state_readonly(void);
int nzm_state_bound(int startupfd, int statefd);
#endif
