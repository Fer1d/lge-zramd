/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "request_backend.h"
#include "uapi/linux/zram.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysmacros.h>

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
		exit(1); \
	} \
} while (0)

static unsigned int test_count;

struct fake {
	struct rb_exec_request request;
	struct zram_wb_exec exec_reply;
	struct zram_wb_status status_reply;
	int open_errno, stat_errno, label_errno, ioctl_errno, close_errno;
	int ioctl_return, fd, expected_access;
	mode_t mode;
	dev_t rdev;
	char label[128];
	ssize_t label_length;
	bool raw_label_length;
	bool exec_operation;
	bool corrupt;
	size_t corrupt_offset;
	int opens, stats, labels, ioctls, closes, active;
	unsigned int serial;
	unsigned int open_order, stat_order, label_order, ioctl_order, close_order;
};

static void check_fd(struct fake *f, int fd)
{
	CHECK(f->active == 1);
	CHECK(fd == f->fd);
}

static int fake_open(void *context, const char *path, int flags)
{
	struct fake *f = context;
	f->opens++;
	f->open_order = ++f->serial;
	CHECK(strcmp(path, RB_DEVICE_PATH) == 0);
	CHECK(flags == (f->expected_access | O_CLOEXEC | O_NOFOLLOW));
	CHECK(f->active == 0);
	if (f->open_errno) {
		errno = f->open_errno;
		return -1;
	}
	f->active = 1;
	return f->fd;
}

static int fake_stat(void *context, int fd, struct stat *st)
{
	struct fake *f = context;
	check_fd(f, fd);
	f->stats++;
	f->stat_order = ++f->serial;
	if (f->stat_errno) {
		errno = f->stat_errno;
		return -1;
	}
	memset(st, 0, sizeof(*st));
	st->st_mode = f->mode;
	st->st_rdev = f->rdev;
	return 0;
}

static ssize_t fake_label(void *context, int fd, const char *name,
			  void *value, size_t size)
{
	struct fake *f = context;
	size_t length;
	check_fd(f, fd);
	f->labels++;
	f->label_order = ++f->serial;
	CHECK(strcmp(name, "security.selinux") == 0);
	if (f->label_errno) {
		errno = f->label_errno;
		return -1;
	}
	if (f->raw_label_length)
		return f->label_length;
	length = (size_t)f->label_length;
	CHECK(length <= sizeof(f->label));
	if (length > size) {
		errno = ERANGE;
		return -1;
	}
	memcpy(value, f->label, length);
	return f->label_length;
}

static int fake_ioctl(void *context, int fd, unsigned long command, void *arg)
{
	struct fake *f = context;
	check_fd(f, fd);
	f->ioctls++;
	f->ioctl_order = ++f->serial;
	CHECK(f->label_order > f->stat_order);
	CHECK(f->stat_order > f->open_order);
	if (f->exec_operation) {
		struct zram_wb_exec expected, *reply = arg;
		memset(&expected, 0, sizeof(expected));
		expected.size = (uint32_t)sizeof(expected);
		expected.version = ZRAM_WB_ABI_VERSION;
		expected.mode = ZRAM_WB_MODE_FILTERED_IDLE;
		expected.request_id = f->request.request_id;
		expected.expected_instance = f->request.expected.instance;
		expected.expected_generation = f->request.expected.generation;
		expected.max_attempt_pages = f->request.max_attempt_pages;
		expected.max_installed_pages = f->request.max_installed_pages;
		expected.idle_count = f->request.filtered_idle_count;
		CHECK(command == (unsigned long)ZRAM_IOC_WB_EXEC);
		/* Verifies every output/reserved byte was zero, and no unit conversion. */
		CHECK(memcmp(reply, &expected, sizeof(expected)) == 0);
		memcpy((unsigned char *)reply + offsetof(struct zram_wb_exec, scanned_pages),
		       (unsigned char *)&f->exec_reply + offsetof(struct zram_wb_exec, scanned_pages),
		       sizeof(*reply) - offsetof(struct zram_wb_exec, scanned_pages));
	} else {
		struct zram_wb_status expected;
		memset(&expected, 0, sizeof(expected));
		expected.size = (uint32_t)sizeof(expected);
		expected.version = ZRAM_WB_ABI_VERSION;
		CHECK(command == (unsigned long)ZRAM_IOC_WB_STATUS);
		CHECK(memcmp(arg, &expected, sizeof(expected)) == 0);
		memcpy(arg, &f->status_reply, sizeof(f->status_reply));
	}
	if (f->corrupt) {
		CHECK(f->corrupt_offset < (f->exec_operation ?
		      sizeof(struct zram_wb_exec) : sizeof(struct zram_wb_status)));
		((unsigned char *)arg)[f->corrupt_offset] ^= 1;
	}
	/* A failure may follow effects and even an apparently complete copyout. */
	if (f->ioctl_errno) {
		errno = f->ioctl_errno;
		return -1;
	}
	return f->ioctl_return;
}

static int fake_close(void *context, int fd)
{
	struct fake *f = context;
	check_fd(f, fd);
	f->closes++;
	f->close_order = ++f->serial;
	f->active = 0;
	if (f->close_errno) {
		errno = f->close_errno;
		return -1;
	}
	/* Deliberately clobber errno: ioctl error must have been saved already. */
	errno = EBUSY;
	return 0;
}

