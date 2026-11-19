/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "request_backend.h"
/* Include ONLY the frozen header, never add the kernel include tree to -I. */
#include "uapi/linux/zram.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/xattr.h>
#include <unistd.h>

_Static_assert(sizeof(struct zram_wb_exec) == 160, "EXEC ABI size");
_Static_assert(sizeof(struct zram_wb_status) == 192, "STATUS ABI size");
_Static_assert(offsetof(struct zram_wb_exec, request_id) == 16, "request id");
_Static_assert(offsetof(struct zram_wb_exec, expected_instance) == 24, "instance");
_Static_assert(offsetof(struct zram_wb_exec, expected_generation) == 32, "generation");
_Static_assert(offsetof(struct zram_wb_exec, scanned_pages) == 96, "counters");
_Static_assert(offsetof(struct zram_wb_exec, stop_reason) == 144, "stop reason");
_Static_assert(offsetof(struct zram_wb_exec, op_errno) == 148, "signed errno");
_Static_assert(offsetof(struct zram_wb_exec, error_stage) == 152, "u32 stage");
_Static_assert(offsetof(struct zram_wb_exec, reserved_out) == 156, "reserved out");
_Static_assert(offsetof(struct zram_wb_status, capabilities) == 32, "capabilities");
_Static_assert(offsetof(struct zram_wb_status, reserved_out) == 152, "status reserved");
_Static_assert(_IOC_SIZE(ZRAM_IOC_WB_EXEC) == 160, "EXEC ioctl size");
_Static_assert(_IOC_SIZE(ZRAM_IOC_WB_STATUS) == 192, "STATUS ioctl size");

static int posix_open(void *context, const char *path, int flags)
{
	(void)context;
	return open(path, flags);
}

static int posix_fstat(void *context, int fd, struct stat *st)
{
	(void)context;
	return fstat(fd, st);
}

static ssize_t posix_fgetxattr(void *context, int fd, const char *name,
			     void *value, size_t size)
{
	(void)context;
	return fgetxattr(fd, name, value, size);
}

static int posix_ioctl(void *context, int fd, unsigned long command, void *arg)
{
	(void)context;
#ifdef __BIONIC__
	return ioctl(fd, (int)command, arg);
#else
	return ioctl(fd, command, arg);
#endif
}

static int posix_close(void *context, int fd)
{
	(void)context;
	return close(fd);
}

static const struct rb_syscalls real_syscalls = {
	.open_fn = posix_open,
	.fstat_fn = posix_fstat,
	.fgetxattr_fn = posix_fgetxattr,
	.ioctl_fn = posix_ioctl,
	.close_fn = posix_close,
};

static const struct rb_syscalls *get_syscalls(const struct rb_backend *backend)
{
	const struct rb_syscalls *ops;

	if (!backend)
		return NULL;
	ops = backend->syscalls ? backend->syscalls : &real_syscalls;
	if (!ops->open_fn || !ops->fstat_fn || !ops->fgetxattr_fn ||
	    !ops->ioctl_fn || !ops->close_fn)
		return NULL;
	return ops;
}

static int saved_errno(void)
{
	/* Also fail closed for a broken injection callback that forgot errno. */
	return errno > 0 ? errno : EIO;
}

static void fail(struct rb_transport *transport, enum rb_failure_stage stage,
		 int error_no)
{
	transport->failure_stage = stage;
	transport->error_no = error_no;
}

static void close_once(const struct rb_backend *backend,
		       const struct rb_syscalls *ops, int fd,
		       struct rb_transport *transport)
{
	errno = 0;
	if (ops->close_fn(backend->syscall_context, fd) != 0)
		transport->close_errno = saved_errno();
	/* Linux closes even on EINTR; retry could close somebody else's fd. */
}

static int open_checked(const struct rb_backend *backend,
			const struct rb_syscalls *ops, int access,
			struct rb_transport *transport)
{
	struct stat st;
	char label[sizeof(RB_DEVICE_LABEL) + 1];
	const size_t label_len = sizeof(RB_DEVICE_LABEL) - 1;
	ssize_t length;
	int fd;

