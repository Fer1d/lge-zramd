# lge-zramd — G7 native zram controller

Source and host tests for the Plan-B kernel-demand controller. Companion kernel sources are in [Fer1d/android_kernel_lge_sdm845, branch 10](https://github.com/Fer1d/android_kernel_lge_sdm845/tree/10): `drivers/block/zram` and `include/uapi/linux/zram_policy.h`. This is not a complete firmware installer.

## Modules

- `swap.c`, `vm.c`, `lifecycle.c`: ordinary swap and VM setup; pause does not disable basic swap.
- `dm_backend.c`, `helper.c`: existing F2FS extent-backed DM acquisition and cold binding, with zero quota during setup. A compatible separately provisioned extm-file-helper is required; its source is not included here.
- `demand_watch.c`, `demand_exec.c`: physical-pool notifications and cold-page requests. Userspace approves budgeted finite execution; kernel policy itself submits no unsolicited writes.
- `ledger.c`, `io.c`, `intent.c`: durable accounting and unknown-outcome evidence. Never overwrite new charges with an older ledger or delete unknown intents to force progress.

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

Ledger checks require the entire new file plus 1MiB reserve before demand intent and again before commit; concurrent ENOSPC is still possible and handled as failure. wear-v1 preserves all history and generation==record count. Full-history rewrite cost remains; automatic compaction is not implemented. Frozen-highwater accounting does not replenish credits merely because the date or boot changes: `daily_mib` is not a completed automatically renewing daily budget.

Legacy mm_stat remains a paired-kernel ten-field contract, not a cross-vendor ABI detector; unknown widths fail unsupported. Demand uses STATUS/pool_demand. writeback_limit units are 4KiB independent of actual page size.

The demand service was observed enabled/listening before these repairs, but first natural physical UFS demand execution was not demonstrated in that observation. Repaired binaries were host-tested and built, not installed at publication time.

## Publication scope

Only source, tests and configuration are included. No OEM binaries, live ledgers, private identifiers, device logs, keys or backups. Preserve applicable source notices; no new license grant is inferred for external/OEM dependencies.
