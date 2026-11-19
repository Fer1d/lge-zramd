# MMD-style process-local budget candidate

This supersedes the persistent wear-v3 proposal for production. No device installation, reboot, accounting migration, ledger deletion, git commit or push occurred for this change. The device's installed wear-v2 controller remains unchanged by this source work.

## Behavior

Each nzm_core owns one memory_budget initialized once per object. Historical wear ledgers are not read, parsed, migrated, inherited, charged or written in production. Recreating the object resets budget history intentionally, including process/device restart. There is no cross-restart hard daily ceiling or emergency allowance.

Known results charge submitted_pages*page_size at completion, including submitted-but-failed/not-installed pages. Zero known submissions use no write budget. Unknown results retain full memory grant, halt and preserve durable exec.intent; an empty budget after restart does not clear execution uncertainty. Authority lock, boot-state, fixed VM×2 target, DM lease/identity and pause remain independent of accounting. Old ledger.new/ledger.txn and exec.intent are absence-only uncertainty fences, not restored budgets.

97 fifteen-minute buckets bound history; expiry is bucket-end+24h, at most15min conservative delay. CLOCK_BOOTTIME includes suspend. Page-aware deadlines walk expirations until at least one page fits; same-sequence expiry can retry, and deadlines crossed during checking cause bounded reevaluation, not fatal state or busy looping. Zero-progress backoff and finite kernel quotas remain. Status declares process_local/reset scope and running_daemon_budget unknown rather than reporting a separate CLI object's empty history as the live budget.

Explicit sources.sh and Android.bp production manifests match24 modules. Old ledger/wear_store/wear_budget, executor and event-runtime/PSI schedulers are excluded; historical regression entrypoints require NZM_LEGACY_TEST. Candidate symbol checks find no persistent-budget or old execution symbols. No /dev directory is restored. Startup checkpoint and intent fsync are correctness operations, not budget persistence.

## Validation

Final build-and-test.sh exit0, ignored local log build/mmd-aggregate-final.log. Ordinary and ASan/UBSan suites pass. Memory module153805 checks include100day bounded growth, object-reset empty history, zero/partial/full/failed submitted writes, nonaging unknown, multiple expiry/page rounding, overflow and clock regression. Production executor/watch fault matrix and4/16/64KiB grants pass. Startup/status tests cover no ledger opens, missing/corrupt/exhausted historical ledger ignored, unchanged old bytes, and transaction/intent fencing.22 startup and17 historical event cases per plain/sanitized variant pass; historical scheduling is explicitly v1 test-only, not production fallback.

106 actual helper execution child traces pass FD/signal/syscall allowlist.3477 request transport cases pass. TSan suites compile; runtime SKIPPED because host reports unexpected memory mapping, NOT PASS. Initial aggregate failed an old execution-gate fixture that bypassed core initialization and relied on selection resetting request IDs; fixture now initializes its object's ID explicitly. Failure log retained build/mmd-aggregate-first.log.

Config's23 assignments unchanged, comments updated for restart-reset policy. No new SELinux grants needed versus installed no-/dev integration. Separate init/context/device acceptance not rerun because no platform integration files changed here.

## API29 arm64 candidate

build/demand/lge-zramd-candidate.android29.arm64
Size94968bytes
SHA256908504070e3679a32e4affc4a2db336e8d832ce73afc7fffc16b2b45a258cd5b

Disarmed and opt-in builds succeed. Actual on-device write/read and reset-budget behavior remain unverified until a separately authorized coherent cold replacement.
