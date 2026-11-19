# Host-only integration delta

`prepare.py` consumes the pinned demand vendor policy (`baf9125d…`), order-fixed
packaging init (`cb43ab19…`) and phase-1 packaging contexts (`69bd187d…`). Full
SHA-256 pins are in the script. Inputs are read-only; mismatches and reapplication
fail closed. Generated full OEM files belong in a **new directory outside git**,
not in this repository or a redistribution bundle.

## Commands

Set these sanitized placeholders to absolute local paths. The five cohort inputs
must come from the same matched platform/product/mapping/public policy set; do not
substitute unrelated firmware files. `OUT` must not exist; its parent must exist.

```sh
REPO=/absolute/path/to/lge-zramd
POLICY=/private/path/to/pinned-demand/vendor_sepolicy.cil
INIT=/private/path/to/pinned-order-fixed/init.lge.svelte.rc
CONTEXTS=/private/path/to/pinned-phase1/vendor_file_contexts
COHORT=/private/path/to/matched-policy-inputs
SECILC=/absolute/path/to/secilc
OUT=/private/build/path/new-integration-output

python3 -I -B "$REPO/integration/test_prepare.py"
python3 -I -B "$REPO/integration/prepare.py" \
  --policy "$POLICY" --init "$INIT" --contexts "$CONTEXTS" \
  --platform "$COHORT/system/etc/selinux/plat_sepolicy.cil" \
  --system-mapping "$COHORT/system/etc/selinux/mapping/29.0.cil" \
  --product "$COHORT/product/etc/selinux/product_sepolicy.cil" \
  --product-mapping "$COHORT/product/etc/selinux/mapping/29.0.cil" \
  --public "$COHORT/vendor/etc/selinux/plat_pub_versioned.cil" \
  --secilc "$SECILC" --out "$OUT"
```

Compilation uses `secilc -m -M true -G -c 30` with all six policy inputs and
neverallow checks enabled. No `-N`, permissive conversion or retry weakening.
A compile failure leaves evidence in `OUT/compile.log`; use a new output path for
another run. Omitting the compiler/cohort options only prepares files and is **not**
a compile PASS. `--platform` is always required for inherited logger checks.

## Exact scope and compatibility

- Remove the init `on boot` block that creates/restores `/dev/lge-zram-manager`,
  its recursive file-context entry, its named directory transition, and the five
  direct runtime-type allow rules (daemon directory/file access, init
  directory/relabel access and daemon directory lock). Retain the inert type,
  attribute memberships and unrelated shared-parent permissions.
- Add exact file transitions and regular-file contexts under
  `/mnt/vendor/persist/lge-zram-wear/` for `boot-state`, `boot-state.new`,
  `boot-state.txn` and `ledger.v1.bak`, all using `extm_wear_ledger`.
- Add only file `lock` for `zramswap` on `extm_wear_authority`; retain its mutation
  neverallow. No authority creation/write/append/unlink/rename/setattr grant.
  Verify inherited domain/logd rules; add no logger permissions.

The authority-file singleton implementation is **incompatible with the old
mkdir-based lock**. Treat the matching daemon, init, policy and contexts as one
integration set; do not mix lock generations or run old and new daemons together.
This script does not provision authority state, install files, contact a device
or perform git operations.

Host unit-test and full-policy compile success do not qualify boot behavior,
actual labels, persistent-state migration, logger delivery or device safety.
The SELinux compile does not validate the generated init or file-context files
with Android's separate validators. Device/deployment acceptance remains separate.
