# Helper bounded cleanup contract

`helper.c` keeps the existing public call signatures and `nzm_helper_run` layout.
A successful fork still sets `attempted=1`; any subsequent failure remains an
unknown operation outcome, including `reaped=0`. Never replay that operation.
A later reap does not retroactively update the returned run or prove whether the
operation took effect. The original timeout, cancellation, overflow or other
operation error takes precedence over cleanup errors.

Every call reserves one of 16 process-owned child slots before opening spawn
resources. Setup/fork failure releases that slot. Concurrent active calls retain
independent exact-PID ownership. A mutex serializes slot changes and the final
pending check/fork/PID publication; exhaustion returns `-EBUSY` without attempting
execution. No new thread or background reaper is created.

Cleanup uses only exact-PID `waitpid(..., WNOHANG)`. After a zero ownership probe,
the active owner sends SIGKILL once. Cleanup then observes a 100 ms monotonic
grace, at most 32 probes, with non-retried 5 ms sleeps. This bounds cleanup's
intentional waiting even if SIGKILL cannot complete or waits keep returning
EINTR; ordinary scheduler/syscall latency is not a hard real-time guarantee.
Clock failure ends the grace immediately. ECHILD relinquishes ownership and
never authorizes a PID signal.

If still unresolved at return, ownership transfers to the reserved process
slot. Descriptors and caller storage are not retained. A worker can return/join
without losing the PID; another thread can later enter an eligible helper call
or preflight and perform one nonblocking exact-PID probe per pending slot.
Those entry probes never signal. Any unresolved slot blocks that entry with
`-EBUSY`; a new call has `attempted=0`. Successful exact reap or ECHILD releases
the slot. Stop/argument rejection may return before probing pending slots.
Preflight itself never forks or executes a helper.

The sole-reaper and stable default SIGCHLD-disposition requirements extend past
worker return until retained ownership is resolved. No external wait-any-child
loop, SIGCHLD reaper, SIG_IGN or SA_NOCLDWAIT may consume these children. Pending
state is process-local, not a durable reconciliation ledger: keep the process
alive and make later safe entries to collect children when possible. Process
exit/restart is not operation cancellation or reconciliation; a surviving
kernel-blocked helper may outlive the daemon. Supervisors/callers must not infer
retry safety from exit/restart, ECHILD, or an empty pending set. No new external
API is required by this change.

## Focused fixtures

Use the helper-specific compile/run commands in `../build-and-test.sh` for
`test_safe_spawn` and `test_io_helper`, both plain and sanitized. The former
adds hidden-readiness fault injection to model an unreapable child without
using a device: bounded return, original-error precedence over kill failure,
persistent EINTR, capacity exhaustion/release, busy entry, cross-thread deferred
reap, and deferred ECHILD without signaling. Existing real ELF, FD churn,
concurrent owners, unrelated-child, timeout, stop, and setup/fork failure tests
remain enabled. The syscall adapter rejects every helper wait not using
WNOHANG and an owned exact PID.

These host fixtures do not create actual uninterruptible kernel I/O and do not
prove Android/device timing or whole-daemon shutdown/restart reconciliation.
