# lge-zramd — G7 native zram controller

Source and host tests for the Plan-B kernel-demand controller. Companion kernel sources are in [Fer1d/android_kernel_lge_sdm845, branch 10](https://github.com/Fer1d/android_kernel_lge_sdm845/tree/10): `drivers/block/zram` and `include/uapi/linux/zram_policy.h`. This is not a complete firmware installer.

## Modules

- `swap.c`, `vm.c`, `lifecycle.c`: ordinary swap and VM setup; pause does not disable basic swap.
- `dm_backend.c`, `helper.c`: existing F2FS extent-backed DM acquisition and cold binding, with zero quota during setup. A compatible separately provisioned extm-file-helper is required; its source is not included here.
- `demand_watch.c`, `demand_exec.c`: physical-pool notifications and cold-page requests. Userspace approves budgeted finite execution; kernel policy itself submits no unsolicited writes.
- `memory_budget.c`: bounded process-local rolling write history; controller-object restart deliberately resets accounting.
- `wear_budget.c`, `wear_store.c`, `ledger.c`: archived persistent-budget regression modules, excluded from production sources.
- `boot_state.c`: authority-file singleton lock and one compact boot-bound startup checkpoint; no controller runtime directory under `/dev`.
- `io.c`, `intent.c`: durable replacement and unknown-outcome evidence. Never overwrite new charges with an older ledger or delete unknown intents to force progress.

The Chinese configuration retains the deployed profile's 23 values, including `vm_minfree_mult=2`. It is read once at startup. Matching init, SELinux policy and file-context integration must be supplied separately. This import does not redistribute the device's compiled platform policy.

## Build

Linux x86_64 prerequisites: C compiler, Python3, strace, coreutils and Android NDK r27c.

```sh
export NDK=/absolute/path/to/android-ndk-r27c
bash build-demand.sh
bash build-and-test.sh
```

Outputs are ignored under `build/`. The opt-in demand candidate is `build/demand/lge-zramd-candidate.android29.arm64`; do not confuse it with the historical disarmed aggregate artifact. `NZM_DEMAND_RUNTIME_QUALIFIED` is a build interlock, not runtime proof.

## Validation and limitations

Normal and ASan/UBSan suites passed before import, covering helper ownership/late reap, ledger space and commit faults, request transport and 4/16/64KiB page-unit fixtures. These are not hardware acceptance tests. TSan suites compiled; execution was skipped after independent probes reported unsupported memory mappings. An earlier probe exited139; neither result is TSan PASS.

Post-kill helper cleanup is bounded by 100ms/32 probes. Unresolved child ownership is process-local and blocks subsequent helper calls; reap is attempted on safe helper entries. This does not repair kernel deadlocks or replace persistent uncertainty handling across process death.

The budget follows Android mmd's process-local approach: `daily_mib` limits a rolling window inside one controller object, not a cross-reboot hard ceiling. At most97 fifteen-minute buckets retain completed writes; expiry is24h after bucket end, at most15min conservative delay compared with an exact event queue. CLOCK_BOOTTIME includes suspend; RTC is irrelevant. A fresh object starts empty, including after process restart. Frequent restarts can therefore increase calendar-day writes; this tradeoff is explicit.

An in-memory reservation bounds each request before finite kernel authorization. Known replies are charged by `submitted_pages * page_size` at completion, not installed pages. Zero submissions consume no write budget; submitted failed or partially installed writes remain charged. Unknown results retain the full in-memory grant and halt. Durable `exec.intent` remains independent: a fresh empty budget does not authorize replay or clearing an old intent.

Production does not open, parse, migrate, update or checkpoint historical `ledger`/`ledger.v1.bak`; missing, malformed or exhausted accounting alone does not block it. Old files are left untouched, not automatically deleted. Unfinished `exec.intent`, `ledger.new` or `ledger.txn` still fences demand as unresolved execution evidence while basic swap remains governed by existing startup rules. Startup checkpoint and intent fsync remain necessary safety operations; there are no per-batch budget disk transactions or periodic budget writes.

Memory budget stays in `struct nzm_core`, not temporary per-call state. `sources.sh` explicitly separates production from legacy regression sources, with `NZM_LEGACY_TEST` enabling historical entrypoints only in tests. Separate CLI status reports `running_daemon_budget=unknown`, not its own empty history as if it belonged to the live daemon. Daemon logs expose actual process-local counters; no new IPC or /dev directory is introduced.

Known zero-progress execution uses exponential retry delays from max(60 seconds, cold-page age), capped at thirty minutes; demand sequence refreshes cannot bypass the delay. Budget expiry permits reevaluation without a new sequence. Unknown execution outcomes halt rather than retry. Events are written explicitly to Android logcat with tag `lge-zramd`; host stderr is diagnostic only.

Legacy mm_stat remains a paired-kernel ten-field contract, not a cross-vendor ABI detector; unknown widths fail unsupported. Demand uses STATUS/pool_demand. writeback_limit units are 4KiB independent of actual page size.

The demand service was observed enabled/listening before these repairs, but first natural physical UFS demand execution was not demonstrated in that observation. Repaired binaries were host-tested and built, not installed at publication time.

## Publication scope

Only source, tests and configuration are included. No OEM binaries, live ledgers, private identifiers, device logs, keys or backups. Preserve applicable source notices; no new license grant is inferred for external/OEM dependencies.