static const struct rb_syscalls fake_syscalls = {
	.open_fn = fake_open,
	.fstat_fn = fake_stat,
	.fgetxattr_fn = fake_label,
	.ioctl_fn = fake_ioctl,
	.close_fn = fake_close,
};

static struct rb_backend setup(struct fake *f, bool exec_operation)
{
	struct rb_backend backend;
	memset(f, 0, sizeof(*f));
	f->fd = 17;
	f->exec_operation = exec_operation;
	f->expected_access = exec_operation ? O_RDWR : O_RDONLY;
	f->mode = S_IFBLK | 0600;
	f->rdev = makedev(252, 0);
	memcpy(f->label, RB_DEVICE_LABEL, sizeof(RB_DEVICE_LABEL));
	f->label_length = (ssize_t)sizeof(RB_DEVICE_LABEL);
	f->request.expected.instance = UINT64_C(0xf23456789abcdef0);
	f->request.expected.generation = 123;
	f->request.request_id = UINT64_C(0xdeadbeef12345678);
	f->request.max_attempt_pages = 16;
	f->request.max_installed_pages = 8;
	f->request.filtered_idle_count = 3;
	f->exec_reply.scanned_pages = 20;
	f->exec_reply.selected_pages = 12;
	f->exec_reply.submitted_pages = 10;
	f->exec_reply.installed_pages = 8;
	f->exec_reply.invalidated_pages = 2;
	f->exec_reply.duration_ns = 321;
	f->exec_reply.stop_reason = ZRAM_WB_STOP_INSTALLED_LIMIT;
	f->status_reply.size = (uint32_t)sizeof(f->status_reply);
	f->status_reply.version = ZRAM_WB_ABI_VERSION;
	f->status_reply.capabilities = ZRAM_WB_CAP_EXEC | ZRAM_WB_CAP_STATUS |
		ZRAM_WB_CAP_FILTERED_IDLE | ZRAM_WB_CAP_DUAL_LIMIT;
	f->status_reply.instance = f->request.expected.instance;
	f->status_reply.generation = f->request.expected.generation;
	f->status_reply.disksize_bytes = UINT64_C(1) << 33;
	f->status_reply.page_size = 4096;
	f->status_reply.state_flags = ZRAM_WB_STATE_INITIALIZED |
		ZRAM_WB_STATE_BACKING | ZRAM_WB_STATE_GLOBAL_LIMIT;
	/* Intentionally not a coherent tuple; all samples must survive unchanged. */
	f->status_reply.wb_limit_remaining_4k = UINT64_MAX;
	f->status_reply.pages_stored = 1;
	f->status_reply.same_pages = 9;
	f->status_reply.huge_pages = 10;
	f->status_reply.lowratio_pages = 11;
	f->status_reply.bd_count_pages = 12;
	f->status_reply.bd_reads_pages = 13;
	f->status_reply.bd_writes_pages = 14;
	f->status_reply.compr_data_bytes = UINT64_MAX - 1;
	f->status_reply.mem_used_bytes = 42;
	backend.expected_rdev = f->rdev;
	backend.syscalls = &fake_syscalls;
	backend.syscall_context = f;
	return backend;
}

static void finished(struct fake *f)
{
	CHECK(f->opens == 1);
	CHECK(f->stats == 1);
	CHECK(f->labels == 1);
	CHECK(f->ioctls == 1); /* Includes EFAULT/EINTR: never any replay/probe. */
	CHECK(f->closes == 1);
	CHECK(f->active == 0);
	CHECK(f->close_order > f->ioctl_order);
	test_count++;
}

static void unknown(struct rb_exec_result r)
{
	CHECK(!r.known);
	CHECK(r.no_replay);
	CHECK(r.transport.syscall_called);
	CHECK(r.transport.outcome == RB_TRANSPORT_OUTCOME_UNKNOWN);
	CHECK(r.op_errno == 0 && r.error_stage == 0 && r.stop_reason == 0);
	CHECK(r.request_id == 0 && r.expected.instance == 0 && r.expected.generation == 0);
	CHECK(r.counters.scanned_pages == 0 && r.counters.selected_pages == 0);
	CHECK(r.counters.submitted_pages == 0 && r.counters.installed_pages == 0);
	CHECK(r.counters.invalidated_pages == 0 && r.duration_ns == 0);
}

