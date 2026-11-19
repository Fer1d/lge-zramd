#define _GNU_SOURCE
#include "runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define NS_MS UINT64_C(1000000)
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #expr, errno); \
    abort(); \
} } while (0)

static unsigned test_count;
static _Thread_local unsigned gate_depth;

/* Link-time fault injection stays entirely in tests: no runtime API hooks. */
int __real_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                           void *(*start)(void *), void *arg);
int __real_close(int fd);
int __real_eventfd(unsigned initval, int flags);
static atomic_bool fail_next_thread;
static atomic_bool capture_eventfd;
static atomic_int captured_eventfd;
struct close_fault {
    struct nzm_runtime *runtime;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int target_fd, replacement_fd;
    bool in_reuse_window, producer_done;
};
static _Atomic(struct close_fault *) active_close_fault;

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                           void *(*start)(void *), void *arg)
{
    if (atomic_exchange(&fail_next_thread, false))
        return EAGAIN;
    return __real_pthread_create(thread, attr, start, arg);
}

int __wrap_eventfd(unsigned initval, int flags)
{
    int fd = __real_eventfd(initval, flags);
    if (atomic_load(&capture_eventfd))
        atomic_store(&captured_eventfd, fd);
    return fd;
}

int __wrap_close(int fd)
{
    struct close_fault *fault = atomic_load(&active_close_fault);
    uint64_t unexpected;
    ssize_t n;
    if (!fault || fd != fault->target_fd)
        return __real_close(fd);
    CHECK(gate_depth == 1);
    CHECK(__real_close(fd) == 0);
    /* Reuse the exact just-closed wakefd number while runtime close_one() has
     * not returned. A late kick would now corrupt an unrelated eventfd. All
     * FD-table operations remain within runtime's existing public gate. */
    CHECK(dup2(fault->replacement_fd, fd) == fd);
    CHECK(pthread_mutex_lock(&fault->mutex) == 0);
    fault->in_reuse_window = true;
    CHECK(pthread_cond_broadcast(&fault->cond) == 0);
    while (!fault->producer_done)
        CHECK(pthread_cond_wait(&fault->cond, &fault->mutex) == 0);
    CHECK(pthread_mutex_unlock(&fault->mutex) == 0);
    n = read(fault->replacement_fd, &unexpected, sizeof(unexpected));
    CHECK(n == -1 && errno == EAGAIN); /* absolutely no late eventfd producer */
    return __real_close(fd);
}

static void passed(const char *name)
{
    printf("PASS %s\n", name);
    ++test_count;
}

static void sleep_ms(unsigned ms)
{
    struct timespec request = { .tv_sec = (time_t)(ms / 1000),
                                .tv_nsec = (long)(ms % 1000) * 1000000L };
    while (nanosleep(&request, &request) < 0)
        CHECK(errno == EINTR);
}

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static int tracked_enter(void *context, struct nzm_spawn_fd_guard *guard)
{
    int rc;
    (void)context;
    CHECK(gate_depth == 0);
    rc = nzm_spawn_fd_enter(guard);
    if (!rc)
        gate_depth = 1;
    return rc;
}

static void tracked_leave(void *context, struct nzm_spawn_fd_guard *guard)
{
    (void)context;
    CHECK(gate_depth == 1 && guard->held);
    gate_depth = 0;
    nzm_spawn_fd_leave(guard);
}

static const struct nzm_runtime_fd_gate tracked_gate = {
    .enter = tracked_enter, .leave = tracked_leave
};

static void gated_pipe(int pipefd[2])
{
    struct nzm_spawn_fd_guard guard = NZM_SPAWN_FD_GUARD_INIT;
    CHECK(nzm_spawn_fd_enter(&guard) == 0);
    CHECK(pipe2(pipefd, O_CLOEXEC) == 0);
    nzm_spawn_fd_leave(&guard);
}

static void gated_close(int fd)
{
    struct nzm_spawn_fd_guard guard = NZM_SPAWN_FD_GUARD_INIT;
    CHECK(nzm_spawn_fd_enter(&guard) == 0);
    CHECK(close(fd) == 0);
    nzm_spawn_fd_leave(&guard);
}

static void put_byte(int fd)
{
    char byte = 'x';
    ssize_t n;
    do {
        n = write(fd, &byte, 1);
    } while (n < 0 && errno == EINTR);
    CHECK(n == 1);
}

static void get_byte(int fd)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    char byte;
    ssize_t n;
    int rc;
    do {
        rc = poll(&pfd, 1, 5000);
    } while (rc < 0 && errno == EINTR);
    CHECK(rc == 1 && (pfd.revents & POLLIN));
    do {
        n = read(fd, &byte, 1);
    } while (n < 0 && errno == EINTR);
    CHECK(n == 1);
}

struct fake_psi {
    int peer, fd, flags, open_error, write_error, ack_error;
    bool short_write;
    unsigned opens, writes, closes;
    atomic_uint acks;
    char trigger[80];
};

static int fake_open(void *context, int flags)
{
    struct fake_psi *fake = context;
    int pair[2];
    CHECK(gate_depth == 1);
    fake->opens++;
    fake->flags = flags;
    if (fake->open_error)
        return -fake->open_error;
    /* AF_UNIX MSG_OOB delivers real EPOLLPRI on this host. No EPOLLIN adapter
     * escape hatch: runtime uses the same interest mask as production PSI. */
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, pair) < 0)
        return -errno;
    fake->fd = pair[0];
    fake->peer = pair[1];
    return pair[0];
}

