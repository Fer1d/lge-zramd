#ifndef NZM_IO_H
#define NZM_IO_H
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <signal.h>
#include "spawn_fd.h"

/* All errors are negative errno. Fds are caller-owned unless returned by open.
 * Directory arguments must be pinned trusted fds; names are single components.
 * No runtime path overrides. Context NULL explicitly means no MAC requirement
 * (only for ancestors with a separately reviewed policy). */
struct nzm_meta { uid_t uid; gid_t gid; mode_t mode; nlink_t links; const char *context; };
int nzm_io_check(int fd, mode_t type, const struct nzm_meta *meta);
int nzm_io_dir_at(int parent, const char *name, const struct nzm_meta *meta);
int nzm_io_data_dir(int rootfd); /* exact system:system 0771 /data */
int nzm_io_private_dir_at(int parent, const char *name, const char *context);
int nzm_io_read_fd(int fd, void *buf, size_t cap, size_t *len);
int nzm_io_read_at(int dir, const char *name, const struct nzm_meta *meta,
                   void *buf, size_t cap, size_t *len);
/* One and only one write syscall; short/EINTR is uncertain, never retried. */
int nzm_io_sysfs_write(int fd, const void *buf, size_t len);
/* Caller MUST hold manager lock. Parent must be private.
 * Both ledger.new and ledger.txn block entry; core status/wear validation must
 * recognize BOTH as fail-closed pending evidence, never ignore/delete them.
 * ledger.txn: root:root 0600 nlink=1, same required context as ledger, exact
 * "nzm-ledger-txn-v1\n". Create+fsync(file)+fsync(dir) BEFORE ledger.new.
 * After rename, fsync(dir) BEFORE unlink(txn), then fsync(dir) again.
 * Failure never cleans pending. Retirement fsync failure poisons this process
 * and attempts marker restoration. After restart an absent marker is safe only
 * because ledger rename was already durable; a present marker requires review.
 * Policy needs create/write/open/getattr/fsync/unlink for ledger.txn under the
 * existing ledger private type, plus directory add_name/remove_name/write. */
/* dir_context validates only the root0700 parent (extm_wear_dir).
 * file_context validates ledger/ledger.new/ledger.txn (extm_wear_file).
 * Both are required, independently exact; this API never relabels objects. */
int nzm_io_ledger_preflight(int dirfd, size_t new_bytes);
int nzm_io_ledger_commit(int dir, const char *text, size_t len,
                         const char *dir_context, const char *file_context);
struct nzm_run_lock { int fd; dev_t dev; ino_t ino; };
/* Atomic mkdir("lock") interoperates with shell lock; no stale lock removal.
 * On acquisition errors the directory is deliberately retained fail-closed. */
int nzm_io_lock(int runfd, const char *context, struct nzm_run_lock *lock);
int nzm_io_unlock(int runfd, struct nzm_run_lock *lock);

enum nzm_helper_op { NZM_HELPER_PREPARE, NZM_HELPER_CREATE, NZM_HELPER_QUERY, NZM_HELPER_BIND };
struct nzm_helper_identity {
    uint64_t bytes, inode, fs_dev;
    unsigned backing_major, backing_minor;
    /* bind requires require_dm=1 with the previously verified create/query devt. */
    int require_dm; unsigned dm_major, dm_minor;
};
struct nzm_helper_result { unsigned dm_major, dm_minor; uint64_t extents; int bound; };
#define NZM_HELPER_CAPTURE 4096
struct nzm_helper_run {
    /* attempted means operation may have taken effect: never retry automatically,
     * including timeout, malformed output, nonzero exit and partial bind. */
    int attempted, reaped, wait_status;
    size_t out_len, err_len;
    char out[NZM_HELPER_CAPTURE + 1], err[NZM_HELPER_CAPTURE + 1];
};
/* Read-only pinned validation: no fork/execution/manager lock; takes FD gate. */
int nzm_helper_preflight(void);
void nzm_helper_diagnostic(FILE *out, enum nzm_helper_op op,
                           const struct nzm_helper_run *run, int rc,
                           int writeback_attempted);
int nzm_helper_parse(enum nzm_helper_op op, const struct nzm_helper_identity *id,
                     const char *text, size_t len, struct nzm_helper_result *result);
/* Owner-only spawn/capture with bounded exact-PID WNOHANG cleanup. No
 * waitpid(-1), external SIGCHLD reaper, SIG_IGN or SA_NOCLDWAIT. Disposition and
 * sole-reaper contract must remain stable even AFTER return while unreaped.
 * Sixteen process-owned slots are reserved before spawn; exhaustion is EBUSY.
 * After SIGKILL, cleanup has a 100ms monotonic grace and 32-probe ceiling.
 * Unresolved children retain exact PID ownership (no caller pointers); later
 * eligible call/preflight entries probe pending PIDs once, without signaling.
 * Any unresolved pending PID blocks new calls/preflight with EBUSY, attempted=0.
 * Active concurrent calls keep their own reserved slots and sole reaper.
 * Do not pthread_cancel the owner: use stop + join. Cancellation is disabled
 * until resources are released and PID ownership is released or transferred.
 * Child mask is prepared in parent, NOT inherited from the blocked worker.
 * call/call_controlled explicitly default to EMPTY (including SIGTERM). If
 * unrelated blocked signals must survive exec, use call_with_mask with the
 * process's startup mask recorded BEFORE eventloop blocks SIGTERM/signalfd
 * signals. This per-call mask is copied in parent; a blocked SIGTERM is rejected.
 * Parent blocks signals over fork/setup then restores its exact original mask.
 * Caught handlers must be child-safe on the tiny unmask->exec boundary; do not
 * ignore SIGTERM process-wide (ignored dispositions survive exec).
 * Link executable/containing DSO with -Wl,-z,now (no lazy PLT resolution in
 * child). No unsafe exec/close/signal interposers or application child-atfork
 * callbacks. This module deliberately avoids bionic/glibc fexecve fallbacks;
 * the only execve argument is a parent-formatted, still-open proc FD handle.
 * No PID is published. Stop merely sets an atomic flag; only this owner may
 * kill its still-unreaped child. ECHILD drops ownership, never kill after it.
 * A call's id/run/result and stop storage must stay alive through its return;
 * no concurrent writes to id/run/result. Reinitialize stop only after join.
 * Failure after attempted=1 is uncertain, NEVER automatically retry.
 * procfs fd handles are mandatory; failure is an error, NOT a path fallback. */
struct nzm_helper_stop { atomic_bool requested; };
void nzm_helper_stop_init(struct nzm_helper_stop *stop);
void nzm_helper_stop_request(struct nzm_helper_stop *stop);
/* exec_mask=NULL selects the explicitly empty default; otherwise caller-owned
 * immutable storage through return. SIGUSR1 etc. are preserved exactly. */
int nzm_helper_call_with_mask(enum nzm_helper_op op,
                    const struct nzm_helper_identity *id, unsigned timeout_ms,
                    const struct nzm_helper_stop *stop, const sigset_t *exec_mask,
                    struct nzm_helper_run *run, struct nzm_helper_result *result);
int nzm_helper_call_controlled(enum nzm_helper_op op,
                    const struct nzm_helper_identity *id, unsigned timeout_ms,
                    const struct nzm_helper_stop *stop, struct nzm_helper_run *run,
                    struct nzm_helper_result *result);
int nzm_helper_call(enum nzm_helper_op op, const struct nzm_helper_identity *id,
                    unsigned timeout_ms, struct nzm_helper_run *run,
                    struct nzm_helper_result *result);
#endif
