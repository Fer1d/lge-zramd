#define _GNU_SOURCE
#include "runtime.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#define NS_PER_SEC UINT64_C(1000000000)
enum fd_tag { TAG_WAKE = 1, TAG_TIMER, TAG_SIGNAL, TAG_PSI };

struct nzm_runtime_control { struct nzm_runtime *runtime; };
struct nzm_runtime {
    struct nzm_runtime_options options;
    struct nzm_psi_adapter adapter;
    struct nzm_runtime_fd_gate gate;
    struct nzm_runtime_executor executor;
    struct nzm_runtime_control control;
    pthread_mutex_t mutex;
    pthread_cond_t work_cond, stop_cond;
    pthread_t owner, worker;
    sigset_t original_mask, signal_mask;
    bool mask_saved, run_started, running, done, accepting, stop;
    bool job_ready, result_ready, busy, worker_exited, worker_joined;
    uint32_t pending;
    struct nzm_runtime_job job;
    struct nzm_runtime_result result;
    struct timespec completed_at;
    struct nzm_runtime_stats stats;
    int epoll_fd, wake_fd, timer_fd, signal_fd;
    struct nzm_psi psi;
};

static int default_enter(void *context, struct nzm_spawn_fd_guard *guard)
{
    (void)context;
    return nzm_spawn_fd_enter(guard);
}

static void default_leave(void *context, struct nzm_spawn_fd_guard *guard)
{
    (void)context;
    nzm_spawn_fd_leave(guard);
}

void nzm_runtime_options_default(struct nzm_runtime_options *options)
{
    if (!options)
        return;
    memset(options, 0, sizeof(*options));
    options->idle_sample_interval_ns = 30 * NS_PER_SEC;
    options->active_sample_interval_ns = 2 * NS_PER_SEC;
    options->max_fallback_interval_ns = 60 * NS_PER_SEC;
    options->psi.enabled = true;
    options->psi.threshold_us = 100000;
    options->psi.window_us = 1000000;
}

static int validate_options(const struct nzm_runtime_options *options)
{
    int rc;
    if (!options || !options->idle_sample_interval_ns ||
        !options->active_sample_interval_ns ||
        options->max_fallback_interval_ns < options->idle_sample_interval_ns ||
        options->max_fallback_interval_ns < options->active_sample_interval_ns ||
        options->max_fallback_interval_ns > INT64_MAX)
        return -EINVAL;
    if (options->fd_gate && (!options->fd_gate->enter || !options->fd_gate->leave))
        return -EINVAL;
    rc = nzm_psi_validate(&options->psi);
    return rc ? rc : nzm_psi_validate_adapter(options->psi_adapter);
}

uint64_t nzm_runtime_next_delay(const struct nzm_runtime_options *options,
                               const struct nzm_runtime_result *result)
{
    uint64_t minimum, delay;
    minimum = result->active ? options->active_sample_interval_ns :
                              options->idle_sample_interval_ns;
    delay = result->next_delay_ns ? result->next_delay_ns : minimum;
    if (delay < minimum)
        delay = minimum;
    if (delay > options->max_fallback_interval_ns)
        delay = options->max_fallback_interval_ns;
    return delay;
}

int nzm_runtime_deadline_after(const struct timespec *now, uint64_t delay_ns,
                               struct timespec *deadline)
{
    uint64_t seconds, nanos, max_seconds;
    if (!now || !deadline || now->tv_sec < 0 || now->tv_nsec < 0 ||
        now->tv_nsec >= (long)NS_PER_SEC)
        return -EINVAL;
    max_seconds = sizeof(time_t) >= 8 ? (uint64_t)INT64_MAX : (uint64_t)INT32_MAX;
    nanos = (uint64_t)now->tv_nsec + delay_ns % NS_PER_SEC;
    seconds = delay_ns / NS_PER_SEC + nanos / NS_PER_SEC;
    if ((uint64_t)now->tv_sec > max_seconds ||
        seconds > max_seconds - (uint64_t)now->tv_sec)
        return -EOVERFLOW;
    deadline->tv_sec = now->tv_sec + (time_t)seconds;
    deadline->tv_nsec = (long)(nanos % NS_PER_SEC);
    return 0;
}

