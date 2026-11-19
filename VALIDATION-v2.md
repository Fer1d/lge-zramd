# wear-v2 / no-runtime-directory validation

Source candidate only: no device installation, ledger migration, reboot, git commit or push was performed for this change.

## Implementation

- Fixed-size wear-v2 budget: at most97 fifteen-minute buckets,24h after bucket end expiry, monotonic lifetime charge/reservation generation. CLOCK_BOOTTIME within boot, durable newboot reanchor with conservative lost tails; no RTC-derived credits.
- Original wear-v1 bytes backed up exclusively and verified before migration; ALL legacy cumulative charge held conservatively, no automatic refunds. Unknown intent or transaction residue prevents migration/EXEC.
- Outstanding budget checkpoints at30min, no ledger writes on ordinary30s health poll. Budget deadlines can wake execution without demand sequence change.
- Zero-progress backoff starts at execution completion, exponential max30min; sequence refresh does not bypass it. Four-success continuation bound retained. Unknown outcomes do not retry. Explicit Android log output and readonly budget/status reasons added.
- No production runtime-directory creation/use under /dev. Singleton lock uses immutable authority inode; startup facts are bounded boot-state checkpoint, ordinary diagnostics process-local. Sameboot active swap reconciliation never repeats activation; interrupted state remains fail-closed. VM baseline/target persists so ×2 does not become ×4.

## Tests

Final aggregate build-and-test.sh completed exit0 with the explicit NDK r27c path. Full log is ignored local build/v2-aggregate-complete.log. Ordinary and ASan/UBSan suites passed, including192247 budget checks,100-day bounded-history model, migration/store fault boundaries, checkpoint inode/flock/durability/no-space cases, readonly known-v2 status with zero writes, request transport3477 cases,4/16/64KiB fixtures and safe-spawn ownership/late reap. Real-main fixtures include18 startup and17 historical event cases per variant. Historical scheduler regressions remain explicit wear-v1 tests, not a production fallback.

Child syscall trace:106 real helper exec attempts passed allowlist/FD/signal-mask checks. This does not inject a kernel D-state or prove Android production behavior.

TSan suites compiled. Execution was SKIPPED due host unexpected-memory-mapping failures; a probe can pass while a suite fails at runtime startup. No race finding is classified as SKIP; unsupported results are NOT suite PASS. Prior failed integration/mid-edit build logs remain in ignored build/ and are not erased or represented as successes.

Integration transformation:4 tests passed. Matched complete platform/product/mapping/public plus transformed vendor policy compiled with secilc -m -M true -G -c30, neverallows enabled. Exact delta6 removals/5 additions preserves immutable-authority neverallow. Compiled policy SHA256941acfdbcb544c4bd709fc26e975010308eecf3fdf2e3df89a832aaee1c6371d; generated policy/init/context outputs kept outside repository. Separate Android init/file-context validators and device labels/boot were NOT validated.

## Android candidate

build/demand/lge-zramd-candidate.android29.arm64
Size106784 bytes
SHA256300dc04c9daa9cc9990e66f438a14d7c4a339aa7ed6c5f27c0fb84dacff9e796
Dependencies: liblog.so, libdl.so, libc.so.

Default disarmed and opt-in demand builds both compile. The qualified macro remains an integration interlock, not runtime proof.

## Deployment boundary

Do not replace the daemon alone. The new authority lock does not interoperate with the old mkdir lock. Matching init, contexts and policy changes, cold coherent backups and current config preservation are required. Current device watermarks/scanning choices were not overwritten. Repository config values remain unchanged; only obsolete budget comments were updated. No actual UFS write/read acceptance is claimed for this candidate. See MIGRATION.md and integration/README.md.
