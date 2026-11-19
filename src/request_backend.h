/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ZRAM_REQUEST_BACKEND_H
#define ZRAM_REQUEST_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

#define RB_DEVICE_PATH "/dev/block/zram0"
#define RB_DEVICE_LABEL "u:object_r:lge_zram0_device:s0"

/*
 * Independent, synchronous FILTERED_IDLE transport; no core/thread integration,
 * ledger, retry, policy, idle marking, sysfs writes, quota setter or refund.
 * All caps/counters are unsigned KERNEL PAGES, without a business ceiling.
 * Bytes/4KiB <-> kernel-page conversion belongs to the caller (4/16/64KiB too).
 * A request_id is correlation only, never a deduplication key.
 */
struct rb_identity {
	uint64_t instance;
	uint64_t generation;
};

struct rb_exec_request {
	struct rb_identity expected; /* Obtained from a validated STATUS. */
	uint64_t request_id;         /* Nonzero. */
	uint64_t max_attempt_pages;
	uint64_t max_installed_pages; /* <= max_attempt_pages; zero is legal. */
	uint32_t filtered_idle_count; /* UAPI range 0..10; not an idle-mark action. */
};

/*
 * Optional dependency injection. NULL syscalls selects the real POSIX adapter.
 * An injected table must provide ALL callbacks, with POSIX return/errno rules.
 * The table/context must outlive the call. Callbacks must not replay ioctl.
 * No descriptor, identity, STATUS result or request history is retained here.
 */
struct rb_syscalls {
	int (*open_fn)(void *context, const char *path, int flags);
	int (*fstat_fn)(void *context, int fd, struct stat *st);
	ssize_t (*fgetxattr_fn)(void *context, int fd, const char *name,
			      void *value, size_t size);
	int (*ioctl_fn)(void *context, int fd, unsigned long command, void *arg);
	int (*close_fn)(void *context, int fd);
};

struct rb_backend {
	dev_t expected_rdev; /* Caller-established identity of the zram0 block node. */
	const struct rb_syscalls *syscalls;
	void *syscall_context;
};

enum rb_transport_outcome {
	RB_TRANSPORT_NOT_CALLED = 0,
	RB_TRANSPORT_RETURNED,
	RB_TRANSPORT_FAILED,          /* STATUS only; no trustworthy STATUS result. */
	RB_TRANSPORT_OUTCOME_UNKNOWN  /* EXEC called, result unavailable/untrusted. */
};

enum rb_failure_stage {
	RB_FAILURE_NONE = 0,
	RB_FAILURE_INPUT,
	RB_FAILURE_OPEN,
	RB_FAILURE_FSTAT,
	RB_FAILURE_DEVICE,
	RB_FAILURE_LABEL,
	RB_FAILURE_IOCTL,
	RB_FAILURE_RESPONSE
};

struct rb_transport {
	enum rb_transport_outcome outcome;
	enum rb_failure_stage failure_stage;
	bool syscall_called; /* Specifically the operation's ioctl, not open/fstat. */
	int error_no;        /* Positive local/syscall errno; EPROTO for bad replies. */
	int close_errno;     /* Separate diagnostic; never erases a known result. */
};

enum rb_backend_selection {
	RB_BACKEND_UNAVAILABLE = 0,
	RB_BACKEND_REQUEST,
	RB_BACKEND_LEGACY /* Only STATUS ioctl -1/ENOTTY; never executes legacy. */
};

enum rb_statistics_consistency {
	RB_STATISTICS_UNAVAILABLE = 0,
	RB_STATISTICS_SAMPLED /* Individually sampled, explicitly NOT coherent. */
};

struct rb_sampled_statistics {
	uint64_t wb_limit_remaining_4k; /* Original UAPI unit, NOT converted. */
	uint64_t pages_stored;
	uint64_t same_pages;
	uint64_t huge_pages;
	uint64_t lowratio_pages;
	uint64_t bd_count_pages;
	uint64_t bd_reads_pages;
	uint64_t bd_writes_pages;
	uint64_t compr_data_bytes;
	uint64_t mem_used_bytes;
};

struct rb_status_result {
	struct rb_transport transport;
	bool known;
	enum rb_backend_selection selection;
	enum rb_statistics_consistency statistics_consistency;
	uint64_t capabilities; /* Raw ZRAM_WB_CAP_* bits; unknown bits preserved. */
	struct rb_identity identity;
	uint64_t disksize_bytes;
	uint32_t page_size;
	uint32_t state_flags; /* Raw ZRAM_WB_STATE_* bits. */
	bool ready; /* INITIALIZED && BACKING; false never means legacy. */
	struct rb_sampled_statistics sampled;
};

struct rb_request_counters {
	uint64_t scanned_pages;
	uint64_t selected_pages;
	uint64_t submitted_pages;
	uint64_t installed_pages;
	uint64_t invalidated_pages;
};

struct rb_exec_result {
	struct rb_transport transport;
	bool known;
	bool no_replay; /* True whenever EXEC ioctl was called, even on success. */
	/* The following payload is meaningful ONLY when known is true. */
	uint64_t request_id;
	struct rb_identity expected;
	int32_t op_errno;     /* Zero or NEGATIVE errno; may accompany partial work. */
	uint32_t stop_reason; /* ZRAM_WB_STOP_* from the frozen UAPI. */
	uint32_t error_stage; /* ZRAM_WB_STAGE_*; distinct from transport failure. */
	struct rb_request_counters counters;
	uint64_t duration_ns;
};

/*
 * Per call: fixed path, O_CLOEXEC|O_NOFOLLOW, fstat(S_IFBLK + expected_rdev),
 * fgetxattr(security.selinux) matching RB_DEVICE_LABEL, one ioctl, one close.
 * STATUS uses O_RDONLY; EXEC uses O_RDWR. No EINTR retry (including close).
 * known=false zero payload is NOT proof of zero effects. For EXEC, ANY syscall
 * error (even EPERM/ENOTTY/EFAULT/EINTR) after ioctl entry means unknown/no-replay.
 * ioctl==0 with a validated negative op_errno is a KNOWN, possibly partial result.
 * STATUS does not retrieve/certify EXEC results, nor clear unknown outcomes.
 * The caller MUST honor no_replay, including after a later STATUS/ENOTTY: that
 * cannot authorize replay via legacy. There is no cross-call deduplication state.
 * No automatic fallback exists; only rb_status can select RB_BACKEND_LEGACY.
 */
struct rb_status_result rb_status(const struct rb_backend *backend);
struct rb_exec_result rb_exec(const struct rb_backend *backend,
			     const struct rb_exec_request *request);

#endif
