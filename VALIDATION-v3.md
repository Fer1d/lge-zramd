# Submitted-write settlement candidate (wear-v3)

Implemented in the standalone controller repository. No device writes, reboot, live migration, manual budget changes, git commit or push occurred for this change. The installed device remains on the prior wear-v2 candidate.

## Accounting

Like Android mmd's consumed-quota accounting, bill known submitted writes rather than approved capacity. This controller preserves a durable full reservation before execution and a single nonaging pending record, unlike a purely in-memory history. Validated reply, verified zero quota, matching lifecycle and fresh completion BOOTTIME precede one durable settlement. The charge is submitted_pages times page size, including submitted failed/partially installed BIOs; zero installed does not imply zero writes. This is host-submitted data, not NAND wear measurement.

Zero/partial known submissions release only unused reservation after durable commit. Lifetime approved total never decreases, known-submitted cumulative covers v3 only. One receipt makes identical settlement idempotent; no unrestricted refund API. Unknown ioctl/reply/time/identity/closure/commit retains conservative uncertainty and blocks replay. A known op-error reply may settle counters but retains intent and halts under existing retirement rules.

Pending reservation never ages out, including long execution, crash and newboot. Actual completed charge enters completion-time bucket. Crossboot actual settlement is rejected. V2 migration keeps anchors/buckets/IDs/lifetime approved/generation and expiry intact; no old refund or renewed waiting interval. Existing v1 conservative migration remains separate with exact original backup.

## Verification

Final full build-and-test.sh: exit0. Log ignored locally at build/v3-aggregate-ready.log. Ordinary and ASan/UBSan suites passed, including422842 budget assertions; bounded100day history; idempotent/conflicting settlements; pending beyond24h; wrong boot/ID/amount; v2 conversion preservation; executor reserve/close/settle/retire ordering; known op-error; submitted-not-installed; zero/full/partial submission; durable replacement succeeded then returned error with no retry/retirement. Store tests verify exact snapshot and owned intent, immutable v1 backup and v2 migration without new backup.

Existing no-/dev startup, readonly status, VM multiplier, helper ownership, request transport3477 scenarios and4/16/64KiB unit fixtures retained. Real-main18 startup and17 historical cases per normal/sanitized variant passed. Historical regressions are not a production fallback. Child trace106 real helper exec attempts passed. TSan compiled, execution SKIPPED for host unexpected-memory-mapping startup; NOT suite PASS. First in-progress aggregate failed store fixture initializer and is preserved as build/v3-aggregate-first.log, not represented as PASS.

## API29 candidate

build/demand/lge-zramd-candidate.android29.arm64
Size112760 bytes
SHA256 c3e628f9dccec6b9f08092597d9f43152fb9809ab42d52915b17e0748f1a615e

Disarmed and opt-in Android builds compile. Configuration assignments unchanged; explanatory comments updated. No new kernel ABI, persistent filenames, /dev directory or permission grants required beyond prior wear-v2 integration. Device verification/migration remains separate; historical device deductions cannot be refunded without evidence.

## Sources

- Linux zram writeback budget documentation: https://www.kernel.org/doc/html/latest/admin-guide/blockdev/zram.html#writeback
- Android mmd writeback: https://android.googlesource.com/platform/system/memory/mmd/+/refs/heads/main/src/zram/writeback.rs
- Android mmd history: https://android.googlesource.com/platform/system/memory/mmd/+/refs/heads/main/src/zram/writeback/history.rs

Design comparison only; upstream Rust source was not copied into the C implementation. Exact request submitted counter semantics are from this repository's companion Plan-B kernel, not assumed interchangeable with all kernels.