static void test_status(void)
{
	struct fake f;
	struct rb_backend b = setup(&f, false);
	struct rb_status_result r = rb_status(&b);
	CHECK(r.known && r.ready && r.selection == RB_BACKEND_REQUEST);
	CHECK(r.transport.outcome == RB_TRANSPORT_RETURNED);
	CHECK(r.transport.syscall_called && !r.transport.error_no && !r.transport.close_errno);
	CHECK(r.statistics_consistency == RB_STATISTICS_SAMPLED);
	CHECK(r.identity.instance == f.status_reply.instance);
	CHECK(r.identity.generation == f.status_reply.generation);
	CHECK(r.disksize_bytes == f.status_reply.disksize_bytes);
	CHECK(r.page_size == 4096 && r.state_flags == f.status_reply.state_flags);
	CHECK(r.capabilities == f.status_reply.capabilities);
	CHECK(r.sampled.wb_limit_remaining_4k == UINT64_MAX);
	CHECK(r.sampled.pages_stored == 1 && r.sampled.same_pages == 9);
	CHECK(r.sampled.huge_pages == 10 && r.sampled.lowratio_pages == 11);
	CHECK(r.sampled.bd_count_pages == 12 && r.sampled.bd_reads_pages == 13);
	CHECK(r.sampled.bd_writes_pages == 14 && r.sampled.compr_data_bytes == UINT64_MAX - 1);
	CHECK(r.sampled.mem_used_bytes == 42);
	finished(&f);
	for (unsigned int state = 0; state < 4; state++) {
		b = setup(&f, false);
		f.status_reply.state_flags = state;
		r = rb_status(&b);
		CHECK(r.known && r.ready == (state == 3));
		CHECK(r.selection == RB_BACKEND_REQUEST);
		finished(&f);
	}
	for (unsigned int bit = 0; bit < 4; bit++) {
		b = setup(&f, false);
		f.status_reply.capabilities &= ~(UINT64_C(1) << bit);
		r = rb_status(&b);
		CHECK(r.known && r.selection == RB_BACKEND_UNAVAILABLE);
		finished(&f);
	}
	b = setup(&f, false);
	f.status_reply.capabilities |= UINT64_C(1) << 63;
	f.status_reply.state_flags |= UINT32_C(1) << 31;
	r = rb_status(&b);
	CHECK(r.known && r.selection == RB_BACKEND_REQUEST);
	CHECK(r.capabilities == f.status_reply.capabilities && r.state_flags == f.status_reply.state_flags);
	finished(&f);
}

static void test_syscall_errors(void)
{
	const int errors[] = {ENOTTY, EACCES, EPERM, EBADF, EINVAL, ENODEV,
		ESTALE, EIO, EFAULT, EINTR, ENOMEM, EAGAIN, EPROTONOSUPPORT};
	for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
		struct fake f;
		struct rb_backend b = setup(&f, false);
		struct rb_status_result s;
		struct rb_exec_result r;
		f.ioctl_errno = errors[i];
		s = rb_status(&b);
		CHECK(!s.known && s.transport.syscall_called);
		CHECK(s.transport.outcome == RB_TRANSPORT_FAILED);
		CHECK(s.transport.failure_stage == RB_FAILURE_IOCTL && s.transport.error_no == errors[i]);
		CHECK(s.selection == (errors[i] == ENOTTY ? RB_BACKEND_LEGACY : RB_BACKEND_UNAVAILABLE));
		CHECK(s.statistics_consistency == RB_STATISTICS_UNAVAILABLE);
		finished(&f);
		b = setup(&f, true);
		f.ioctl_errno = errors[i];
		r = rb_exec(&b, &f.request);
		unknown(r); /* Even when fake ioctl wrote a complete successful reply. */
		CHECK(r.transport.failure_stage == RB_FAILURE_IOCTL && r.transport.error_no == errors[i]);
		finished(&f);
	}
	for (unsigned int op = 0; op < 2; op++) {
		struct fake f;
		struct rb_backend b = setup(&f, op != 0);
		f.ioctl_return = 1; /* UAPI demands exactly zero. */
		if (op) {
			struct rb_exec_result r = rb_exec(&b, &f.request);
			unknown(r);
			CHECK(r.transport.error_no == EPROTO);
		} else {
			struct rb_status_result s = rb_status(&b);
			CHECK(!s.known && s.selection == RB_BACKEND_UNAVAILABLE);
			CHECK(s.transport.error_no == EPROTO);
		}
		finished(&f);
	}
}

static void test_preflight(void)
{
	/* Each failure is tested for both operations, before any ioctl is called. */
	for (unsigned int op = 0; op < 2; op++) {
		for (unsigned int kind = 0; kind < 18; kind++) {
			struct fake f;
			struct rb_backend b = setup(&f, op != 0);
			struct rb_transport t;
			enum rb_failure_stage stage = RB_FAILURE_LABEL;
			int err = EACCES;
			switch (kind) {
			case 0: f.open_errno = ENOTTY; stage = RB_FAILURE_OPEN; err = ENOTTY; break;
			case 1: f.open_errno = ELOOP; stage = RB_FAILURE_OPEN; err = ELOOP; break;
			case 2: f.open_errno = EACCES; stage = RB_FAILURE_OPEN; break;
			case 3: f.open_errno = EINTR; stage = RB_FAILURE_OPEN; err = EINTR; break;
			case 4: f.stat_errno = EBADF; stage = RB_FAILURE_FSTAT; err = EBADF; break;
			case 5: f.mode = S_IFREG | 0600; stage = RB_FAILURE_DEVICE; err = ENODEV; break;
			case 6: f.rdev = makedev(252, 1); stage = RB_FAILURE_DEVICE; err = ENODEV; break;
			case 7: f.mode = S_IFCHR | 0600; stage = RB_FAILURE_DEVICE; err = ENODEV; break;
			case 8: f.label_errno = ENOTTY; err = ENOTTY; break;
			case 9: f.label_errno = ENODATA; err = ENODATA; break;
			case 10: f.label_errno = EOPNOTSUPP; err = EOPNOTSUPP; break;
			case 11: memcpy(f.label, "u:object_r:block_device:s0", 25); f.label_length = 25; break;
			case 12: f.label_length = 0; break;
			case 13: f.label_length--; f.label[5] = '\0'; break;
			case 14: f.label[sizeof(RB_DEVICE_LABEL) - 1] = 'X'; break;
			case 15: f.label_length++; break; /* Extra NUL is not an exact label. */
			case 16: f.raw_label_length = true; f.label_length = 1024; break;
			case 17: f.label_errno = ERANGE; err = ERANGE; break;
			}
			if (op) {
				struct rb_exec_result r = rb_exec(&b, &f.request);
				CHECK(!r.known && !r.no_replay);
				t = r.transport;
			} else {
				struct rb_status_result r = rb_status(&b);
				CHECK(!r.known && r.selection == RB_BACKEND_UNAVAILABLE);
				t = r.transport;
			}
			CHECK(!t.syscall_called && t.outcome == RB_TRANSPORT_NOT_CALLED);
			CHECK(t.failure_stage == stage && t.error_no == err);
			CHECK(f.opens == 1 && f.ioctls == 0 && f.active == 0);
			CHECK(f.closes == (f.open_errno ? 0 : 1));
			CHECK(f.stats == (f.open_errno ? 0 : 1));
			CHECK(f.labels == (kind >= 8 ? 1 : 0));
			test_count++;
		}
		/* Non-NUL xattrs and fd 0 are also legal. */
		struct fake f;
		struct rb_backend b = setup(&f, op != 0);
		f.label_length--;
		f.fd = 0;
		if (op)
			CHECK(rb_exec(&b, &f.request).known);
		else
			CHECK(rb_status(&b).known);
		finished(&f);
	}
}