static ssize_t fake_write(void *context, int fd, const void *buf, size_t len)
{
    struct fake_psi *fake = context;
    CHECK(gate_depth == 1 && fd == fake->fd && len < sizeof(fake->trigger));
    memcpy(fake->trigger, buf, len);
    fake->trigger[len] = '\0';
    fake->writes++;
    if (fake->write_error)
        return -fake->write_error;
    return (ssize_t)len - (fake->short_write ? 1 : 0);
}

static int fake_ack(void *context, int fd)
{
    struct fake_psi *fake = context;
    char byte;
    ssize_t n;
    CHECK(gate_depth == 0 && fd == fake->fd);
    atomic_fetch_add(&fake->acks, 1);
    if (fake->ack_error)
        return -fake->ack_error;
    n = recv(fd, &byte, 1, MSG_OOB | MSG_DONTWAIT);
    return n == 1 ? 0 : n < 0 ? -errno : -EIO;
}

static int fake_close(void *context, int fd)
{
    struct fake_psi *fake = context;
    CHECK(gate_depth == 1 && fd == fake->fd);
    fake->closes++;
    return close(fd) < 0 ? -errno : 0;
}

static void fake_init(struct fake_psi *fake)
{
    memset(fake, 0, sizeof(*fake));
    fake->peer = fake->fd = -1;
    atomic_init(&fake->acks, 0);
}

static struct nzm_psi_adapter fake_adapter(struct fake_psi *fake)
{
    return (struct nzm_psi_adapter){
        .context = fake, .open = fake_open, .write = fake_write,
        .ack = fake_ack, .close = fake_close
    };
}

static void fake_priority(struct fake_psi *fake)
{
    char byte = '!';
    CHECK(send(fake->peer, &byte, 1, MSG_OOB | MSG_NOSIGNAL) == 1);
}

static void fake_release(struct fake_psi *fake)
{
    if (fake->peer >= 0) {
        gated_close(fake->peer);
        fake->peer = -1;
    }
}

static void wait_acks(struct fake_psi *fake, unsigned count)
{
    uint64_t end = monotonic_ns() + 4000 * NS_MS;
    while (atomic_load(&fake->acks) < count) {
        CHECK(monotonic_ns() < end);
        sleep_ms(1);
    }
}

static struct nzm_runtime_options test_options(void)
{
    struct nzm_runtime_options options;
    nzm_runtime_options_default(&options);
    options.psi.enabled = false;
    options.idle_sample_interval_ns = 80 * NS_MS;
    options.active_sample_interval_ns = 20 * NS_MS;
    options.max_fallback_interval_ns = 400 * NS_MS;
    options.fd_gate = &tracked_gate;
    return options;
}

static void verify_restored(const sigset_t *original)
{
    sigset_t current;
    int signal_number;
    CHECK(pthread_sigmask(SIG_SETMASK, NULL, &current) == 0);
    for (signal_number = 1; signal_number < NSIG; ++signal_number)
        CHECK(sigismember(original, signal_number) == sigismember(&current, signal_number));
}

static void no_execute(void *context, const struct nzm_runtime_job *job,
                        const struct nzm_runtime_control *ctl,
                        struct nzm_runtime_result *result)
{
    (void)context; (void)job; (void)ctl; (void)result;
    CHECK(false);
}

static void test_validation_clock(void)
{
    struct nzm_runtime_options options = test_options();
    struct nzm_runtime_executor executor = { .execute = no_execute };
    struct nzm_runtime_result result = {0};
    struct nzm_runtime *runtime = NULL;
    struct timespec now = { .tv_sec = 123, .tv_nsec = 999999999L }, deadline;
    struct nzm_psi_options psi = { .enabled = true, .threshold_us = 1, .window_us = 500000 };
    sigset_t original, blocked;
    CHECK(nzm_psi_validate(&psi) == 0);
    psi.window_us = 10000000; psi.threshold_us = 10000000;
    CHECK(nzm_psi_validate(&psi) == 0);
    psi.window_us = 499999; psi.threshold_us = 1;
    CHECK(nzm_psi_validate(&psi) == -EINVAL);
    psi.window_us = 10000001;
    CHECK(nzm_psi_validate(&psi) == -EINVAL);
    psi.window_us = 500000; psi.threshold_us = 500001;
    CHECK(nzm_psi_validate(&psi) == -EINVAL);
    psi.threshold_us = 0;
    CHECK(nzm_psi_validate(&psi) == -EINVAL);
    CHECK(nzm_psi_events() == (EPOLLPRI | EPOLLERR | EPOLLHUP));
    CHECK(!(nzm_psi_events() & EPOLLIN));
    CHECK(nzm_runtime_deadline_after(&now, 2, &deadline) == 0);
    CHECK(deadline.tv_sec == 124 && deadline.tv_nsec == 1);
    now.tv_sec = sizeof(time_t) >= 8 ? (time_t)INT64_MAX : (time_t)INT32_MAX;
    CHECK(nzm_runtime_deadline_after(&now, 1, &deadline) == -EOVERFLOW);
    now.tv_sec = 0; now.tv_nsec = 1000000000L;
    CHECK(nzm_runtime_deadline_after(&now, 1, &deadline) == -EINVAL);
    CHECK(nzm_runtime_next_delay(&options, &result) == 80 * NS_MS);
    result.active = true;
    CHECK(nzm_runtime_next_delay(&options, &result) == 20 * NS_MS);
    result.next_delay_ns = 1;
    CHECK(nzm_runtime_next_delay(&options, &result) == 20 * NS_MS);
    result.next_delay_ns = UINT64_MAX;
    CHECK(nzm_runtime_next_delay(&options, &result) == 400 * NS_MS);
    result.next_delay_ns = 50 * NS_MS;
    CHECK(nzm_runtime_next_delay(&options, &result) == 50 * NS_MS);
    options.idle_sample_interval_ns = 0;
    CHECK(nzm_runtime_create(&runtime, &options, &executor) == -EINVAL && !runtime);
    options = test_options();
    CHECK(pthread_sigmask(SIG_SETMASK, NULL, &original) == 0);
    blocked = original;
    CHECK(sigaddset(&blocked, SIGTERM) == 0);
    CHECK(pthread_sigmask(SIG_SETMASK, &blocked, NULL) == 0);
    CHECK(nzm_runtime_create(&runtime, &options, &executor) == -EINVAL && !runtime);
    verify_restored(&blocked);
    CHECK(pthread_sigmask(SIG_SETMASK, &original, NULL) == 0);
    CHECK(nzm_runtime_create(&runtime, &options, &executor) == 0);
    CHECK(nzm_runtime_destroy(runtime) == 0); /* no worker was created */
    verify_restored(&original);
    passed("validation, PSI ranges/mask, monotonic overflow, interval clamps, original-mask rejection");
}

