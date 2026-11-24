# Host-only `/data/vendor/extm` integration

`prepare.py` uses the same exact source inputs and Android 10 platform/product/mapping/public cohort as the V4 candidate. Full SHA-256 pins are in the script. The script reads but does not modify those inputs, rejects mismatches or reapplication, and writes only to a new directory outside this repository. It does not install anything or contact a device.

## Reproduction

Use absolute paths to the original V4 inputs, not the generated V4 outputs. The output parent must exist; the output directory must not exist.

```sh
REPO=/absolute/path/to/lge-zramd
BASE=/private/path/to/v4-source-baseline
ROOT_INIT=/private/path/to/actual/system/etc/init/lge-extm-provision.rc
COHORT=/private/path/to/matched-policy-inputs
SECILC=/absolute/path/to/secilc
CHECKFC=/absolute/path/to/checkfc
OUT=/private/build/path/new-vendor-extm-candidate

python3 -I -B "$REPO/integration/test_prepare.py"
python3 -I -B "$REPO/integration/prepare.py" \
  --policy "$BASE/vendor_sepolicy.cil" \
  --init "$BASE/init.lge.svelte.rc" \
  --root-init "$ROOT_INIT" --contexts "$BASE/vendor_file_contexts" \
  --platform "$COHORT/system/etc/selinux/plat_sepolicy.cil" \
  --system-mapping "$COHORT/system/etc/selinux/mapping/29.0.cil" \
  --product "$COHORT/product/etc/selinux/product_sepolicy.cil" \
  --product-mapping "$COHORT/product/etc/selinux/mapping/29.0.cil" \
  --public "$COHORT/vendor/etc/selinux/plat_pub_versioned.cil" \
  --secilc "$SECILC" --checkfc "$CHECKFC" --out "$OUT"
```

`--checkfc` is optional; if omitted, the script reports NOT RUN. Compilation is mandatory: `secilc -m -M true -G -c 30` with all six CIL inputs and neverallow checks enabled. No `-N`, permissive conversion, retry weakening, or attribute stripping. Input hashes and validation logs are retained beside the output.

## Init and native contract

- Replace the **complete existing platform/system** provisioning rc at its system location, not vendor/odm. Its disabled oneshot bootstrap service runs `/vendor/bin/lge-zramd bootstrap --confirmed` via synchronous `exec_start` on `zygote-start`. On the matched `/init.rc`, both the normal and vold restart sequences enqueue `post-fs-data` before `zygote-start`; `post-fs-data` creates `/data/vendor` (`0771 root:root`) and runs `init_user0`. A separate event avoids depending on the parse order of `post-fs-data` actions. This does **not** promise to run before the primary zygote actions.
- The daemon still starts on `sys.boot_completed=1` and `init.svc.qcom-post-boot=stopped`. `exec_start` waits but does not propagate bootstrap success into the daemon trigger: the daemon still validates the state, authority, and boot prerequisites independently. No property mkdir/ack handshake, init mkdir, restorecon, chown, or chmod remains in the replacement provisioning rc.
- The bootstrap explicitly uses `u:r:lge_zram_bootstrap:s0` and the existing `zramswap_exec` entrypoint. The original default daemon transition to `zramswap` remains unchanged. There is no new core/vendor data violation attribute membership.
- `/data` is traversed by O_PATH, with no parent open/read or write grants. Bootstrap validates the existing `vendor_data_file` parent, then creates absent `extm` and `extm/state` with mode `0700` under named transitions; it does not repair existing metadata. The daemon and helper get read-only traversal of `vendor_data_file`; explicit legacy daemon write/add_name and the old `lge-zram` creation transition are removed, and new neverallows reject parent mutations and ioctl by those domains. Bootstrap alone is granted write/add_name for its named extm creation. SELinux grants on `vendor_data_file` are type-wide, not pathname-specific; native validates the direct parent and only attempts the fixed name `extm`.
- Bootstrap can call FS_IOC_GET_ENCRYPTION_POLICY on `/data/vendor`, `extm`, and `extm/state`, with exact ioctl xperm `0x6615` (the low 16 bits of the 4.9 UAPI request `0x400c6615`). No policy-set or key ioctl is granted. Native requires ENODATA; a successful policy read or unsupported probe must fail closed. Native reads of cold zram/sysfs, DM names, proc mount metadata, and block-device metadata do not grant bootstrap raw block I/O, DM writes, or sys_admin. Platform `domain` permissions supply logd and the `exported_vold_prop` read used for `ro.crypto.type`.
- Bootstrap creates `authority` with mode `0600`, content `extm-wear-v1\n`, and the exact named label. It can read/write/lock the authority, but cannot append, rename, unlink, change metadata, or relabel it. Preservation after initial creation depends on native exclusive creation and validation, not a temporal policy rule. The original daemon authority neverallow is retained. The daemon gains root-directory `lock` to take the extm lock before the authority lock; state transactions remain the daemon’s responsibility, not the bootstrap’s.
- Only the whitelisted exact file-context entries move: root directory, `owner`, `file`, the `state` directory, and its named authority, pause, ledger and boot-state objects. Directory/file mode qualifiers and literal-dot escaping are preserved. No recursive extm label or old path alias is added. Legacy ledger labels do not authorize migration. The helper’s `file` and the daemon’s `owner` transitions stay anchored to the private extm root type. Old explicit persist access and legacy init transitions/grants for provisioning are removed.

## Outputs and limits

The candidate contains `vendor_sepolicy.cil`, `precompiled_sepolicy`, `vendor_file_contexts`, `init.lge.svelte.rc`, `lge-extm-provision.rc`, and validation evidence. They belong with the matching native daemon/bootstrap and the helper fixed to `/data/vendor/extm/file`; do not mix old helper binaries, persist state, or provisioning rc files with this set. This tooling does not install or qualify the candidate.

Host compilation, context validation, and init syntax validation do not prove real boot ordering, runtime labels, fscrypt results, or successful zram startup. Init syntax validation requires a separate `host_init_verifier`; inspect the captured root init sequence separately for ordering. No device write or migration is part of this procedure.
