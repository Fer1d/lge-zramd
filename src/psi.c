#define _GNU_SOURCE
#include "psi.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

static int real_open(void *context, int flags)
{
    int fd;
    (void)context;
    fd = open("/proc/pressure/memory", flags);
    return fd < 0 ? -errno : fd;
}

static ssize_t real_write(void *context, int fd, const void *buf, size_t len)
{
    ssize_t n;
    (void)context;
    n = write(fd, buf, len);
    return n < 0 ? -errno : n;
}

static int real_ack(void *context, int fd)
{
    (void)context;
    (void)fd;
    /* PSI's poll implementation atomically consumes its event flag. Reading
     * the pressure text is not necessary and would be an unrelated sample. */
    return 0;
}

static int real_close(void *context, int fd)
{
    (void)context;
    return close(fd) < 0 ? -errno : 0;
}

int nzm_psi_validate(const struct nzm_psi_options *options)
{
    if (!options)
        return -EINVAL;
    if (!options->enabled)
        return 0;
    if (options->window_us < NZM_PSI_WINDOW_MIN_US ||
        options->window_us > NZM_PSI_WINDOW_MAX_US ||
        !options->threshold_us || options->threshold_us > options->window_us)
        return -EINVAL;
    return 0;
}

int nzm_psi_validate_adapter(const struct nzm_psi_adapter *adapter)
{
    if (adapter && (!adapter->open || !adapter->write ||
                    !adapter->ack || !adapter->close))
        return -EINVAL;
    return 0;
}

int nzm_psi_open(struct nzm_psi *psi, const struct nzm_psi_options *options,
                 const struct nzm_psi_adapter *adapter)
{
    char trigger[80];
    int rc, n;
    ssize_t written;
    if (!psi)
        return -EINVAL;
    memset(psi, 0, sizeof(*psi));
    psi->fd = -1;
    rc = nzm_psi_validate(options);
    if (rc)
        return rc;
    rc = nzm_psi_validate_adapter(adapter);
    if (rc)
        return rc;
    if (!options->enabled)
        return 0;
    if (adapter) {
        psi->adapter = *adapter;
    } else {
        psi->adapter.open = real_open;
        psi->adapter.write = real_write;
        psi->adapter.ack = real_ack;
        psi->adapter.close = real_close;
    }
    n = snprintf(trigger, sizeof(trigger), "some %u %u\n",
                 options->threshold_us, options->window_us);
    if (n < 0 || (size_t)n >= sizeof(trigger))
        return -EOVERFLOW;
    rc = psi->adapter.open(psi->adapter.context,
                           O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (rc < 0)
        return rc;
    psi->fd = rc;
    written = psi->adapter.write(psi->adapter.context, psi->fd,
                                  trigger, (size_t)n);
    if (written != n) {
        rc = written < 0 ? (int)written : -EIO;
        (void)nzm_psi_close(psi);
        return rc;
    }
    return 0;
}

int nzm_psi_close(struct nzm_psi *psi)
{
    int fd;
    if (!psi)
        return -EINVAL;
    if (psi->fd < 0)
        return 0;
    fd = psi->fd;
    psi->fd = -1;
    /* Linux close(EINTR) has consumed the descriptor: never retry/reuse it. */
    return psi->adapter.close(psi->adapter.context, fd);
}

uint32_t nzm_psi_events(void)
{
    return EPOLLPRI | EPOLLERR | EPOLLHUP;
}

int nzm_psi_ack(struct nzm_psi *psi, uint32_t events)
{
    if (!psi || psi->fd < 0)
        return -EINVAL;
    if (events & (EPOLLERR | EPOLLHUP))
        return -EIO;
    if (!(events & EPOLLPRI))
        return 0;
    return psi->adapter.ack(psi->adapter.context, psi->fd);
}