static void test_psi_direct(void)
{
    const int errors[] = { ENOENT, EOPNOTSUPP, EACCES, EPERM };
    struct nzm_psi_options options = { .enabled = true, .threshold_us = 1234, .window_us = 500000 };
    struct nzm_spawn_fd_guard guard = NZM_SPAWN_FD_GUARD_INIT;
    struct fake_psi fake;
    struct nzm_psi psi;
    struct nzm_psi_adapter adapter;
    unsigned i;
    for (i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        fake_init(&fake); adapter = fake_adapter(&fake);
        fake.open_error = errors[i];
        CHECK(tracked_enter(NULL, &guard) == 0);
        CHECK(nzm_psi_open(&psi, &options, &adapter) == -errors[i]);
        tracked_leave(NULL, &guard);
        CHECK(psi.fd == -1 && fake.opens == 1 && fake.writes == 0 && fake.closes == 0);
    }
    for (i = 0; i < 3; ++i) {
        fake_init(&fake); adapter = fake_adapter(&fake);
        fake.short_write = i == 0;
        fake.write_error = i == 1 ? EINTR : i == 2 ? EACCES : 0;
        CHECK(tracked_enter(NULL, &guard) == 0);
        CHECK(nzm_psi_open(&psi, &options, &adapter) == -(i == 0 ? EIO : fake.write_error));
        tracked_leave(NULL, &guard);
        CHECK(psi.fd == -1 && fake.opens == 1 && fake.writes == 1 && fake.closes == 1);
        fake_release(&fake);
    }
    fake_init(&fake); adapter = fake_adapter(&fake);
    CHECK(tracked_enter(NULL, &guard) == 0);
    CHECK(nzm_psi_open(&psi, &options, &adapter) == 0);
    tracked_leave(NULL, &guard);
    CHECK(strcmp(fake.trigger, "some 1234 500000\n") == 0);
    CHECK((fake.flags & O_ACCMODE) == O_RDWR);
    CHECK((fake.flags & (O_NONBLOCK | O_CLOEXEC)) == (O_NONBLOCK | O_CLOEXEC));
    CHECK(nzm_psi_ack(&psi, EPOLLIN) == 0 && atomic_load(&fake.acks) == 0);
    CHECK(nzm_psi_ack(&psi, EPOLLERR) == -EIO);
    CHECK(nzm_psi_ack(&psi, EPOLLHUP) == -EIO);
    CHECK(tracked_enter(NULL, &guard) == 0);
    CHECK(nzm_psi_close(&psi) == 0);
    CHECK(nzm_psi_close(&psi) == 0);
    tracked_leave(NULL, &guard);
    CHECK(fake.closes == 1);
    fake_release(&fake);
    passed("PSI exact trigger/newline/flags, ERR/HUP, no partial-write retry, gate coverage");
}

struct busy_context {
    struct nzm_runtime *runtime;
    struct fake_psi *fake;
    pthread_t loop, worker;
    int entered[2], release[2];
    unsigned calls, initialized, finalized;
    uint32_t reasons[4];
    atomic_uint executed;
    atomic_bool producer_done;
};

static int busy_initialize(void *context, const struct nzm_runtime_control *ctl)
{
    struct busy_context *busy = context;
    sigset_t current;
    (void)ctl;
    busy->worker = pthread_self();
    CHECK(!pthread_equal(busy->loop, busy->worker));
    CHECK(pthread_sigmask(SIG_SETMASK, NULL, &current) == 0);
    CHECK(sigismember(&current, SIGTERM) == 1 && sigismember(&current, SIGINT) == 1 &&
          sigismember(&current, SIGHUP) == 1);
    busy->initialized++;
    return 0;
}

static void busy_execute(void *context, const struct nzm_runtime_job *job,
                          const struct nzm_runtime_control *ctl,
                          struct nzm_runtime_result *result)
{
    struct busy_context *busy = context;
    (void)ctl;
    CHECK(pthread_equal(busy->worker, pthread_self()));
    CHECK(busy->calls < 4 && job->sequence == busy->calls + 1);
    busy->reasons[busy->calls++] = job->reasons;
    atomic_store(&busy->executed, busy->calls);
    put_byte(busy->entered[1]);
    if (busy->calls <= 2)
        get_byte(busy->release[0]); /* genuine worker-owned blocking pipe I/O */
    result->next_delay_ns = 300 * NS_MS;
    result->code = 17;
}