static void test_input(void)
{
	struct fake f;
	struct rb_backend b = setup(&f, true);
	struct rb_exec_result r;
	CHECK(rb_status(NULL).transport.failure_stage == RB_FAILURE_INPUT);
	CHECK(rb_exec(NULL, &f.request).transport.failure_stage == RB_FAILURE_INPUT);
	CHECK(rb_exec(&b, NULL).transport.failure_stage == RB_FAILURE_INPUT);
	for (unsigned int kind = 0; kind < 3; kind++) {
		b = setup(&f, true);
		if (kind == 0) f.request.request_id = 0;
		if (kind == 1) f.request.filtered_idle_count = 11;
		if (kind == 2) f.request.max_installed_pages = f.request.max_attempt_pages + 1;
		r = rb_exec(&b, &f.request);
		CHECK(!r.known && !r.no_replay && !r.transport.syscall_called);
		CHECK(r.transport.failure_stage == RB_FAILURE_INPUT && r.transport.error_no == EINVAL);
		CHECK(f.opens == 0);
		test_count++;
	}
	for (unsigned int kind = 0; kind < 5; kind++) {
		struct rb_syscalls incomplete = fake_syscalls;
		b = setup(&f, true);
		if (kind == 0) incomplete.open_fn = NULL;
		if (kind == 1) incomplete.fstat_fn = NULL;
		if (kind == 2) incomplete.fgetxattr_fn = NULL;
		if (kind == 3) incomplete.ioctl_fn = NULL;
		if (kind == 4) incomplete.close_fn = NULL;
		b.syscalls = &incomplete;
		CHECK(rb_exec(&b, &f.request).transport.failure_stage == RB_FAILURE_INPUT);
		CHECK(rb_status(&b).transport.failure_stage == RB_FAILURE_INPUT);
		CHECK(f.opens == 0);
		test_count++;
	}
}