	errno = 0;
	fd = ops->open_fn(backend->syscall_context, RB_DEVICE_PATH,
			 access | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0) {
		fail(transport, RB_FAILURE_OPEN, saved_errno());
		return -1;
	}
	memset(&st, 0, sizeof(st));
	errno = 0;
	if (ops->fstat_fn(backend->syscall_context, fd, &st) != 0) {
		fail(transport, RB_FAILURE_FSTAT, saved_errno());
		goto rejected;
	}
	if (!S_ISBLK(st.st_mode) || st.st_rdev != backend->expected_rdev) {
		fail(transport, RB_FAILURE_DEVICE, ENODEV);
		goto rejected;
	}
	memset(label, 0, sizeof(label));
	errno = 0;
	length = ops->fgetxattr_fn(backend->syscall_context, fd,
				 "security.selinux", label, sizeof(label));
	if (length < 0) {
		fail(transport, RB_FAILURE_LABEL, saved_errno());
		goto rejected;
	}
	/* SELinux xattrs may include one terminating NUL; no prefixes/suffixes. */
	if ((length != (ssize_t)label_len &&
	     length != (ssize_t)(label_len + 1)) ||
	    memcmp(label, RB_DEVICE_LABEL, label_len) != 0 ||
	    (length == (ssize_t)(label_len + 1) && label[label_len] != '\0')) {
		fail(transport, RB_FAILURE_LABEL, EACCES);
		goto rejected;
	}
	return fd;

rejected:
	close_once(backend, ops, fd, transport);
	return -1;
}

static bool valid_status(const struct zram_wb_status *status)
{
	size_t i;

	if (status->size != sizeof(*status) ||
	    status->version != ZRAM_WB_ABI_VERSION || status->flags ||
	    status->reserved0 || !status->instance || !status->generation ||
	    !status->page_size || (status->page_size & (status->page_size - 1)))
		return false;
	for (i = 0; i < sizeof(status->reserved) / sizeof(status->reserved[0]); i++)
		if (status->reserved[i])
			return false;
	for (i = 0; i < sizeof(status->reserved_out) / sizeof(status->reserved_out[0]); i++)
		if (status->reserved_out[i])
			return false;
	/* No cross-statistic checks: the statistics/limit are SAMPLED, not coherent. */
	return true;
}

struct rb_status_result rb_status(const struct rb_backend *backend)
{
	struct rb_status_result result = {0};
	struct zram_wb_status status;
	const struct rb_syscalls *ops = get_syscalls(backend);
	const uint64_t required = ZRAM_WB_CAP_EXEC | ZRAM_WB_CAP_STATUS |
		ZRAM_WB_CAP_FILTERED_IDLE | ZRAM_WB_CAP_DUAL_LIMIT;
	const uint32_t ready_flags = ZRAM_WB_STATE_INITIALIZED | ZRAM_WB_STATE_BACKING;
	int fd, rc, error_no;

	if (!ops) {
		fail(&result.transport, RB_FAILURE_INPUT, EINVAL);
		return result;
	}
	memset(&status, 0, sizeof(status));
	status.size = (uint32_t)sizeof(status);
	status.version = ZRAM_WB_ABI_VERSION;
	fd = open_checked(backend, ops, O_RDONLY, &result.transport);
	if (fd < 0)
		return result;
	result.transport.syscall_called = true;
	errno = 0;
	rc = ops->ioctl_fn(backend->syscall_context, fd, ZRAM_IOC_WB_STATUS, &status);
	error_no = saved_errno();
	close_once(backend, ops, fd, &result.transport);
	if (rc != 0) {
		result.transport.outcome = RB_TRANSPORT_FAILED;
		fail(&result.transport, RB_FAILURE_IOCTL, rc < 0 ? error_no : EPROTO);
		if (rc == -1 && error_no == ENOTTY)
			result.selection = RB_BACKEND_LEGACY;
		return result;
	}
	if (!valid_status(&status)) {
		result.transport.outcome = RB_TRANSPORT_FAILED;
		fail(&result.transport, RB_FAILURE_RESPONSE, EPROTO);
		return result;
	}
	result.transport.outcome = RB_TRANSPORT_RETURNED;
	result.known = true;
	result.selection = (status.capabilities & required) == required ?
		RB_BACKEND_REQUEST : RB_BACKEND_UNAVAILABLE;
	result.statistics_consistency = RB_STATISTICS_SAMPLED;
	result.capabilities = status.capabilities;
	result.identity.instance = status.instance;
	result.identity.generation = status.generation;
	result.disksize_bytes = status.disksize_bytes;
	result.page_size = status.page_size;
	result.state_flags = status.state_flags;
	result.ready = (status.state_flags & ready_flags) == ready_flags;
	result.sampled.wb_limit_remaining_4k = status.wb_limit_remaining_4k;
	result.sampled.pages_stored = status.pages_stored;
	result.sampled.same_pages = status.same_pages;
	result.sampled.huge_pages = status.huge_pages;
	result.sampled.lowratio_pages = status.lowratio_pages;
	result.sampled.bd_count_pages = status.bd_count_pages;
	result.sampled.bd_reads_pages = status.bd_reads_pages;
	result.sampled.bd_writes_pages = status.bd_writes_pages;
	result.sampled.compr_data_bytes = status.compr_data_bytes;
	result.sampled.mem_used_bytes = status.mem_used_bytes;
	return result;
}