static void busy_finalize(void *context, const struct nzm_runtime_control *ctl)
{
    struct busy_context *busy = context;
    CHECK(pthread_equal(busy->worker, pthread_self()));
    CHECK(nzm_runtime_control_stop_requested(ctl));
    busy->finalized++;
    /* Worker alone closes its pipe endpoints; runtime must never close them. */
    gated_close(busy->entered[1]);
    gated_close(busy->release[0]);
}

static void *burst_producer(void *context)
{
    struct busy_context *busy = context;
    unsigned i;
    for (i = 0; i < 25000; ++i)
        CHECK(nzm_runtime_notify(busy->runtime, NZM_REASON_MANUAL) == 0);
    return NULL;
}

static void *busy_driver(void *context)
{
    struct busy_context *busy = context;
    pthread_t producers[4];
    unsigned i;
    get_byte(busy->entered[0]);
    for (i = 0; i < 4; ++i)
        CHECK(pthread_create(&producers[i], NULL, burst_producer, busy) == 0);
    for (i = 0; i < 4; ++i)
        CHECK(pthread_join(producers[i], NULL) == 0);
    for (i = 0; i < 16; ++i) {
        fake_priority(busy->fake);
        wait_acks(busy->fake, i + 1);
    }
    sleep_ms(100); /* let the single initial timer expire while worker is busy */
    CHECK(atomic_load(&busy->executed) == 1);
    put_byte(busy->release[1]);
    get_byte(busy->entered[0]);
    CHECK(atomic_load(&busy->executed) == 2);
    sleep_ms(20);
    CHECK(nzm_runtime_request_stop(busy->runtime) == 0);
    CHECK(nzm_runtime_notify(busy->runtime, NZM_REASON_MANUAL) == -ESHUTDOWN);
    /* Prove request_stop did NOT close the blocked worker FD to force exit. */
    sleep_ms(40);
    CHECK(fcntl(busy->release[0], F_GETFD) >= 0);
    CHECK(atomic_load(&busy->executed) == 2);
    put_byte(busy->release[1]);
    atomic_store(&busy->producer_done, true);
    return NULL;
}

static void test_busy_burst(void)
{
    struct nzm_runtime_options options = test_options();
    struct fake_psi fake;
    struct nzm_psi_adapter adapter;
    struct busy_context busy = { .loop = pthread_self() };
    struct nzm_runtime_executor executor = {
        .owner_context = &busy, .initialize = busy_initialize,
        .execute = busy_execute, .finalize = busy_finalize
    };
    struct nzm_runtime_stats stats;
    pthread_t driver;
    sigset_t original;
    atomic_init(&busy.executed, 0);
    atomic_init(&busy.producer_done, false);
    fake_init(&fake); adapter = fake_adapter(&fake); busy.fake = &fake;
    options.psi.enabled = true; options.psi_adapter = &adapter;
    CHECK(pthread_sigmask(SIG_SETMASK, NULL, &original) == 0);
    gated_pipe(busy.entered); gated_pipe(busy.release);
    CHECK(nzm_runtime_create(&busy.runtime, &options, &executor) == 0);
    /* Options/callback tables are copied, not reloaded or dereferenced later. */
    options.idle_sample_interval_ns = 1;
    adapter.open = NULL;
    CHECK(pthread_create(&driver, NULL, busy_driver, &busy) == 0);
    CHECK(nzm_runtime_run(busy.runtime) == 0);
    CHECK(pthread_join(driver, NULL) == 0);
    CHECK(nzm_runtime_get_stats(busy.runtime, &stats) == 0);
    CHECK(busy.calls == 2 && busy.initialized == 1 && busy.finalized == 1);
    CHECK(busy.reasons[0] == NZM_REASON_STARTUP);
    CHECK((busy.reasons[1] & (NZM_REASON_TIMER | NZM_REASON_MANUAL | NZM_REASON_PSI)) ==
          (NZM_REASON_TIMER | NZM_REASON_MANUAL | NZM_REASON_PSI));
    CHECK(stats.jobs_started == 2 && stats.jobs_completed == 2 && stats.worker_joined);
    CHECK(stats.notifications == 100000 && stats.coalesced >= 99999);
    CHECK(stats.psi_events == 16 && stats.timer_events == 1 && stats.last_result_code == 17);
    CHECK(stats.last_delay_ns == 300 * NS_MS && fake.opens == 1 && fake.closes == 1);
    CHECK(atomic_load(&busy.producer_done));
    CHECK(nzm_runtime_run(busy.runtime) == -EALREADY);
    CHECK(nzm_runtime_notify(busy.runtime, NZM_REASON_MANUAL) == -ESHUTDOWN);
    CHECK(nzm_runtime_request_stop(busy.runtime) == 0);
    verify_restored(&original);
    CHECK(nzm_runtime_destroy(busy.runtime) == 0);
    fake_release(&fake);
    gated_close(busy.entered[0]); gated_close(busy.release[1]);
    passed("100000 concurrent notifications + real PRI + timer coalesce into one pending job; blocked-I/O stop/join");
}

struct simple_context {
    struct nzm_runtime *runtime;
    pthread_t worker;
    unsigned calls, limit;
    uint64_t times[8];
    uint32_t reasons[8];
    int ready[2];
    bool ready_enabled, wait_stop, wait_timeout, check_mask;
    enum nzm_runtime_boundary boundary;
    int action;
    uint64_t wait_elapsed;
    sigset_t original_mask;
};

