# Process-local MMD-style budget and no-/dev startup state

This describes a source candidate, not an installed-device result. Historical wear-v2/v3 documents describe superseded persistent-accounting designs and are not the current production policy.

## Budget transition

The production daemon starts a fresh in-memory rolling write history when its controller object is initialized. It neither opens nor migrates `ledger` or `ledger.v1.bak`, and never inherits their historical deductions. Missing, malformed or exhausted old budget files alone do not block operation. Old files remain untouched; no automatic deletion or overwrite is needed.

`daily_mib` still caps the object's rolling history, and each request retains a finite kernel limit. Known submissions are charged at completion; unknown results retain the full grant and halt. Fifteen-minute buckets expire at bucket end plus24h, at most15min conservatively late. This is inspired by AOSP mmd, with bounded aggregation rather than its exact event queue. It is NOT a cross-restart daily hard ceiling: controller-object recreation resets accounting and frequent restarts can increase calendar-day writes.

No budget parsing, version migration, immutable backup creation, settlement disk transaction or periodic checkpoint runs in production. Legacy budget sources/tests are explicitly excluded from the production manifest, retained only for regression history.

## Safety state is not budget history

The current daemon keeps immutable `authority`, compact `boot-state`, pause and `exec.intent` under `/data/vendor/extm/state`. These protect singleton ownership, VM baseline/target, activation identity and unknown execution outcomes. They are not restored write budgets.

Moving from the old persist directory is a cold, coordinated transition. Stop the old controller and verify that it has no unresolved execution intent, incomplete activation or accounting transaction, or active pause that would be lost in the new directory. Old state remains untouched: neither a pause nor an uncertainty fence in the old directory is automatically carried forward. Do not proceed when the old state cannot be reconciled; do not clear evidence or replay an unknown request to force progress.

The authority lock does not interoperate with the old authority directory or the original mkdir-based `/dev` lock. Use the matching init, SELinux and file-context changes described in `integration/README.md`, and the matching backend helper for `/data/vendor/extm/file`. Do not run both controllers or blindly reapply the hash-pinned integration transform.

## Installation boundary

Cold-back up the current binary/config/integration cohort and all persistent evidence before any installation. Preserve customized config assignments. Do not hot-swap old and new daemon instances, clear active pause, discard unknown intents or copy a stale ledger over newer evidence. Old budget contents may be archived separately only under explicit authorization, not to force progress.

After installation verify ordinary swap, correct DM identity, one lock owner, VM×2 unchanged and absent /dev controller directory. Status intentionally reports `running_daemon_budget=unknown` because a separate CLI process does not own the daemon's history; use daemon logcat events for actual local counters. Verify actual write/read behavior separately. Budget exhaustion never disables existing backend reads.

This source work does not install, reboot, delete device files, commit or push changes.