/* mutex protects accepting and every concurrent eventfd producer. */
static int kick_locked(struct nzm_runtime *runtime)
{
    const uint64_t one = 1;
    ssize_t n;
    do {
        n = write(runtime->wake_fd, &one, sizeof(one));
    } while (n < 0 && errno == EINTR);
    if (n == (ssize_t)sizeof(one) || (n < 0 && errno == EAGAIN))
        return 0;
    return n < 0 ? -errno : -EIO;
}

static void stop_locked(struct nzm_runtime *runtime)
{
    runtime->stop = true;
    runtime->accepting = false;
    runtime->pending = 0;
    (void)pthread_cond_broadcast(&runtime->stop_cond);
    (void)pthread_cond_broadcast(&runtime->work_cond);
}

int nzm_runtime_notify(struct nzm_runtime *runtime, uint32_t reasons)
{
    int rc = 0;
    bool need_wake;
    if (!runtime || !reasons || (reasons & ~NZM_REASON_EXTERNAL))
        return -EINVAL;
    (void)pthread_mutex_lock(&runtime->mutex);
    if (!runtime->accepting) {
        rc = -ESHUTDOWN;
    } else {
        need_wake = runtime->pending == 0;
        runtime->stats.notifications++;
        if ((runtime->pending & reasons) || runtime->busy)
            runtime->stats.coalesced++;
        runtime->pending |= reasons;
        if (need_wake)
            rc = kick_locked(runtime);
    }
    (void)pthread_mutex_unlock(&runtime->mutex);
    return rc;
}

int nzm_runtime_request_stop(struct nzm_runtime *runtime)
{
    int rc = 0;
    if (!runtime)
        return -EINVAL;
    (void)pthread_mutex_lock(&runtime->mutex);
    if (!runtime->stop && !runtime->done) {
        stop_locked(runtime);
        rc = kick_locked(runtime);
    }
    (void)pthread_mutex_unlock(&runtime->mutex);
    return rc;
}

bool nzm_runtime_control_stop_requested(const struct nzm_runtime_control *ctl)
{
    bool stop;
    if (!ctl || !ctl->runtime)
        return true;
    (void)pthread_mutex_lock(&ctl->runtime->mutex);
    stop = ctl->runtime->stop;
    (void)pthread_mutex_unlock(&ctl->runtime->mutex);
    return stop;
}

static int stop_action(enum nzm_runtime_boundary boundary, bool requested)
{
    switch (boundary) {
    case NZM_BOUNDARY_OBSERVING:
    case NZM_BOUNDARY_PRECOMMIT:
        return requested ? NZM_CONTROL_STOP_BEFORE_COMMIT : NZM_CONTROL_CONTINUE;
    case NZM_BOUNDARY_POSTPRECHARGE:
    case NZM_BOUNDARY_SUBMITTING:
        return requested ? NZM_CONTROL_STOP_RETAIN_RESERVATION : NZM_CONTROL_CONTINUE;
    default:
        return -EINVAL;
    }
}

int nzm_runtime_control_check(const struct nzm_runtime_control *ctl,
                              enum nzm_runtime_boundary boundary)
{
    if (!ctl || !ctl->runtime)
        return -EINVAL;
    return stop_action(boundary, nzm_runtime_control_stop_requested(ctl));
}