static int simple_initialize(void *context, const struct nzm_runtime_control *ctl)
{
    struct simple_context *simple = context;
    simple->worker = pthread_self();
    if (simple->check_mask) {
        sigset_t exec_mask, current;
        int number;
        CHECK(nzm_runtime_control_get_exec_mask(ctl, &exec_mask) == 0);
        for (number = 1; number < NSIG; ++number)
            CHECK(sigismember(&exec_mask, number) == sigismember(&simple->original_mask, number));
        CHECK(pthread_sigmask(SIG_SETMASK, NULL, &current) == 0);
        CHECK(sigismember(&current, SIGUSR1) == 1);
        CHECK(sigismember(&exec_mask, SIGUSR1) == 1);
        CHECK(sigismember(&exec_mask, SIGTERM) == 0);
    }
    return 0;
}

static void simple_execute(void *context, const struct nzm_runtime_job *job,
                            const struct nzm_runtime_control *ctl,
                            struct nzm_runtime_result *result)
{
    struct simple_context *simple = context;
    CHECK(pthread_equal(simple->worker, pthread_self()));
    CHECK(simple->calls < 8 && job->sequence == simple->calls + 1);
    simple->times[simple->calls] = monotonic_ns();
    simple->reasons[simple->calls++] = job->reasons;
    if (simple->ready_enabled)
        put_byte(simple->ready[1]);
    if (simple->wait_stop || simple->wait_timeout) {
        struct timespec now, deadline;
        uint64_t start = monotonic_ns();
        CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
        CHECK(nzm_runtime_deadline_after(&now,
              simple->wait_stop ? 30000 * NS_MS : 60 * NS_MS, &deadline) == 0);
        simple->action = nzm_runtime_control_wait_until(ctl, simple->boundary, &deadline);
        simple->wait_elapsed = monotonic_ns() - start;
        if (simple->wait_timeout) {
            CHECK(simple->action == NZM_CONTROL_CONTINUE);
            CHECK(simple->wait_elapsed >= 50 * NS_MS);
            CHECK(nzm_runtime_request_stop(simple->runtime) == 0);
        } else {
            CHECK(simple->action == nzm_runtime_control_check(ctl, simple->boundary));
            CHECK(nzm_runtime_control_check(ctl, NZM_BOUNDARY_OBSERVING) == NZM_CONTROL_STOP_BEFORE_COMMIT);
            CHECK(nzm_runtime_control_check(ctl, NZM_BOUNDARY_PRECOMMIT) == NZM_CONTROL_STOP_BEFORE_COMMIT);
            CHECK(nzm_runtime_control_check(ctl, NZM_BOUNDARY_POSTPRECHARGE) == NZM_CONTROL_STOP_RETAIN_RESERVATION);
            CHECK(nzm_runtime_control_check(ctl, NZM_BOUNDARY_SUBMITTING) == NZM_CONTROL_STOP_RETAIN_RESERVATION);
        }
    }
    result->active = false;
    if (simple->limit && simple->calls >= simple->limit)
        CHECK(nzm_runtime_request_stop(simple->runtime) == 0);
}

static struct nzm_runtime_executor simple_executor(struct simple_context *simple)
{
    return (struct nzm_runtime_executor){
        .owner_context = simple, .initialize = simple_initialize, .execute = simple_execute
    };
}

static void test_init_fallback(void)
{
    const int errors[] = { ENOENT, EOPNOTSUPP, EACCES, EPERM };
    unsigned i;
    for (i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        struct nzm_runtime_options options = test_options();
        struct fake_psi fake;
        struct nzm_psi_adapter adapter;
        struct simple_context simple = { .limit = 3 };
        struct nzm_runtime_executor executor = simple_executor(&simple);
        struct nzm_runtime_stats stats;
        fake_init(&fake); fake.open_error = errors[i]; adapter = fake_adapter(&fake);
        options.psi.enabled = true; options.psi_adapter = &adapter;
        options.idle_sample_interval_ns = 25 * NS_MS;
        CHECK(nzm_runtime_create(&simple.runtime, &options, &executor) == 0);
        CHECK(nzm_runtime_run(simple.runtime) == 0);
        CHECK(nzm_runtime_get_stats(simple.runtime, &stats) == 0);
        CHECK(simple.calls == 3 && stats.timer_events == 2 && stats.psi_fallbacks == 1);
        CHECK(stats.timer_only && stats.psi_error == -errors[i]);
        CHECK(fake.opens == 1 && !fake.writes && !fake.closes);
        CHECK((simple.reasons[1] & (NZM_REASON_TIMER | NZM_REASON_FALLBACK)) ==
              (NZM_REASON_TIMER | NZM_REASON_FALLBACK));
        CHECK(nzm_runtime_destroy(simple.runtime) == 0);
    }
    passed("ENOENT/EOPNOTSUPP/EACCES/EPERM initialization falls back to timer without reopen");
}

struct fallback_driver_context {
    struct simple_context *simple;
    struct fake_psi *fake;
    bool hup;
};

static void *fallback_driver(void *context)
{
    struct fallback_driver_context *driver = context;
    get_byte(driver->simple->ready[0]);
    if (driver->hup) {
        /* shutdown creates genuine HUP without cross-thread closing runtime FD. */
        CHECK(shutdown(driver->fake->peer, SHUT_RDWR) == 0);
    } else {
        fake_priority(driver->fake);
    }
    return NULL;
}

