#ifndef NZM_EVENT_ADAPTER_H
#define NZM_EVENT_ADAPTER_H
/* Compile-time syscall seams for test_main_event only. Never include this from
 * runtime.c, psi.c, request_backend.c, helper.c, or spawn_fd.c. */
#include "nzm_io.h"
#include "runtime.h"
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <sys/types.h>
#include <time.h>
int event_open(const char *, int, ...);
int event_openat(int, const char *, int, ...);
int event_fstat(int, struct stat *);
int event_fstatfs(int, struct statfs *);
int event_stat(const char *, struct stat *);
int event_lstat(const char *, struct stat *);
ssize_t event_xattr(int, const char *, void *, size_t);
ssize_t event_write(int, const void *, size_t);
int event_close(int);
int event_fsync(int);
int event_ioctl(int, unsigned long, ...);
int event_swapon(const char *, int);
int event_space(int, struct statvfs *);
int event_sleep(const struct timespec *, struct timespec *);
int event_helper(enum nzm_helper_op, const struct nzm_helper_identity *,
                 unsigned, const struct nzm_helper_stop *, const sigset_t *,
                 struct nzm_helper_run *, struct nzm_helper_result *);
int event_runtime_create(struct nzm_runtime **, const struct nzm_runtime_options *,
                         const struct nzm_runtime_executor *);
int event_runtime_stats(struct nzm_runtime *, struct nzm_runtime_stats *);
int event_runtime_destroy(struct nzm_runtime *);
int event_control_check(const struct nzm_runtime_control *, enum nzm_runtime_boundary);
int event_control_wait(const struct nzm_runtime_control *, enum nzm_runtime_boundary,
                       const struct timespec *);
#ifndef NZM_EVENT_IMPLEMENTATION
#define open event_open
#define openat event_openat
#define fstat event_fstat
#define fstatfs event_fstatfs
#define stat(path, out) event_stat(path, out)
#define lstat event_lstat
#define fgetxattr event_xattr
#define write event_write
#define close event_close
#define fsync event_fsync
#define ioctl event_ioctl
#define swapon event_swapon
#define fstatvfs event_space
#define nanosleep event_sleep
#define nzm_helper_call_with_mask event_helper
#define nzm_runtime_create event_runtime_create
#define nzm_runtime_get_stats event_runtime_stats
#define nzm_runtime_destroy event_runtime_destroy
#define nzm_runtime_control_check event_control_check
#define nzm_runtime_control_wait_until event_control_wait
#endif
#endif