static void test_known_results(void)
{
	struct fake f;
	struct rb_backend b = setup(&f, true);
	struct rb_exec_result r = rb_exec(&b, &f.request);
	CHECK(r.known && r.no_replay && r.transport.syscall_called);
	CHECK(r.transport.outcome == RB_TRANSPORT_RETURNED && r.transport.error_no == 0);
	CHECK(r.op_errno == 0 && r.error_stage == 0 && r.stop_reason == ZRAM_WB_STOP_INSTALLED_LIMIT);
	CHECK(r.request_id == f.request.request_id);
	CHECK(r.expected.instance == f.request.expected.instance && r.expected.generation == f.request.expected.generation);
	CHECK(r.counters.scanned_pages == 20 && r.counters.selected_pages == 12);
	CHECK(r.counters.submitted_pages == 10 && r.counters.installed_pages == 8);
	CHECK(r.counters.invalidated_pages == 2 && r.duration_ns == 321);
	finished(&f);
	const struct { uint32_t stop, stage; int32_t error; bool partial; } cases[] = {
		{ZRAM_WB_STOP_STALE, ZRAM_WB_STAGE_NONE, -ESTALE, false},
		{ZRAM_WB_STOP_NOT_READY, ZRAM_WB_STAGE_NONE, -ENODEV, false},
		{ZRAM_WB_STOP_ERROR, ZRAM_WB_STAGE_ALLOC, -ENOMEM, false},
		{ZRAM_WB_STOP_ERROR, ZRAM_WB_STAGE_SNAPSHOT_READ, -EIO, true},
		{ZRAM_WB_STOP_ERROR, ZRAM_WB_STAGE_SNAPSHOT_READ, -ENOMEM, true},
		{ZRAM_WB_STOP_ERROR, ZRAM_WB_STAGE_SNAPSHOT_READ, -EILSEQ, true},
		{ZRAM_WB_STOP_ERROR, ZRAM_WB_STAGE_BIO_BUILD, -EIO, true},
		{ZRAM_WB_STOP_ERROR, ZRAM_WB_STAGE_IO, -EIO, true},
		{ZRAM_WB_STOP_ERROR, ZRAM_WB_STAGE_IO, -ETIMEDOUT, true},
		{ZRAM_WB_STOP_ERROR, ZRAM_WB_STAGE_IO, -ENOSPC, true},
		{ZRAM_WB_STOP_ERROR, ZRAM_WB_STAGE_NONE, -EOVERFLOW, false},
		{ZRAM_WB_STOP_INTERRUPTED, ZRAM_WB_STAGE_NONE, -EINTR, true},
		{ZRAM_WB_STOP_BACKING_FULL, ZRAM_WB_STAGE_NONE, -ENOSPC, true},
		{ZRAM_WB_STOP_GLOBAL_LIMIT, ZRAM_WB_STAGE_NONE, -EIO, true},
		{ZRAM_WB_STOP_GLOBAL_LIMIT, ZRAM_WB_STAGE_NONE, -EIO, false},
		{ZRAM_WB_STOP_COMPLETE, ZRAM_WB_STAGE_NONE, 0, true},
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		b = setup(&f, true);
		if (!cases[i].partial)
			memset(&f.exec_reply, 0, sizeof(f.exec_reply));
		else
			f.exec_reply.installed_pages = 3;
		f.exec_reply.stop_reason = cases[i].stop;
		f.exec_reply.op_errno = cases[i].error;
		f.exec_reply.error_stage = cases[i].stage;
		if (cases[i].stop == ZRAM_WB_STOP_GLOBAL_LIMIT && !cases[i].partial)
			f.exec_reply.scanned_pages = f.exec_reply.selected_pages = 1;
		if (cases[i].stop == ZRAM_WB_STOP_STALE) {
			/* A zero/mismatched caller identity is a kernel-known stale result. */
			f.request.expected.instance = 0;
			f.request.expected.generation = 0;
		}
		r = rb_exec(&b, &f.request);
		CHECK(r.known && r.no_replay && r.transport.outcome == RB_TRANSPORT_RETURNED);
		CHECK(r.op_errno == cases[i].error && r.error_stage == cases[i].stage);
		CHECK(r.stop_reason == cases[i].stop);
		CHECK(r.counters.installed_pages == (cases[i].partial ? 3u : 0u));
		finished(&f);
	}
	/* GLOBAL_LIMIT can consume some submissions yet invalidate unsubmitted slots. */
	b = setup(&f, true);
	f.exec_reply.scanned_pages = f.exec_reply.selected_pages = 12;
	f.exec_reply.submitted_pages = 3;
	f.exec_reply.installed_pages = 2;
	f.exec_reply.invalidated_pages = 10;
	f.exec_reply.stop_reason = ZRAM_WB_STOP_GLOBAL_LIMIT;
	f.exec_reply.op_errno = -EIO;
	r = rb_exec(&b, &f.request);
	CHECK(r.known && r.counters.invalidated_pages > r.counters.submitted_pages);
	finished(&f);
	/* A failed snapshot can make selected exceed submitted, without invalidation. */
	b = setup(&f, true);
	f.request.max_attempt_pages = f.request.max_installed_pages = 2;
	f.exec_reply.scanned_pages = f.exec_reply.selected_pages = 2;
	f.exec_reply.submitted_pages = 1;
	f.exec_reply.installed_pages = 0;
	f.exec_reply.invalidated_pages = 1;
	f.exec_reply.stop_reason = ZRAM_WB_STOP_ERROR;
	f.exec_reply.error_stage = ZRAM_WB_STAGE_SNAPSHOT_READ;
	f.exec_reply.op_errno = -EIO;
	CHECK(rb_exec(&b, &f.request).known);
	finished(&f);
	/* No work is allowed with either zero cap; both need not be zero. */
	for (unsigned int i = 0; i < 2; i++) {
		b = setup(&f, true);
		f.request.max_attempt_pages = i ? UINT64_MAX : 0;
		f.request.max_installed_pages = 0;
		memset(&f.exec_reply, 0, sizeof(f.exec_reply));
		f.exec_reply.stop_reason = ZRAM_WB_STOP_NO_WORK;
		CHECK(rb_exec(&b, &f.request).known);
		finished(&f);
	}
	/* Saturating attempt vs installation goals are distinct stop causes. */
	b = setup(&f, true);
	f.exec_reply.scanned_pages = f.exec_reply.selected_pages = 16;
	f.exec_reply.submitted_pages = 16;
	f.exec_reply.installed_pages = 4;
	f.exec_reply.invalidated_pages = 12;
	f.exec_reply.stop_reason = ZRAM_WB_STOP_ATTEMPT_LIMIT;
	CHECK(rb_exec(&b, &f.request).known);
	finished(&f);
}

static void test_counter_properties(void)
{
	/* Exhaustive small-domain inequalities, independent of stop/cap saturation.
	 * No invented equality selected==submitted or invalidated<=submitted. */
	for (uint64_t scanned = 0; scanned <= 4; scanned++) {
		for (uint64_t selected = 0; selected <= 4; selected++) {
			for (uint64_t submitted = 0; submitted <= 4; submitted++) {
				for (uint64_t installed = 0; installed <= 4; installed++) {
					for (uint64_t invalidated = 0; invalidated <= 4; invalidated++) {
						struct fake f;
						struct rb_backend b = setup(&f, true);
						bool valid = installed <= submitted && submitted <= 3 &&
							installed <= 2 && submitted <= selected &&
							selected <= scanned && installed + invalidated <= selected &&
							submitted > installed; /* STOP_ERROR / IO failed submission. */
						f.request.max_attempt_pages = 3;
						f.request.max_installed_pages = 2;
						f.exec_reply.scanned_pages = scanned;
						f.exec_reply.selected_pages = selected;
						f.exec_reply.submitted_pages = submitted;
						f.exec_reply.installed_pages = installed;
						f.exec_reply.invalidated_pages = invalidated;
						f.exec_reply.stop_reason = ZRAM_WB_STOP_ERROR;
						f.exec_reply.op_errno = -EIO;
						f.exec_reply.error_stage = ZRAM_WB_STAGE_IO;
						struct rb_exec_result r = rb_exec(&b, &f.request);
						CHECK(r.known == valid);
						if (!valid)
							unknown(r);
						finished(&f);
					}
				}
			}
		}
	}
}

