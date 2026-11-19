#ifndef NZM_SPAWN_FD_H
#define NZM_SPAWN_FD_H
/* Process-wide FD lifecycle gate, shared by helper and future eventloop.
 * One definition (spawn_fd.c) per process; NOT one mutex per caller/DSO.
 * Enter before open/openat/close/dup/pipe/socket/accept/epoll/eventfd/signalfd,
 * fdopendir/closedir, received SCM_RIGHTS, or any other FD table mutation.
 * Existing nzm_io_* routines remain raw: caller holds this gate around any
 * routine that creates/closes FDs. Also gate ownership transfer and teardown.
 * Reads/writes/poll on exclusively owned stable FDs need not hold the gate.
 * Non-recursive: do NOT call helper/preflight with the gate held. No signal
 * handler or atfork hook may acquire it or mutate FDs. No ungated library FD
 * churn is allowed across spawn. Gate is parent-only, never unlocked in child.
 * enter/leave must be paired in the same thread; guard must not be copied.
 * Host POSIX cancellation is disabled while held, restored after unlocking.
 * Android bionic has no pthread cancellation API; no emulation is installed.
 * enter returns 0 or negative errno; initialize every guard as below. */
struct nzm_spawn_fd_guard { int cancel_state, held; };
#define NZM_SPAWN_FD_GUARD_INIT {0, 0}
int nzm_spawn_fd_enter(struct nzm_spawn_fd_guard *guard);
void nzm_spawn_fd_leave(struct nzm_spawn_fd_guard *guard);
#endif
