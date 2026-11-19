#ifndef NZM_PSI_H
#define NZM_PSI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define NZM_PSI_WINDOW_MIN_US UINT32_C(500000)
#define NZM_PSI_WINDOW_MAX_US UINT32_C(10000000)

struct nzm_psi_options {
    bool enabled;
    uint32_t threshold_us; /* 1 <= threshold <= window, NOT the window minimum. */
    uint32_t window_us;    /* Linux PSI accepts 500000..10000000 inclusive. */
};

/* Optional test/platform adapter. All four callbacks are required together.
 * open/write/ack/close return a descriptor/count/0, or negative errno.
 * open receives O_RDWR|O_NONBLOCK|O_CLOEXEC. write is called ONCE per open.
 * ack must consume a fake priority notification; real PSI poll consumes it.
 * open/close (including adapter-owned FD mutations) run under the process-wide
 * spawn_fd gate. They MUST NOT recursively acquire it. No permission changes.
 * Adapter storage/context must outlive runtime_destroy; context is not core.
 */
struct nzm_psi_adapter {
    void *context;
    int (*open)(void *context, int flags);
    ssize_t (*write)(void *context, int fd, const void *buf, size_t len);
    int (*ack)(void *context, int fd);
    int (*close)(void *context, int fd);
};

struct nzm_psi {
    int fd;
    struct nzm_psi_adapter adapter;
};

int nzm_psi_validate(const struct nzm_psi_options *options);
int nzm_psi_validate_adapter(const struct nzm_psi_adapter *adapter);
/* Caller holds the shared spawn_fd gate for open/close; never reuse this FD
 * outside its eventloop owner. Init failure closes any partially opened FD.
 * No retries/reopening on failure, including short writes and EINTR.
 */
int nzm_psi_open(struct nzm_psi *psi, const struct nzm_psi_options *options,
                 const struct nzm_psi_adapter *adapter);
int nzm_psi_close(struct nzm_psi *psi);
/* EPOLLPRI|EPOLLERR|EPOLLHUP, deliberately never EPOLLIN. */
uint32_t nzm_psi_events(void);
/* Negative errno means retire this trigger for the rest of this runtime. */
int nzm_psi_ack(struct nzm_psi *psi, uint32_t events);

#endif