static void test_runtime_fallback(void)
{
    const int errors[] = { ENOENT, EOPNOTSUPP, EACCES, EPERM, EIO };
    unsigned i;
    for (i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        struct nzm_runtime_options options = test_options();
        struct fake_psi fake;
        struct nzm_psi_adapter adapter;
        struct simple_context simple = { .limit = 4, .ready_enabled = true };
        struct nzm_runtime_executor executor = simple_executor(&simple);
        struct nzm_runtime_stats stats;
        struct fallback_driver_context driver_context = { .simple = &simple, .fake = &fake, .hup = i == 4 };
        pthread_t driver;
        fake_init(&fake); fake.ack_error = errors[i]; adapter = fake_adapter(&fake);
        options.psi.enabled = true; options.psi_adapter = &adapter;
        options.idle_sample_interval_ns = 35 * NS_MS;
        gated_pipe(simple.ready);
        CHECK(nzm_runtime_create(&simple.runtime, &options, &executor) == 0);
        CHECK(pthread_create(&driver, NULL, fallback_driver, &driver_context) == 0);
        CHECK(nzm_runtime_run(simple.runtime) == 0);
        CHECK(pthread_join(driver, NULL) == 0);
        CHECK(nzm_runtime_get_stats(simple.runtime, &stats) == 0);
        CHECK(simple.calls == 4 && stats.timer_events >= 2 && stats.psi_fallbacks == 1);
        CHECK(stats.timer_only && stats.psi_error == -errors[i]);
        CHECK(fake.opens == 1 && fake.writes == 1 && fake.closes == 1);
        CHECK(atomic_load(&fake.acks) == (i == 4 ? 0U : 1U));
        CHECK(nzm_runtime_destroy(simple.runtime) == 0);
        fake_release(&fake);
        gated_close(simple.ready[0]); gated_close(simple.ready[1]);
    }
    passed("runtime PSI errors and genuine HUP close once, never re-register, continue timer callbacks");
}

static void *stop_driver(void *context)
{
    struct simple_context *simple = context;
    get_byte(simple->ready[0]);
    sleep_ms(20);
    CHECK(nzm_runtime_request_stop(simple->runtime) == 0);
    return NULL;
}

static void test_stop_waits(void)
{
    enum nzm_runtime_boundary boundaries[] = {
        NZM_BOUNDARY_OBSERVING, NZM_BOUNDARY_PRECOMMIT,
        NZM_BOUNDARY_POSTPRECHARGE, NZM_BOUNDARY_SUBMITTING
    };
    unsigned i;
    for (i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); ++i) {
        struct nzm_runtime_options options = test_options();
        struct simple_context simple = { .ready_enabled = true, .wait_stop = true,
                                         .boundary = boundaries[i] };
        struct nzm_runtime_executor executor = simple_executor(&simple);
        struct nzm_runtime_stats stats;
        pthread_t driver;
        gated_pipe(simple.ready);
        CHECK(nzm_runtime_create(&simple.runtime, &options, &executor) == 0);
        CHECK(pthread_create(&driver, NULL, stop_driver, &simple) == 0);
        CHECK(nzm_runtime_run(simple.runtime) == 0);
        CHECK(pthread_join(driver, NULL) == 0);
        CHECK(nzm_runtime_get_stats(simple.runtime, &stats) == 0);
        CHECK(stats.worker_joined && simple.calls == 1);
        CHECK(simple.action == (i < 2 ? NZM_CONTROL_STOP_BEFORE_COMMIT : NZM_CONTROL_STOP_RETAIN_RESERVATION));
        CHECK(simple.wait_elapsed < 4000 * NS_MS);
        CHECK(nzm_runtime_destroy(simple.runtime) == 0);
        gated_close(simple.ready[0]); gated_close(simple.ready[1]);
    }
    {
        struct nzm_runtime_options options = test_options();
        struct simple_context simple = { .wait_timeout = true, .boundary = NZM_BOUNDARY_OBSERVING };
        struct nzm_runtime_executor executor = simple_executor(&simple);
        CHECK(nzm_runtime_create(&simple.runtime, &options, &executor) == 0);
        CHECK(nzm_runtime_run(simple.runtime) == 0);
        CHECK(simple.wait_elapsed >= 50 * NS_MS && simple.wait_elapsed < 4000 * NS_MS);
        CHECK(nzm_runtime_destroy(simple.runtime) == 0);
    }
    passed("real monotonic timed waits: expiration and cooperative observer/precommit/postprecharge/submitting stop");
}

struct schedule_context {
    struct simple_context simple;
};

static void schedule_execute(void *context, const struct nzm_runtime_job *job,
                              const struct nzm_runtime_control *ctl,
                              struct nzm_runtime_result *result)
{
    struct schedule_context *schedule = context;
    struct simple_context *simple = &schedule->simple;
    simple_execute(simple, job, ctl, result);
    if (simple->calls == 1) {
        result->active = true;
        result->next_delay_ns = 1; /* clamped to 20ms active minimum */
    } else if (simple->calls == 2) {
        result->active = false;
        result->next_delay_ns = 120 * NS_MS; /* larger than 80ms idle minimum */
    } else {
        result->next_delay_ns = UINT64_MAX; /* clamp maximum fallback */
    }
}