static void test_invalid_replies(void)
{
	/* Mutate EACH echoed input byte: size/version/id/tokens/caps/reserved etc. */
	for (size_t offset = 0; offset < offsetof(struct zram_wb_exec, scanned_pages); offset++) {
		struct fake f;
		struct rb_backend b = setup(&f, true);
		f.corrupt = true;
		f.corrupt_offset = offset;
		struct rb_exec_result r = rb_exec(&b, &f.request);
		unknown(r);
		CHECK(r.transport.failure_stage == RB_FAILURE_RESPONSE && r.transport.error_no == EPROTO);
		finished(&f);
	}
	for (size_t offset = offsetof(struct zram_wb_exec, reserved_out); offset < sizeof(struct zram_wb_exec); offset++) {
		struct fake f;
		struct rb_backend b = setup(&f, true);
		f.corrupt = true;
		f.corrupt_offset = offset;
		unknown(rb_exec(&b, &f.request));
		finished(&f);
	}
	for (unsigned int kind = 0; kind < 13; kind++) {
		struct fake f;
		struct rb_backend b = setup(&f, true);
		switch (kind) {
		case 0: f.exec_reply.installed_pages = 11; break;
		case 1: f.exec_reply.submitted_pages = 17; break;
		case 2: f.exec_reply.installed_pages = 9; break;
		case 3: f.exec_reply.selected_pages = 21; break;
		case 4: f.exec_reply.invalidated_pages = 5; break;
		case 5: f.exec_reply.selected_pages = 9; break;
		case 6: f.exec_reply.invalidated_pages = UINT64_MAX; break;
		case 7: f.exec_reply.op_errno = EIO; break;
		case 8: f.exec_reply.stop_reason = ZRAM_WB_STOP_NONE; break;
		case 9: f.exec_reply.stop_reason = ZRAM_WB_STOP_NOT_READY + 1; break;
		case 10: f.exec_reply.error_stage = ZRAM_WB_STAGE_IO + 1; break;
		case 11: f.exec_reply.stop_reason = ZRAM_WB_STOP_ERROR; break;
		case 12: f.exec_reply.error_stage = ZRAM_WB_STAGE_IO; break;
		}
		unknown(rb_exec(&b, &f.request));
		finished(&f);
	}
	for (size_t offset = 0; offset < sizeof(struct zram_wb_status); offset++) {
		/* Every header/reserved byte, not the sampled output bytes. */
		if (offset >= offsetof(struct zram_wb_status, capabilities) &&
		    offset < offsetof(struct zram_wb_status, reserved_out))
			continue;
		struct fake f;
		struct rb_backend b = setup(&f, false);
		f.corrupt = true;
		f.corrupt_offset = offset;
		struct rb_status_result r = rb_status(&b);
		CHECK(!r.known && r.selection == RB_BACKEND_UNAVAILABLE);
		CHECK(r.transport.failure_stage == RB_FAILURE_RESPONSE && r.transport.error_no == EPROTO);
		CHECK(r.transport.outcome == RB_TRANSPORT_FAILED);
		finished(&f);
	}
	for (unsigned int kind = 0; kind < 4; kind++) {
		struct fake f;
		struct rb_backend b = setup(&f, false);
		if (kind == 0) f.status_reply.instance = 0;
		if (kind == 1) f.status_reply.generation = 0;
		if (kind == 2) f.status_reply.page_size = 0;
		if (kind == 3) f.status_reply.page_size = 16385;
		CHECK(!rb_status(&b).known);
		finished(&f);
	}
}

