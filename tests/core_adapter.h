#ifndef CORE_ADAPTER_H
#define CORE_ADAPTER_H
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>
int test_open(const char *, int, ...);
int test_openat(int, const char *, int, ...);
#define openat test_openat
int test_fstat(int, struct stat *);
ssize_t test_xattr(int, const char *, void *, size_t);
ssize_t test_write(int, const void *, size_t);
int test_stat(const char *, struct stat *);
#define stat(path, out) test_stat(path, out)
#define open test_open
#define fstat test_fstat
#define fgetxattr test_xattr
#define write test_write
/* test_mounts.c executes the full production mounted()/mounts_verify chain
 * with syscall-boundary fixtures; test_recovery.c additionally tests its parser.
 * Core fixtures model the approved mount contract at this module boundary. */
#ifndef NZM_RECOVERY_ADAPTER
static inline int test_mounts_verify(void) { return 0; }
#define nzm_mounts_verify test_mounts_verify
#define nzm_persist_verify test_mounts_verify
#endif
#ifdef NZM_INTEGRATION
#include "nzm_io.h"
#include <dirent.h>
#include <sys/statvfs.h>
#include <time.h>
int test_fsync(int);
int test_close(int);
int test_unlinkat(int, const char *, int);
#define unlinkat test_unlinkat
#define close test_close
#define fsync test_fsync
int test_clock(clockid_t, struct timespec *);
struct dirent *test_readdir(DIR *);
#define clock_gettime test_clock
#define readdir test_readdir
int test_ioctl(int, unsigned long, ...);
int test_swapon(const char *, int);
int test_sleep(const struct timespec *, struct timespec *);
int test_space(int, struct statvfs *);
int test_helper_call(enum nzm_helper_op, const struct nzm_helper_identity *,
                     unsigned, struct nzm_helper_run *,
                     struct nzm_helper_result *);
#define ioctl test_ioctl
#define swapon test_swapon
#define nanosleep test_sleep
#define fstatvfs test_space
int test_helper_call_with_mask(enum nzm_helper_op, const struct nzm_helper_identity *,
    unsigned, const struct nzm_helper_stop *, const sigset_t *,
    struct nzm_helper_run *, struct nzm_helper_result *);
#define nzm_helper_call_with_mask test_helper_call_with_mask
#endif
#endif

#ifdef NZM_INTEGRATION
long test_sysconf(int);
#define sysconf test_sysconf
#endif
