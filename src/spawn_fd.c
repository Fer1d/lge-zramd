#define _GNU_SOURCE
#include "spawn_fd.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

/* Single process-wide gate: future eventloop uses this exact implementation. */
static pthread_mutex_t spawn_fd_gate = PTHREAD_MUTEX_INITIALIZER;

int nzm_spawn_fd_enter(struct nzm_spawn_fd_guard *guard) {
    int rc, old=0;
    if (!guard) return -EINVAL;
    if (guard->held) return -EDEADLK;
    /* Bionic deliberately has no pthread cancellation API. Host POSIX builds
     * suppress cancellation while the gate is held; Android needs no shim. */
#ifndef __ANDROID__
    rc = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old);
    if (rc) return -rc;
#endif
    rc = pthread_mutex_lock(&spawn_fd_gate);
    if (rc) {
#ifndef __ANDROID__
        (void)pthread_setcancelstate(old, NULL);
#endif
        return -rc;
    }
    guard->cancel_state = old;
    guard->held = 1;
    return 0;
}

void nzm_spawn_fd_leave(struct nzm_spawn_fd_guard *guard) {
    int old;
    if (!guard || !guard->held) abort();
    old = guard->cancel_state;
    guard->held = 0;
    if (pthread_mutex_unlock(&spawn_fd_gate)) abort();
#ifndef __ANDROID__
    if (pthread_setcancelstate(old, NULL)) abort();
#else
    (void)old;
#endif
}