static void test_units_and_unsigned_caps(void)
{
	const uint32_t page_sizes[] = {4096, 16384, 65536};
	for (size_t i = 0; i < sizeof(page_sizes) / sizeof(page_sizes[0]); i++) {
		struct fake f;
		struct rb_backend b = setup(&f, false);
		f.status_reply.page_size = page_sizes[i];
		struct rb_status_result s = rb_status(&b);
		CHECK(s.known && s.page_size == page_sizes[i]);
		CHECK(s.sampled.wb_limit_remaining_4k == UINT64_MAX);
		CHECK(s.sampled.bd_writes_pages == 14);
		finished(&f);
		b = setup(&f, true);
		/* There is intentionally no page-size input to EXEC; caller converts. */
		f.request.max_attempt_pages = UINT64_MAX;
		f.request.max_installed_pages = UINT64_MAX - 1;
		f.request.request_id = UINT64_MAX;
		f.request.expected.generation = UINT64_MAX;
		f.request.filtered_idle_count = i == 0 ? 0 : 10;
		f.exec_reply.stop_reason = ZRAM_WB_STOP_COMPLETE;
		struct rb_exec_result r = rb_exec(&b, &f.request);
		CHECK(r.known && r.counters.submitted_pages == 10);
		CHECK(r.request_id == UINT64_MAX && r.expected.generation == UINT64_MAX);
		finished(&f);
	}
	/* A u64 input ceiling is legal; UINT64_MAX actual counters are not. */
	struct fake f;
	struct rb_backend b = setup(&f, true);
	f.request.max_attempt_pages = f.request.max_installed_pages = UINT64_MAX;
	f.exec_reply.scanned_pages = f.exec_reply.selected_pages = UINT64_MAX;
	f.exec_reply.submitted_pages = f.exec_reply.installed_pages = UINT64_MAX - 1;
	f.exec_reply.invalidated_pages = 1;
	f.exec_reply.stop_reason = ZRAM_WB_STOP_COMPLETE;
	struct rb_exec_result bad = rb_exec(&b, &f.request);
	unknown(bad);
	CHECK(bad.transport.error_no == EPROTO && bad.transport.failure_stage == RB_FAILURE_RESPONSE);
	finished(&f);
	/* Exactly 2^32 scans/selections: last slot is u32 UINT32_MAX, no wrap. */
	b = setup(&f, true);
	f.request.max_attempt_pages = f.request.max_installed_pages = UINT64_MAX;
	f.exec_reply.scanned_pages = f.exec_reply.selected_pages = UINT64_C(1) << 32;
	f.exec_reply.submitted_pages = f.exec_reply.installed_pages = UINT64_C(1) << 32;
	f.exec_reply.invalidated_pages = 0;
	f.exec_reply.stop_reason = ZRAM_WB_STOP_COMPLETE;
	CHECK(rb_exec(&b, &f.request).known);
	finished(&f);
}

static void malformed(struct rb_exec_result r)
{
	unknown(r);
	CHECK(r.transport.error_no == EPROTO);
	CHECK(r.transport.failure_stage == RB_FAILURE_RESPONSE);
}

static void test_stop_contract(void)
{
	const struct { uint32_t stop; int32_t error; } fixed[] = {
		{ZRAM_WB_STOP_GLOBAL_LIMIT, -EIO},
		{ZRAM_WB_STOP_BACKING_FULL, -ENOSPC},
		{ZRAM_WB_STOP_INTERRUPTED, -EINTR},
		{ZRAM_WB_STOP_STALE, -ESTALE},
		{ZRAM_WB_STOP_NOT_READY, -ENODEV},
	};
	for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
		for (unsigned int fault = 0; fault < 7; fault++) {
			struct fake f;
			struct rb_backend b = setup(&f, true);
			memset(&f.exec_reply, 0, sizeof(f.exec_reply));
			f.exec_reply.stop_reason = fixed[i].stop;
			f.exec_reply.op_errno = fixed[i].error;
			if (i < 2) f.exec_reply.scanned_pages = f.exec_reply.selected_pages = 1;
			if (fault == 0) f.exec_reply.op_errno = -EACCES;
			if (fault == 1) f.exec_reply.error_stage = ZRAM_WB_STAGE_IO;
			if (fault >= 2) {
				if (i < 3) continue;
				/* Every stale/not-ready counter must independently be zero. */
				if (fault == 2) f.exec_reply.scanned_pages = 1;
				if (fault == 3) f.exec_reply.selected_pages = 1;
				if (fault == 4) f.exec_reply.submitted_pages = 1;
				if (fault == 5) f.exec_reply.installed_pages = 1;
				if (fault == 6) f.exec_reply.invalidated_pages = 1;
			}
			malformed(rb_exec(&b, &f.request));
			finished(&f);
		}
	}
	for (unsigned int fault = 0; fault < 22; fault++) {
		struct fake f;
		struct rb_backend b = setup(&f, true);
		switch (fault) {
		case 0: /* Both budgets positive, even though no counters. */
			memset(&f.exec_reply, 0, sizeof(f.exec_reply));
			f.exec_reply.stop_reason = ZRAM_WB_STOP_NO_WORK;
			break;
		case 1: /* NO_WORK cannot scan even when zero installation budget. */
			memset(&f.exec_reply, 0, sizeof(f.exec_reply));
			f.request.max_installed_pages = 0;
			f.exec_reply.scanned_pages = 1;
			f.exec_reply.stop_reason = ZRAM_WB_STOP_NO_WORK;
			break;
		case 2: f.exec_reply.installed_pages--; break;
		case 3: f.exec_reply.stop_reason = ZRAM_WB_STOP_ATTEMPT_LIMIT; break;
		case 4: /* Equal goals prefer installed. */
			f.request.max_attempt_pages = f.exec_reply.submitted_pages;
			f.exec_reply.stop_reason = ZRAM_WB_STOP_ATTEMPT_LIMIT;
			break;
		case 5: f.exec_reply.stop_reason = ZRAM_WB_STOP_COMPLETE; break;
		case 6: /* COMPLETE cannot hide an attempt limit. */
			f.request.max_attempt_pages = f.exec_reply.submitted_pages;
			f.exec_reply.installed_pages--;
			f.exec_reply.stop_reason = ZRAM_WB_STOP_COMPLETE;
			break;
		case 7: case 8:
			f.exec_reply.scanned_pages = (UINT64_C(1) << 32) + 1;
			if (fault == 8) f.exec_reply.selected_pages = f.exec_reply.scanned_pages;
			break;
		case 9: /* Former bogus known result with huge cap. */
			f.request.max_attempt_pages = f.request.max_installed_pages = UINT64_MAX;
			break;
		case 10: case 11: case 12: case 13:
			f.exec_reply.stop_reason = ZRAM_WB_STOP_ERROR;
			f.exec_reply.op_errno = fault == 13 ? -EINVAL : -ENOMEM;
			f.exec_reply.error_stage = fault == 10 ? ZRAM_WB_STAGE_BIO_BUILD :
				fault == 11 ? ZRAM_WB_STAGE_ALLOC : ZRAM_WB_STAGE_NONE;
			break;
		case 14: case 15:
			f.exec_reply.stop_reason = ZRAM_WB_STOP_ERROR;
			f.exec_reply.op_errno = -EIO;
			f.exec_reply.error_stage = fault == 14 ? ZRAM_WB_STAGE_IO : ZRAM_WB_STAGE_SNAPSHOT_READ;
			if (fault == 14) f.exec_reply.submitted_pages = f.exec_reply.installed_pages;
			else f.exec_reply.selected_pages = f.exec_reply.submitted_pages;
			break;
		case 16:
			memset(&f.exec_reply, 0, sizeof(f.exec_reply));
			f.exec_reply.stop_reason = ZRAM_WB_STOP_GLOBAL_LIMIT;
			f.exec_reply.op_errno = -EIO;
			break;
		case 17:
			f.exec_reply.stop_reason = ZRAM_WB_STOP_BACKING_FULL;
			f.exec_reply.op_errno = -ENOSPC;
			break;
		case 18: case 19: case 20: case 21:
			memset(&f.exec_reply, 0, sizeof(f.exec_reply));
			f.request.max_installed_pages = 0;
			f.exec_reply.stop_reason = fault == 18 ? ZRAM_WB_STOP_COMPLETE :
				fault == 19 ? ZRAM_WB_STOP_INSTALLED_LIMIT :
				fault == 20 ? ZRAM_WB_STOP_INTERRUPTED : ZRAM_WB_STOP_ERROR;
			if (fault == 20) f.exec_reply.op_errno = -EINTR;
			if (fault == 21) { f.exec_reply.op_errno = -ENOMEM; f.exec_reply.error_stage = ZRAM_WB_STAGE_ALLOC; }
			break;
		}
		malformed(rb_exec(&b, &f.request));
		finished(&f);
	}
	/* Cap ties prefer installed, but terminal I/O failure precedes attempt cap. */
	struct fake f;
	struct rb_backend b = setup(&f, true);
	f.request.max_attempt_pages = f.exec_reply.submitted_pages;
	CHECK(rb_exec(&b, &f.request).known);
	finished(&f);
	b = setup(&f, true);
	f.request.max_attempt_pages = f.exec_reply.submitted_pages;
	f.exec_reply.stop_reason = ZRAM_WB_STOP_ERROR;
	f.exec_reply.error_stage = ZRAM_WB_STAGE_IO;
	f.exec_reply.op_errno = -EREMOTEIO;
	CHECK(rb_exec(&b, &f.request).known);
	finished(&f);
}