static void test_schedule(void)
{
    struct nzm_runtime_options options = test_options();
    struct schedule_context schedule = { .simple = { .limit = 4 } };
    struct nzm_runtime_executor executor = {
        .owner_context = &schedule, .initialize = simple_initialize, .execute = schedule_execute
    };
    struct nzm_runtime_stats stats;
    options.max_fallback_interval_ns = 160 * NS_MS;
    CHECK(nzm_runtime_create(&schedule.simple.runtime, &options, &executor) == 0);
    CHECK(nzm_runtime_run(schedule.simple.runtime) == 0);
    CHECK(nzm_runtime_get_stats(schedule.simple.runtime, &stats) == 0);
    CHECK(schedule.simple.times[1] - schedule.simple.times[0] >= 18 * NS_MS);
    CHECK(schedule.simple.times[2] - schedule.simple.times[1] >= 115 * NS_MS);
    CHECK(schedule.simple.times[3] - schedule.simple.times[2] >= 155 * NS_MS);
    CHECK(stats.timer_events == 3 && stats.jobs_completed == 4 && stats.last_delay_ns == 160 * NS_MS);
    CHECK(nzm_runtime_destroy(schedule.simple.runtime) == 0);
    passed("one-shot deadlines use active/idle minima, callback delay and max fallback (no fixed scanning)");
}

struct signal_driver_context { struct simple_context *simple; int stop_signal; };

static void *signal_driver(void *context)
{
    struct signal_driver_context *driver = context;
    get_byte(driver->simple->ready[0]);
    CHECK(kill(getpid(), SIGHUP) == 0);
    sleep_ms(25);
    CHECK(kill(getpid(), driver->stop_signal) == 0);
    return NULL;
}

static void test_signals_mask(void)
{
    const int stop_signals[] = { SIGTERM, SIGINT };
    sigset_t saved, unrelated;
    struct sigaction before, after;
    unsigned i;
    CHECK(pthread_sigmask(SIG_SETMASK, NULL, &saved) == 0);
    unrelated = saved;
    CHECK(sigaddset(&unrelated, SIGUSR1) == 0);
    CHECK(pthread_sigmask(SIG_SETMASK, &unrelated, NULL) == 0);
    CHECK(sigaction(SIGCHLD, NULL, &before) == 0);
    for (i = 0; i < 2; ++i) {
        struct nzm_runtime_options options = test_options();
        struct simple_context simple = { .ready_enabled = true, .wait_stop = true,
            .check_mask = true, .boundary = NZM_BOUNDARY_OBSERVING, .original_mask = unrelated };
        struct nzm_runtime_executor executor = simple_executor(&simple);
        struct nzm_runtime_stats stats;
        struct signal_driver_context driver_context = { .simple = &simple, .stop_signal = stop_signals[i] };
        pthread_t driver;
        gated_pipe(simple.ready);
        CHECK(nzm_runtime_create(&simple.runtime, &options, &executor) == 0);
        CHECK(pthread_create(&driver, NULL, signal_driver, &driver_context) == 0);
        CHECK(nzm_runtime_run(simple.runtime) == 0);
        CHECK(pthread_join(driver, NULL) == 0);
        CHECK(nzm_runtime_get_stats(simple.runtime, &stats) == 0);
        CHECK(stats.hup_unsupported == 1 && stats.jobs_started == 1 && simple.calls == 1);
        CHECK(simple.action == NZM_CONTROL_STOP_BEFORE_COMMIT);
        verify_restored(&unrelated);
        CHECK(nzm_runtime_destroy(simple.runtime) == 0);
        gated_close(simple.ready[0]); gated_close(simple.ready[1]);
    }
    CHECK(sigaction(SIGCHLD, NULL, &after) == 0);
    CHECK(before.sa_handler == after.sa_handler && before.sa_flags == after.sa_flags);
    CHECK(pthread_sigmask(SIG_SETMASK, &saved, NULL) == 0);
    passed("real signalfd HUP unsupported, TERM/INT stop, exact original SIGUSR1 mask, no SIGCHLD handler");
}

static int fail_initialize(void *context, const struct nzm_runtime_control *ctl)
{
    unsigned *calls = context;
    (void)ctl;
    (*calls)++;
    return -EIO;
}

static void fail_finalize(void *context, const struct nzm_runtime_control *ctl)
{
    unsigned *calls = context;
    CHECK(nzm_runtime_control_stop_requested(ctl));
    (*calls)++;
}

static void test_early_stop_init_failure(void)
{
    struct nzm_runtime_options options = test_options();
    struct nzm_runtime *runtime = NULL;
    struct nzm_runtime_stats stats;
    unsigned calls = 0;
    struct nzm_runtime_executor executor = { .owner_context = &calls,
        .initialize = fail_initialize, .execute = no_execute, .finalize = fail_finalize };
    CHECK(nzm_runtime_create(&runtime, &options, &executor) == 0);
    CHECK(nzm_runtime_run(runtime) == -EIO);
    CHECK(nzm_runtime_get_stats(runtime, &stats) == 0);
    CHECK(calls == 2 && stats.jobs_completed == 0 && stats.worker_error == -EIO && stats.worker_joined);
    CHECK(nzm_runtime_destroy(runtime) == 0);
    calls = 0;
    CHECK(nzm_runtime_create(&runtime, &options, &executor) == 0);
    CHECK(nzm_runtime_request_stop(runtime) == 0);
    CHECK(nzm_runtime_run(runtime) == 0);
    CHECK(nzm_runtime_get_stats(runtime, &stats) == 0);
    CHECK(calls == 1 && stats.jobs_started == 0 && stats.worker_joined);
    CHECK(nzm_runtime_destroy(runtime) == 0);
    passed("worker init failure and pre-run stop preserve cleanup/join ordering");
}