static bool no_counts(const struct zram_wb_exec *reply)
{
	return !reply->scanned_pages && !reply->selected_pages &&
		!reply->submitted_pages && !reply->installed_pages &&
		!reply->invalidated_pages;
}

static bool valid_exec(const struct zram_wb_exec *reply,
		       const struct zram_wb_exec *input)
{
	/* Every input/header/token/reserved field is echoed by this frozen ABI. */
	if (memcmp(reply, input, offsetof(struct zram_wb_exec, scanned_pages)) ||
	    reply->reserved_out || reply->op_errno > 0 ||
	    reply->error_stage > ZRAM_WB_STAGE_IO ||
	    reply->stop_reason < ZRAM_WB_STOP_COMPLETE ||
	    reply->stop_reason > ZRAM_WB_STOP_NOT_READY)
		return false;
	if (reply->stop_reason != ZRAM_WB_STOP_ERROR &&
	    reply->error_stage != ZRAM_WB_STAGE_NONE)
		return false;
	/* Current engine uses u64 index/nr_pages and rejects only nr_pages > 2^32.
	 * The final legal u32 slot index is UINT32_MAX, hence 2^32 scans are legal.
	 * This is an ACTUAL counter bound, never an input budget/business ceiling.
	 */
	if (reply->scanned_pages > (UINT64_C(1) << 32) ||
	    reply->installed_pages > reply->submitted_pages ||
	    reply->submitted_pages > input->max_attempt_pages ||
	    reply->installed_pages > input->max_installed_pages ||
	    reply->submitted_pages > reply->selected_pages ||
	    reply->selected_pages > reply->scanned_pages)
		return false;
	/* Each selection is finalized once; invalidated is NOT <= submitted.
	 * Subtract after installed <= selected, avoiding counter-addition overflow.
	 */
	if (reply->invalidated_pages > reply->selected_pages - reply->installed_pages)
		return false;

	/* Identity/readiness checks precede the engine, even for zero budgets. */
	if (reply->stop_reason == ZRAM_WB_STOP_STALE)
		return reply->op_errno == -ESTALE && no_counts(reply);
	if (reply->stop_reason == ZRAM_WB_STOP_NOT_READY)
		return reply->op_errno == -ENODEV && no_counts(reply);
	if (reply->stop_reason == ZRAM_WB_STOP_NO_WORK)
		return reply->op_errno == 0 && no_counts(reply) &&
			(!input->max_attempt_pages || !input->max_installed_pages);
	/* FILTERED_IDLE has no page-index error. Overflow is checked before caps. */
	if (reply->stop_reason == ZRAM_WB_STOP_ERROR &&
	    reply->error_stage == ZRAM_WB_STAGE_NONE)
		return reply->op_errno == -EOVERFLOW && no_counts(reply);
	if (!input->max_attempt_pages || !input->max_installed_pages)
		return false;

	switch (reply->stop_reason) {
	case ZRAM_WB_STOP_COMPLETE:
		return reply->op_errno == 0 &&
			reply->installed_pages < input->max_installed_pages &&
			reply->submitted_pages < input->max_attempt_pages;
	case ZRAM_WB_STOP_INSTALLED_LIMIT:
		/* Ties select installed, not attempt; an earlier error still wins. */
		return reply->op_errno == 0 &&
			reply->installed_pages == input->max_installed_pages;
	case ZRAM_WB_STOP_ATTEMPT_LIMIT:
		return reply->op_errno == 0 &&
			reply->submitted_pages == input->max_attempt_pages &&
			reply->installed_pages < input->max_installed_pages;
	case ZRAM_WB_STOP_GLOBAL_LIMIT:
		/* A limited flush always leaves some selections unsubmitted. */
		return reply->op_errno == -EIO &&
			reply->selected_pages > reply->submitted_pages;
	case ZRAM_WB_STOP_BACKING_FULL:
		return reply->op_errno == -ENOSPC && reply->scanned_pages != 0 &&
			reply->installed_pages < input->max_installed_pages &&
			reply->submitted_pages < input->max_attempt_pages;
	case ZRAM_WB_STOP_INTERRUPTED:
		return reply->op_errno == -EINTR;
	case ZRAM_WB_STOP_ERROR:
		switch (reply->error_stage) {
		case ZRAM_WB_STAGE_ALLOC:
			return reply->op_errno == -ENOMEM && no_counts(reply);
		case ZRAM_WB_STAGE_BIO_BUILD:
			return reply->op_errno == -EIO &&
				reply->selected_pages > reply->submitted_pages;
		case ZRAM_WB_STAGE_SNAPSHOT_READ:
			/* Propagates the reader's negative errno, not just -EIO. */
			return reply->op_errno < 0 &&
				reply->selected_pages > reply->submitted_pages;
		case ZRAM_WB_STAGE_IO:
			/* Failed BIO pages are submitted but not installed. */
			return reply->op_errno < 0 &&
				reply->submitted_pages > reply->installed_pages;
		default:
			return false;
		}
	default:
		return false;
	}
}