static void test_close_and_no_replay(void)
{
	for (unsigned int op = 0; op < 2; op++) {
		struct fake f;
		struct rb_backend b = setup(&f, op != 0);
		f.close_errno = EINTR;
		if (op) {
			struct rb_exec_result r = rb_exec(&b, &f.request);
			CHECK(r.known && r.no_replay && r.transport.close_errno == EINTR);
			CHECK(r.transport.error_no == 0);
		} else {
			struct rb_status_result r = rb_status(&b);
			CHECK(r.known && r.transport.close_errno == EINTR);
		}
		finished(&f);
	}
	struct fake f;
	struct rb_backend b = setup(&f, true);
	f.ioctl_errno = EFAULT;
	f.close_errno = EIO;
	struct rb_exec_result r = rb_exec(&b, &f.request);
	unknown(r);
	CHECK(r.transport.error_no == EFAULT && r.transport.close_errno == EIO);
	finished(&f);
	/* A later STATUS (even ENOTTY) does not turn the previous EXEC into known,
	 * authorize its replay, or select legacy for that attempted operation. */
	f.exec_operation = false;
	f.expected_access = O_RDONLY;
	f.ioctl_errno = ENOTTY;
	f.close_errno = 0;
	struct rb_status_result s = rb_status(&b);
	CHECK(s.selection == RB_BACKEND_LEGACY);
	unknown(r);
	CHECK(f.opens == 2 && f.ioctls == 2 && f.closes == 2 && f.active == 0);
	f.ioctl_errno = 0;
	s = rb_status(&b);
	CHECK(s.known);
	unknown(r);
	CHECK(f.opens == 3 && f.ioctls == 3 && f.closes == 3 && f.active == 0);
	test_count++;
	/* Stateless, not a replay ledger: repeated caller invocations are distinct.
	 * This tests reopening/closing only, NOT recommending reuse of a request ID. */
	b = setup(&f, true);
	CHECK(rb_exec(&b, &f.request).known);
	f.request.request_id++;
	CHECK(rb_exec(&b, &f.request).known);
	CHECK(f.opens == 2 && f.ioctls == 2 && f.closes == 2 && f.active == 0);
	test_count++;
}

int main(void)
{
	test_status();
	test_syscall_errors();
	test_preflight();
	test_input();
	test_known_results();
	test_counter_properties();
	test_invalid_replies();
	test_units_and_unsigned_caps();
	test_stop_contract();
	test_close_and_no_replay();
	printf("PASS: %u backend scenarios (injected syscalls, no device I/O)\n", test_count);
	return 0;
}