int nzm_runtime_control_wait_until(const struct nzm_runtime_control *ctl,
                                   enum nzm_runtime_boundary boundary,
                                   const struct timespec *deadline)
{
    struct nzm_runtime *runtime;
    int rc = 0;
    if (!ctl || !ctl->runtime || !deadline || deadline->tv_sec < 0 ||
        deadline->tv_nsec < 0 || deadline->tv_nsec >= (long)NS_PER_SEC ||
        stop_action(boundary, false) < 0)
        return -EINVAL;
    runtime = ctl->runtime;
    (void)pthread_mutex_lock(&runtime->mutex);
    while (!runtime->stop) {
        rc = pthread_cond_timedwait(&runtime->stop_cond, &runtime->mutex, deadline);
        if (rc == ETIMEDOUT) {
            rc = 0;
            break;
        }
        if (rc) {
            rc = -rc;
            break;
        }
    }
    if (!rc)
        rc = stop_action(boundary, runtime->stop);
    (void)pthread_mutex_unlock(&runtime->mutex);
    return rc;
}

int nzm_runtime_control_get_exec_mask(const struct nzm_runtime_control *ctl,
                                      sigset_t *out)
{
    if (!ctl || !ctl->runtime || !out)
        return -EINVAL;
    *out = ctl->runtime->original_mask;
    return sigismember(out, SIGTERM) == 1 ? -EINVAL : 0;
}