struct rb_exec_result rb_exec(const struct rb_backend *backend,
			     const struct rb_exec_request *request)
{
	struct rb_exec_result result = {0};
	struct zram_wb_exec input, reply;
	const struct rb_syscalls *ops = get_syscalls(backend);
	int fd, rc, error_no;

	if (!ops || !request || !request->request_id ||
	    request->filtered_idle_count > 10 ||
	    request->max_installed_pages > request->max_attempt_pages) {
		fail(&result.transport, RB_FAILURE_INPUT, EINVAL);
		return result;
	}
	memset(&input, 0, sizeof(input));
	input.size = (uint32_t)sizeof(input);
	input.version = ZRAM_WB_ABI_VERSION;
	input.mode = ZRAM_WB_MODE_FILTERED_IDLE;
	input.request_id = request->request_id;
	input.expected_instance = request->expected.instance;
	input.expected_generation = request->expected.generation;
	input.max_attempt_pages = request->max_attempt_pages;
	input.max_installed_pages = request->max_installed_pages;
	input.idle_count = request->filtered_idle_count;
	reply = input;
	fd = open_checked(backend, ops, O_RDWR, &result.transport);
	if (fd < 0)
		return result;
	result.no_replay = true;
	result.transport.syscall_called = true;
	/* Set the conservative outcome BEFORE calling: no retry on any errno. */
	result.transport.outcome = RB_TRANSPORT_OUTCOME_UNKNOWN;
	errno = 0;
	rc = ops->ioctl_fn(backend->syscall_context, fd, ZRAM_IOC_WB_EXEC, &reply);
	error_no = saved_errno();
	close_once(backend, ops, fd, &result.transport);
	if (rc != 0) {
		fail(&result.transport, RB_FAILURE_IOCTL, rc < 0 ? error_no : EPROTO);
		return result;
	}
	if (!valid_exec(&reply, &input)) {
		fail(&result.transport, RB_FAILURE_RESPONSE, EPROTO);
		return result;
	}
	result.transport.outcome = RB_TRANSPORT_RETURNED;
	result.known = true;
	result.request_id = reply.request_id;
	result.expected.instance = reply.expected_instance;
	result.expected.generation = reply.expected_generation;
	result.op_errno = reply.op_errno;
	result.stop_reason = reply.stop_reason;
	result.error_stage = reply.error_stage;
	result.counters.scanned_pages = reply.scanned_pages;
	result.counters.selected_pages = reply.selected_pages;
	result.counters.submitted_pages = reply.submitted_pages;
	result.counters.installed_pages = reply.installed_pages;
	result.counters.invalidated_pages = reply.invalidated_pages;
	result.duration_ns = reply.duration_ns;
	return result;
}