static void *creation_failure_producer(void *context)
{
    struct close_fault *fault = context;
    unsigned i;
    CHECK(pthread_mutex_lock(&fault->mutex) == 0);
    while (!fault->in_reuse_window)
        CHECK(pthread_cond_wait(&fault->cond, &fault->mutex) == 0);
    CHECK(pthread_mutex_unlock(&fault->mutex) == 0);
    for (i = 0; i < 10000; ++i) {
        CHECK(nzm_runtime_request_stop(fault->runtime) == 0);
        CHECK(nzm_runtime_notify(fault->runtime, NZM_REASON_MANUAL) == -ESHUTDOWN);
    }
    CHECK(pthread_mutex_lock(&fault->mutex) == 0);
    fault->producer_done = true;
    CHECK(pthread_cond_broadcast(&fault->cond) == 0);
    CHECK(pthread_mutex_unlock(&fault->mutex) == 0);
    return NULL;
}

static void test_worker_creation_failure_race(void)
{
    unsigned repetition;
    for (repetition = 0; repetition < 32; ++repetition) {
        struct nzm_runtime_options options = test_options();
        struct nzm_runtime_executor executor = { .execute = no_execute };
        struct nzm_runtime_stats stats;
        struct close_fault fault = { .target_fd = -1, .replacement_fd = -1 };
        struct nzm_spawn_fd_guard guard = NZM_SPAWN_FD_GUARD_INIT;
        pthread_t producer;
        sigset_t original;
        CHECK(pthread_mutex_init(&fault.mutex, NULL) == 0);
        CHECK(pthread_cond_init(&fault.cond, NULL) == 0);
        CHECK(nzm_spawn_fd_enter(&guard) == 0);
        fault.replacement_fd = __real_eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        CHECK(fault.replacement_fd >= 0);
        nzm_spawn_fd_leave(&guard);
        CHECK(pthread_sigmask(SIG_SETMASK, NULL, &original) == 0);
        atomic_store(&capture_eventfd, true);
        CHECK(nzm_runtime_create(&fault.runtime, &options, &executor) == 0);
        atomic_store(&capture_eventfd, false);
        fault.target_fd = atomic_load(&captured_eventfd);
        CHECK(fault.target_fd >= 0 && fault.target_fd != fault.replacement_fd);
        CHECK(pthread_create(&producer, NULL, creation_failure_producer, &fault) == 0);
        atomic_store(&active_close_fault, &fault);
        atomic_store(&fail_next_thread, true);
        CHECK(nzm_runtime_run(fault.runtime) == -EAGAIN);
        CHECK(!atomic_load(&fail_next_thread));
        CHECK(pthread_join(producer, NULL) == 0);
        atomic_store(&active_close_fault, NULL);
        CHECK(nzm_runtime_get_stats(fault.runtime, &stats) == 0);
        CHECK(stats.jobs_started == 0 && stats.jobs_completed == 0 && !stats.worker_joined);
        CHECK(nzm_runtime_request_stop(fault.runtime) == 0);
        verify_restored(&original);
        CHECK(nzm_runtime_destroy(fault.runtime) == 0);
        gated_close(fault.replacement_fd);
        CHECK(pthread_cond_destroy(&fault.cond) == 0);
        CHECK(pthread_mutex_destroy(&fault.mutex) == 0);
    }
    passed("pthread_create EAGAIN: 32 deterministic close/reuse windows with 320000 concurrent stop requests, no late kicks");
}

struct owner_context { struct simple_context simple; unsigned reaped; };

static void owner_execute(void *context, const struct nzm_runtime_job *job,
                           const struct nzm_runtime_control *ctl,
                           struct nzm_runtime_result *result)
{
    struct owner_context *owner = context;
    struct nzm_spawn_fd_guard guard = NZM_SPAWN_FD_GUARD_INIT;
    pid_t pid, got;
    int status;
    CHECK(nzm_spawn_fd_enter(&guard) == 0);
    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0)
        _exit(23); /* async-signal-safe only; no inherited gate operations */
    nzm_spawn_fd_leave(&guard);
    do {
        got = waitpid(pid, &status, 0);
    } while (got < 0 && errno == EINTR);
    CHECK(got == pid && WIFEXITED(status) && WEXITSTATUS(status) == 23);
    owner->reaped++;
    simple_execute(&owner->simple, job, ctl, result);
}

static void test_child_owner(void)
{
    struct nzm_runtime_options options = test_options();
    struct owner_context owner = { .simple = { .limit = 1 } };
    struct nzm_runtime_executor executor = {
        .owner_context = &owner, .initialize = simple_initialize, .execute = owner_execute
    };
    CHECK(nzm_runtime_create(&owner.simple.runtime, &options, &executor) == 0);
    CHECK(nzm_runtime_run(owner.simple.runtime) == 0);
    CHECK(owner.reaped == 1);
    CHECK(nzm_runtime_destroy(owner.simple.runtime) == 0);
    passed("worker is sole owner/reaper of its test child; runtime never waitpid/reaps");
}

int main(void)
{
    test_validation_clock();
    test_psi_direct();
    test_busy_burst();
    test_init_fallback();
    test_runtime_fallback();
    test_stop_waits();
    test_schedule();
    test_signals_mask();
    test_early_stop_init_failure();
    test_worker_creation_failure_race();
    test_child_owner();
    printf("runtime tests: %u groups PASS\n", test_count);
    return 0;
}