static int add_fd(struct nzm_runtime *runtime, int fd, uint32_t events,
                  enum fd_tag tag)
{
    struct epoll_event event = { .events = events, .data.u32 = (uint32_t)tag };
    return epoll_ctl(runtime->epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0 ? -errno : 0;
}

static void close_one(int *fd)
{
    if (*fd >= 0) {
        (void)close(*fd); /* Never retry Linux close, including EINTR. */
        *fd = -1;
    }
}

static void close_fds_held(struct nzm_runtime *runtime)
{
    (void)nzm_psi_close(&runtime->psi);
    close_one(&runtime->signal_fd);
    close_one(&runtime->timer_fd);
    close_one(&runtime->wake_fd);
    close_one(&runtime->epoll_fd);
}

static int close_fds(struct nzm_runtime *runtime)
{
    struct nzm_spawn_fd_guard guard = NZM_SPAWN_FD_GUARD_INIT;
    int rc = runtime->gate.enter(runtime->gate.context, &guard);
    if (rc)
        return rc;
    close_fds_held(runtime);
    runtime->gate.leave(runtime->gate.context, &guard);
    return 0;
}

static int restore_mask(struct nzm_runtime *runtime)
{
    int rc = 0;
    if (runtime->mask_saved) {
        rc = pthread_sigmask(SIG_SETMASK, &runtime->original_mask, NULL);
        if (!rc)
            runtime->mask_saved = false;
    }
    return -rc;
}

static int open_fds(struct nzm_runtime *runtime)
{
    struct nzm_spawn_fd_guard guard = NZM_SPAWN_FD_GUARD_INIT;
    int rc = runtime->gate.enter(runtime->gate.context, &guard);
    if (rc)
        return rc;
    runtime->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (runtime->epoll_fd < 0) {
        rc = -errno;
        goto fail;
    }
    runtime->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (runtime->wake_fd < 0) {
        rc = -errno;
        goto fail;
    }
    runtime->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (runtime->timer_fd < 0) {
        rc = -errno;
        goto fail;
    }
    runtime->signal_fd = signalfd(-1, &runtime->signal_mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (runtime->signal_fd < 0) {
        rc = -errno;
        goto fail;
    }
    rc = add_fd(runtime, runtime->wake_fd, EPOLLIN, TAG_WAKE);
    if (!rc)
        rc = add_fd(runtime, runtime->timer_fd, EPOLLIN, TAG_TIMER);
    if (!rc)
        rc = add_fd(runtime, runtime->signal_fd, EPOLLIN, TAG_SIGNAL);
    if (rc)
        goto fail;
    rc = nzm_psi_open(&runtime->psi, &runtime->options.psi,
                      runtime->options.psi_adapter);
    if (!rc && runtime->psi.fd >= 0)
        rc = add_fd(runtime, runtime->psi.fd, nzm_psi_events(), TAG_PSI);
    if (rc) {
        (void)nzm_psi_close(&runtime->psi);
        runtime->stats.psi_error = rc;
        runtime->stats.psi_fallbacks++;
    }
    runtime->stats.timer_only = runtime->psi.fd < 0;
    runtime->gate.leave(runtime->gate.context, &guard);
    return 0; /* PSI absence/denial/unsupported never prevents timer operation. */
fail:
    close_fds_held(runtime);
    runtime->gate.leave(runtime->gate.context, &guard);
    return rc;
}

int nzm_runtime_create(struct nzm_runtime **out,
                        const struct nzm_runtime_options *options,
                        const struct nzm_runtime_executor *executor)
{
    struct nzm_runtime *runtime;
    pthread_condattr_t attr;
    int rc;
    if (!out)
        return -EINVAL;
    *out = NULL;
    rc = validate_options(options);
    if (rc || !executor || !executor->execute)
        return rc ? rc : -EINVAL;
    runtime = calloc(1, sizeof(*runtime));
    if (!runtime)
        return -ENOMEM;
    runtime->epoll_fd = runtime->wake_fd = runtime->timer_fd = runtime->signal_fd = -1;
    runtime->psi.fd = -1;
    runtime->options = *options;
    runtime->executor = *executor;
    runtime->owner = pthread_self();
    runtime->control.runtime = runtime;
    if (options->psi_adapter) {
        runtime->adapter = *options->psi_adapter;
        runtime->options.psi_adapter = &runtime->adapter;
    }
    if (options->fd_gate) {
        runtime->gate = *options->fd_gate;
    } else {
        runtime->gate.enter = default_enter;
        runtime->gate.leave = default_leave;
    }
    runtime->options.fd_gate = &runtime->gate;
    rc = pthread_mutex_init(&runtime->mutex, NULL);
    if (rc) {
        free(runtime);
        return -rc;
    }
    rc = pthread_condattr_init(&attr);
    if (rc)
        goto fail_mutex;
    rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (rc)
        goto fail_attr;
    rc = pthread_cond_init(&runtime->work_cond, &attr);
    if (rc)
        goto fail_attr;
    rc = pthread_cond_init(&runtime->stop_cond, &attr);
    if (rc) {
        (void)pthread_cond_destroy(&runtime->work_cond);
        goto fail_attr;
    }
    (void)pthread_condattr_destroy(&attr);
    rc = pthread_sigmask(SIG_SETMASK, NULL, &runtime->original_mask);
    if (rc)
        goto fail_conds;
    if (sigismember(&runtime->original_mask, SIGTERM) == 1) {
        rc = EINVAL;
        goto fail_conds;
    }
    (void)sigemptyset(&runtime->signal_mask);
    (void)sigaddset(&runtime->signal_mask, SIGINT);
    (void)sigaddset(&runtime->signal_mask, SIGTERM);
    (void)sigaddset(&runtime->signal_mask, SIGHUP);
    rc = pthread_sigmask(SIG_BLOCK, &runtime->signal_mask, NULL);
    if (rc)
        goto fail_conds;
    runtime->mask_saved = true;
    rc = open_fds(runtime);
    if (rc) {
        (void)restore_mask(runtime);
        rc = -rc;
        goto fail_conds;
    }
    runtime->accepting = true;
    *out = runtime;
    return 0;
fail_attr:
    (void)pthread_condattr_destroy(&attr);
    goto fail_mutex;
fail_conds:
    (void)pthread_cond_destroy(&runtime->stop_cond);
    (void)pthread_cond_destroy(&runtime->work_cond);
fail_mutex:
    (void)pthread_mutex_destroy(&runtime->mutex);
    free(runtime);
    return -rc;
}

static void *worker_main(void *arg)
{
    struct nzm_runtime *runtime = arg;
    int rc = 0;
#ifndef __ANDROID__
    /* No cancellation is part of this API; disable it defensively on POSIX. */
    (void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
#endif
    if (!nzm_runtime_control_stop_requested(&runtime->control) &&
        runtime->executor.initialize)
        rc = runtime->executor.initialize(runtime->executor.owner_context,
                                          &runtime->control);
    (void)pthread_mutex_lock(&runtime->mutex);
    if (rc) {
        runtime->stats.worker_error = rc < 0 ? rc : -EIO;
        stop_locked(runtime);
    }
    for (;;) {
        struct nzm_runtime_job job;
        struct nzm_runtime_result result = {0};
        struct timespec completed_at = {0};
        while (!runtime->job_ready && !runtime->stop)
            (void)pthread_cond_wait(&runtime->work_cond, &runtime->mutex);
        if (runtime->stop)
            break;
        job = runtime->job;
        runtime->job_ready = false;
        (void)pthread_mutex_unlock(&runtime->mutex);
        runtime->executor.execute(runtime->executor.owner_context, &job,
                                   &runtime->control, &result);
        rc = clock_gettime(CLOCK_MONOTONIC, &completed_at) < 0 ? -errno : 0;
        (void)pthread_mutex_lock(&runtime->mutex);
        runtime->result = result; /* no pointers/plans/borrowed business state */
        runtime->completed_at = completed_at;
        runtime->result_ready = true;
        if (rc) {
            runtime->stats.worker_error = rc;
            stop_locked(runtime);
        }
        (void)kick_locked(runtime);
    }
    (void)pthread_mutex_unlock(&runtime->mutex);
    if (runtime->executor.finalize)
        runtime->executor.finalize(runtime->executor.owner_context, &runtime->control);
    (void)pthread_mutex_lock(&runtime->mutex);
    runtime->worker_exited = true;
    (void)kick_locked(runtime); /* eventfd is alive until eventloop joins us. */
    (void)pthread_cond_broadcast(&runtime->work_cond);
    (void)pthread_mutex_unlock(&runtime->mutex);
    return NULL;
}

static int arm_timer(struct nzm_runtime *runtime, const struct timespec *from,
                     uint64_t delay)
{
    struct itimerspec timer = {0};
    int rc = nzm_runtime_deadline_after(from, delay, &timer.it_value);
    if (rc)
        return rc;
    if (timerfd_settime(runtime->timer_fd, TFD_TIMER_ABSTIME, &timer, NULL) < 0)
        return -errno;
    runtime->stats.last_delay_ns = delay;
    return 0;
}

static void add_pending(struct nzm_runtime *runtime, uint32_t reasons)
{
    (void)pthread_mutex_lock(&runtime->mutex);
    if (!runtime->stop) {
        if ((runtime->pending & reasons) || runtime->busy)
            runtime->stats.coalesced++;
        runtime->pending |= reasons;
    }
    (void)pthread_mutex_unlock(&runtime->mutex);
}

static int retire_psi(struct nzm_runtime *runtime, int error)
{
    struct nzm_spawn_fd_guard guard = NZM_SPAWN_FD_GUARD_INIT;
    int rc = runtime->gate.enter(runtime->gate.context, &guard);
    if (rc)
        return rc;
    (void)epoll_ctl(runtime->epoll_fd, EPOLL_CTL_DEL, runtime->psi.fd, NULL);
    (void)nzm_psi_close(&runtime->psi);
    runtime->gate.leave(runtime->gate.context, &guard);
    runtime->stats.psi_error = error;
    runtime->stats.psi_fallbacks++;
    runtime->stats.timer_only = true;
    add_pending(runtime, NZM_REASON_FALLBACK);
    return 0;
}

static int consume_counter(int fd, uint64_t *value)
{
    ssize_t n;
    do {
        n = read(fd, value, sizeof(*value));
    } while (n < 0 && errno == EINTR);
    if (n < 0 && errno == EAGAIN)
        return 0;
    if (n != (ssize_t)sizeof(*value))
        return n < 0 ? -errno : -EIO;
    return 1;
}

static int handle_signal(struct nzm_runtime *runtime)
{
    struct signalfd_siginfo info[8];
    ssize_t n;
    size_t i;
    do {
        n = read(runtime->signal_fd, info, sizeof(info));
    } while (n < 0 && errno == EINTR);
    if (n < 0)
        return errno == EAGAIN ? 0 : -errno;
    if (!n || n % (ssize_t)sizeof(info[0]))
        return -EIO;
    for (i = 0; i < (size_t)n / sizeof(info[0]); ++i) {
        if (info[i].ssi_signo == SIGHUP)
            runtime->stats.hup_unsupported++;
        else if (info[i].ssi_signo == SIGINT || info[i].ssi_signo == SIGTERM)
            (void)nzm_runtime_request_stop(runtime);
    }
    return 0;
}

static int handle_event(struct nzm_runtime *runtime, const struct epoll_event *event)
{
    uint64_t value;
    int rc;
    switch (event->data.u32) {
    case TAG_WAKE:
        if (event->events & (EPOLLERR | EPOLLHUP))
            return -EIO;
        rc = consume_counter(runtime->wake_fd, &value);
        return rc < 0 ? rc : 0;
    case TAG_TIMER:
        if (event->events & (EPOLLERR | EPOLLHUP))
            return -EIO;
        rc = consume_counter(runtime->timer_fd, &value);
        if (rc > 0) {
            runtime->stats.timer_events++;
            add_pending(runtime, NZM_REASON_TIMER |
                        (runtime->psi.fd < 0 ? NZM_REASON_FALLBACK : 0));
        }
        return rc < 0 ? rc : 0;
    case TAG_SIGNAL:
        if (event->events & (EPOLLERR | EPOLLHUP))
            return -EIO;
        return handle_signal(runtime);
    case TAG_PSI:
        if (runtime->psi.fd < 0)
            return 0; /* a later stale entry in this epoll batch */
        rc = nzm_psi_ack(&runtime->psi, event->events);
        if (rc < 0)
            return retire_psi(runtime, rc);
        if (event->events & EPOLLPRI) {
            runtime->stats.psi_events++;
            add_pending(runtime, NZM_REASON_PSI);
        }
        return 0;
    default:
        return -EINVAL;
    }
}

int nzm_runtime_run(struct nzm_runtime *runtime)
{
    struct timespec now;
    int rc = 0, cleanup_rc, thread_rc;
#ifndef __ANDROID__
    int old_cancel;
#endif
    if (!runtime || !pthread_equal(runtime->owner, pthread_self()))
        return -EINVAL;
    (void)pthread_mutex_lock(&runtime->mutex);
    if (runtime->run_started) {
        (void)pthread_mutex_unlock(&runtime->mutex);
        return -EALREADY;
    }
    runtime->run_started = runtime->running = true;
    (void)pthread_mutex_unlock(&runtime->mutex);
#ifndef __ANDROID__
    (void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_cancel);
#endif
    thread_rc = pthread_create(&runtime->worker, NULL, worker_main, runtime);
    if (thread_rc) {
        rc = -thread_rc;
        goto teardown;
    }
    add_pending(runtime, NZM_REASON_STARTUP);
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        rc = -errno;
    else
        rc = arm_timer(runtime, &now, runtime->options.idle_sample_interval_ns);
    while (!rc) {
        struct nzm_runtime_result result = {0};
        struct timespec completed_at = {0};
        struct epoll_event events[8];
        bool completed, stopped, exited;
        int count, i;
        (void)pthread_mutex_lock(&runtime->mutex);
        completed = runtime->result_ready;
        if (completed) {
            result = runtime->result;
            completed_at = runtime->completed_at;
            runtime->result_ready = false;
            runtime->busy = false;
            runtime->stats.jobs_completed++;
            runtime->stats.last_result_code = result.code;
        }
        stopped = runtime->stop;
        exited = runtime->worker_exited;
        if (!stopped && !runtime->busy && runtime->pending) {
            runtime->job.reasons = runtime->pending;
            runtime->pending = 0;
            runtime->job.sequence++;
            runtime->stats.last_sequence = runtime->job.sequence;
            runtime->stats.last_reasons = runtime->job.reasons;
            runtime->stats.jobs_started++;
            runtime->busy = runtime->job_ready = true;
            (void)pthread_cond_signal(&runtime->work_cond);
        }
        (void)pthread_mutex_unlock(&runtime->mutex);
        if (exited)
            break;
        if (completed && !stopped) {
            rc = arm_timer(runtime, &completed_at,
                           nzm_runtime_next_delay(&runtime->options, &result));
            if (rc)
                break;
        }
        count = epoll_wait(runtime->epoll_fd, events, 8, -1);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            rc = -errno;
            break;
        }
        for (i = 0; i < count; ++i) {
            rc = handle_event(runtime, &events[i]);
            if (rc)
                break;
        }
    }
    (void)pthread_mutex_lock(&runtime->mutex);
    stop_locked(runtime);
    /* Even on eventloop error, wait for worker's cleanup publication BEFORE
     * join. No descriptor closure or premature join to interrupt blocked I/O. */
    while (!runtime->worker_exited)
        (void)pthread_cond_wait(&runtime->work_cond, &runtime->mutex);
    if (runtime->result_ready) {
        runtime->stats.jobs_completed++;
        runtime->stats.last_result_code = runtime->result.code;
        runtime->result_ready = false;
    }
    if (!rc)
        rc = runtime->stats.worker_error;
    (void)pthread_mutex_unlock(&runtime->mutex);
    thread_rc = pthread_join(runtime->worker, NULL);
    if (thread_rc) {
        /* Fail closed: keep resources alive if join has not succeeded. */
#ifndef __ANDROID__
        (void)pthread_setcancelstate(old_cancel, NULL);
#endif
        return -thread_rc;
    }
    runtime->worker_joined = runtime->stats.worker_joined = true;
teardown:
    (void)pthread_mutex_lock(&runtime->mutex);
    /* Includes pthread_create failure: stop producers before any close/reuse.
     * accepting alone gates notify(), but request_stop() keys on stop/done. */
    stop_locked(runtime);
    (void)pthread_mutex_unlock(&runtime->mutex);
    cleanup_rc = close_fds(runtime);
    if (!cleanup_rc)
        cleanup_rc = restore_mask(runtime);
    if (!rc)
        rc = cleanup_rc;
    (void)pthread_mutex_lock(&runtime->mutex);
    runtime->running = false;
    runtime->done = true;
    (void)pthread_mutex_unlock(&runtime->mutex);
#ifndef __ANDROID__
    (void)pthread_setcancelstate(old_cancel, NULL);
#endif
    return rc;
}

int nzm_runtime_get_stats(struct nzm_runtime *runtime, struct nzm_runtime_stats *out)
{
    int rc = 0;
    if (!runtime || !out)
        return -EINVAL;
    (void)pthread_mutex_lock(&runtime->mutex);
    if (!runtime->done)
        rc = -EBUSY;
    else
        *out = runtime->stats;
    (void)pthread_mutex_unlock(&runtime->mutex);
    return rc;
}

int nzm_runtime_destroy(struct nzm_runtime *runtime)
{
    int rc;
    if (!runtime)
        return 0;
    if (!pthread_equal(runtime->owner, pthread_self()))
        return -EINVAL;
    (void)pthread_mutex_lock(&runtime->mutex);
    if (runtime->running) {
        (void)pthread_mutex_unlock(&runtime->mutex);
        return -EBUSY;
    }
    runtime->accepting = false;
    (void)pthread_mutex_unlock(&runtime->mutex);
    rc = close_fds(runtime);
    if (rc)
        return rc;
    rc = restore_mask(runtime);
    if (rc)
        return rc;
    (void)pthread_cond_destroy(&runtime->stop_cond);
    (void)pthread_cond_destroy(&runtime->work_cond);
    (void)pthread_mutex_destroy(&runtime->mutex);
    free(runtime);
    return 0;
}
