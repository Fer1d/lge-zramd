#!/usr/bin/env python3
"""Pinned /data/vendor/extm integration. Host only; no install/device/git actions.
Run python3 -I -B. Outputs require a new directory outside the repository.
Neverallow compilation is mandatory; file-context validation optional with checkfc.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys

BASELINES = {
    'policy': '433b54c0a53f316c0728e0ef3d63bf3c5895217e754e74cc190b5e2ea48c60ab',
    'init': '1c6da3616377b808e9607dda11f66539890b3d8848d1b6d84162e8c1dfcb716b',
    'root_init': 'e3a6c2c819dbc4fcee19600bba10ac198ece64440a25b9c3e42b01ae8210142f',
    'contexts': '93ed7cf1aefbdc3c6d97db903e9672036f69e9954f169b77e2245f0f9187e363',
}
# Loader inputs from the same cohort used by V4; a successful compile alone
# does not demonstrate that an arbitrary set of platform inputs is matched.
COHORT = {
    'platform': 'b74cc84a3a9cfc8cb0779b8869599d9dd2bc22d756ba5ff6840b52f7b7e7347c',
    'system_mapping': 'f9acac92e6ffb03353cd94aa7168569f4e2f6220978fbc160317cda39bcf63d0',
    'product': 'b335f0020e460eb48fa46c2f95767532b52af1baec30b488dde5d1f76517ae81',
    'product_mapping': '389d35ca1354c20203d9fb53222d3789380d90fe2045234c1ec279c43c6bea50',
    'public': '2f19f8fbe97dfa855eafa7911bf98496c574852489b9bb0c13b45d47b35a7040',
}
NEVERALLOW = '(neverallow zramswap extm_wear_authority (file (create write append unlink rename setattr)))'
REMOVE = (
    '(allow zramswap mnt_vendor_file (dir (getattr search)))',
    '(allow zramswap persist_file (dir (getattr search)))',
    '(allow zramswap mnt_vendor_file (dir (getattr search open read)))',
    '(allow zramswap persist_file (dir (getattr search open read)))',
    '(allow zramswap persist_block_device (blk_file (getattr)))',
    '(allow zramswap extm_provision_request_prop (property_service (set)))',
    '(allow zramswap extm_provision_request_prop (file (getattr open read map)))',
    '(allow zramswap extm_provision_ack_prop (file (getattr open read map)))',
    '(allow zramswap property_socket (sock_file (write)))',
    '(allow zramswap init (unix_stream_socket (connectto)))',
    '(allow zramswap vendor_data_file_29_0 (dir (write add_name)))',
    '(typetransition zramswap vendor_data_file_29_0 dir "lge-zram" lge_zram_data_file)',
    '(typetransition init system_data_file dir "extm" extm_root_data)',
    '(typetransition init extm_root_data file "owner" extm_owner_data)',
    '(typetransition init persist_file dir "lge-zram-wear" extm_wear_dir)',
    '(typetransition init mnt_vendor_file dir "lge-zram-wear" extm_wear_dir)',
    '(typetransition init extm_wear_dir file "authority" extm_wear_authority)',
    '(typetransition init extm_wear_dir file "ledger" extm_wear_ledger)',
    '(allow init extm_root_data (dir (create getattr setattr search open read write add_name relabelto relabelfrom)))',
    '(allow init extm_backing_data (file (getattr relabelto relabelfrom)))',
    '(allow init extm_owner_data (file (create getattr open write setattr relabelfrom relabelto)))',
    '(allow init extm_wear_dir (dir (create getattr setattr search relabelto relabelfrom)))',
    '(allow init extm_wear_authority (file (create getattr open write setattr relabelto relabelfrom)))',
    '(allow init extm_wear_ledger (file (create getattr open write setattr relabelto relabelfrom)))',
    '(allow init extm_wear_pause (file (getattr relabelto relabelfrom)))',
    '(allow init extm_provision_request_prop (file (getattr open read map)))',
    '(allow init extm_provision_ack_prop (file (getattr open read map)))',
    '(allow init extm_provision_ack_prop (property_service (set)))',
)
OLD_EXTM = '/data/extm'
OLD_WEAR = '/mnt/vendor/persist/lge-zram-wear'
EXTM = '/data/vendor/extm'
# Exact path/qualifier/label whitelist for the V4 input. Old ledger contexts
# remain a legacy fence only; they do not authorize migration.
CONTEXTS = {
    OLD_EXTM: ('-d', 'extm_root_data'),
    OLD_EXTM + '/file': ('--', 'extm_backing_data'),
    OLD_EXTM + '/owner': ('--', 'extm_owner_data'),
    OLD_WEAR: ('-d', 'extm_wear_dir'),
    OLD_WEAR + '/authority': ('--', 'extm_wear_authority'),
    OLD_WEAR + '/pause': ('--', 'extm_wear_pause'),
    **{OLD_WEAR + '/' + name: ('--', 'extm_wear_ledger') for name in (
        'ledger', r'ledger\.new', r'ledger\.txn', r'ledger\.v1\.bak',
        r'exec\.intent', 'boot-state', r'boot-state\.new', r'boot-state\.txn',
    )},
}
ROOT_INIT = '''# Platform/root-system service; replace the complete old provisioning rc.
# No property mkdir handshake, restorecon, chown or chmod of existing objects.
service lge-zram-bootstrap /vendor/bin/lge-zramd bootstrap --confirmed
    user root
    group root
    disabled
    oneshot
    seclabel u:r:lge_zram_bootstrap:s0

# The matched /init.rc queues this after post-fs-data creates /data/vendor
# and initializes user data. Do not rely on same-event rc import ordering.
on zygote-start
    exec_start lge-zram-bootstrap
'''
VENDOR_INIT = '''# Replace complete old vendor rc; do not import alongside it.
service lge-zramd /vendor/bin/lge-zramd daemon --confirmed --boot-completed
    user root
    group root system
    disabled
    oneshot
    # Default init -> zramswap_exec -> zramswap remains unchanged.

# Bootstrap synchronously runs on zygote-start, after post-fs-data.
# Native checks immutable authority; cold acquisition requires no bootstrap-result.
on property:sys.boot_completed=1 && property:init.svc.qcom-post-boot=stopped
    start lge-zramd
'''


def require(ok, message):
    if not ok:
        raise ValueError(message)


def once(text, old, new=''):
    require(text.count(old) == 1, 'expected one exact baseline fragment: ' + old)
    return text.replace(old, new, 1)


def allow(source, target, cls, perms):
    return f'(allow {source} {target} ({cls} ({perms})))'


def additions():
    b = 'lge_zram_bootstrap'
    rules = [f'(type {b})', f'(typeattributeset domain ({b}))',
             allow('init', b, 'process', 'transition siginh rlimitinh'),
             allow(b, 'init', 'process', 'sigchld'),
             allow(b, 'zramswap_exec', 'file', 'getattr open read map execute entrypoint'),
             allow(b, 'system_data_file', 'dir', 'getattr search'),
             allow(b, 'vendor_data_file', 'dir', 'getattr search open read write add_name ioctl'),
             '(typetransition lge_zram_bootstrap vendor_data_file dir "extm" extm_root_data)',
             '(typetransition lge_zram_bootstrap extm_root_data dir "state" extm_wear_dir)',
             '(typetransition lge_zram_bootstrap extm_wear_dir file "authority" extm_wear_authority)',
             allow(b, 'extm_root_data', 'dir', 'create getattr open read search write add_name lock ioctl'),
             allow(b, 'extm_wear_dir', 'dir', 'create getattr open read search write add_name ioctl'),
             allow(b, 'extm_wear_authority', 'file', 'create getattr open read write lock'),
             allow('zramswap', 'vendor_data_file', 'dir', 'getattr search open read'),
             allow('extm_file_helper', 'vendor_data_file', 'dir', 'getattr search open read'),
             allow('zramswap', 'extm_root_data', 'dir', 'lock')]
    for target in ('vendor_data_file', 'extm_root_data', 'extm_wear_dir'):
        rules.append(f'(allowx {b} {target} (ioctl dir (0x6615)))')
    for target in ('vendor_file', 'system_linker_exec', 'system_lib_file',
                   'same_process_hal_file', 'vndk_sp_file'):
        rules.append(allow(b, target, 'file', 'getattr open read map execute'))
    for target in ('rootfs', 'proc', 'sysfs', 'sysfs_dm', 'sysfs_zram', 'block_device', 'device'):
        rules.append(allow(b, target, 'dir', 'getattr open read search'))
    for target in ('proc', 'proc_swaps', 'sysfs', 'sysfs_dm', 'sysfs_zram', 'extm_zram_backing_attr'):
        rules.append(allow(b, target, 'file', 'getattr open read'))
    for target in ('proc', 'sysfs', 'block_device'):
        rules.append(allow(b, target, 'lnk_file', 'getattr read'))
    for target in ('block_device', 'userdata_block_device', 'lge_zram0_device'):
        rules.append(allow(b, target, 'blk_file', 'getattr'))
    rules += [f'(neverallow {b} system_data_file (dir (open read write add_name remove_name ioctl)))',
              f'(neverallow {b} vendor_data_file (dir (create setattr remove_name reparent rename rmdir relabelfrom relabelto)))',
              '(neverallow zramswap vendor_data_file (dir (create write add_name remove_name setattr ioctl reparent rename rmdir relabelfrom relabelto)))',
              '(neverallow extm_file_helper vendor_data_file (dir (create write add_name remove_name setattr ioctl reparent rename rmdir relabelfrom relabelto)))',
              f'(neverallow {b} extm_root_data (dir (setattr remove_name rmdir relabelfrom relabelto)))',
              f'(neverallow {b} extm_wear_authority (file (setattr append unlink rename relabelfrom relabelto)))',
              f'(neverallow {b} userdata_block_device (blk_file (open read write ioctl)))']
    return rules


def transform(policy, init, root_init, contexts, platform):
    require(NEVERALLOW in policy.splitlines(), 'daemon authority neverallow missing')
    require('lge_zram_bootstrap' not in policy, 'delta already present')
    for rule in REMOVE:
        policy = once(policy, rule + '\n')
    require('(type vendor_data_file)' in platform, 'missing platform vendor data parent type')
    for fragment in ('(allow domain logdw_socket (sock_file (write)))',
                     '(allow domain logd (unix_dgram_socket (sendto)))',
                     '(allow domain exported_vold_prop (file (read getattr map open)))'):
        require(fragment in platform, 'missing inherited platform permission: ' + fragment)
    policy += '\n; Native bootstrap only; no broad attribute rewrites or daemon authority mutations.\n'
    policy += '\n'.join(additions()) + '\n'
    seen = set()
    for line in contexts.splitlines():
        if line.startswith((OLD_EXTM, OLD_WEAR, EXTM)):
            fields = line.split()
            path = fields[0]
            require(path in CONTEXTS and path not in seen, 'unexpected or duplicate extm context: ' + path)
            qualifier, label = CONTEXTS[path]
            require(fields[1:] == [qualifier, f'u:object_r:{label}:s0'], 'unexpected extm context: ' + line)
            seen.add(path)
            old, new = (OLD_EXTM, EXTM) if path.startswith(OLD_EXTM) else (OLD_WEAR, EXTM + '/state')
            contexts = once(contexts, line + '\n', line.replace(old, new, 1) + '\n')
    require(seen == set(CONTEXTS), 'missing extm contexts: ' + ', '.join(sorted(set(CONTEXTS) - seen)))
    return policy, VENDOR_INIT, ROOT_INIT, contexts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in (*BASELINES, *COHORT, 'secilc', 'out'):
        parser.add_argument('--' + name.replace('_', '-'), required=True, type=Path)
    parser.add_argument('--checkfc', type=Path)
    args = parser.parse_args()
    require(sys.flags.isolated, 'run with python3 -I -B')
    texts = {}
    provenance = {}
    inputs = []
    for name, expected in (BASELINES | COHORT).items():
        path = getattr(args, name).resolve(strict=True)
        data = path.read_bytes()
        source = 'cohort' if name in COHORT else 'baseline'
        require(hashlib.sha256(data).hexdigest() == expected, f'{source} SHA-256 mismatch: {name}')
        if name in BASELINES or name == 'platform':
            texts[name] = data.decode('utf-8')
        if name in COHORT:
            inputs.append(path)
        provenance[str(path)] = expected
    outputs = transform(**texts)
    out = args.out.resolve()
    require(not out.is_relative_to(Path(__file__).resolve().parent.parent), 'output must be outside repository')
    require(not out.exists(), 'output directory must not exist')
    compiler = args.secilc.resolve(strict=True)
    out.mkdir(mode=0o700)
    for name, text in zip(('vendor_sepolicy.cil', 'init.lge.svelte.rc', 'lge-extm-provision.rc', 'vendor_file_contexts'), outputs):
        (out / name).write_text(text)
    (out / 'input-sha256.txt').write_text(''.join(f'{digest}  {path}\n' for path, digest in provenance.items()))
    command = [str(compiler), '-m', '-M', 'true', '-G', '-c', '30', *map(str, inputs),
               str(out / 'vendor_sepolicy.cil'), '-o', str(out / 'precompiled_sepolicy'),
               '-f', str(out / 'compiler.fc')]
    with (out / 'compile.log').open('wb') as log:
        result = subprocess.run(command, cwd=Path(__file__).resolve().parent, stdin=subprocess.DEVNULL,
                                stdout=log, stderr=subprocess.STDOUT, timeout=120)
    require(result.returncode == 0, 'secilc failed; inspect compile.log; neverallow checks not weakened')
    if args.checkfc:
        with (out / 'checkfc.log').open('wb') as log:
            result = subprocess.run([str(args.checkfc.resolve(strict=True)), str(out / 'precompiled_sepolicy'),
                                     str(out / 'vendor_file_contexts')], cwd=Path(__file__).resolve().parent,
                                    stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, timeout=120)
        require(result.returncode == 0, 'checkfc failed')
    print(str(out))
    print('Full-cohort secilc PASS with neverallows; host-only, not device qualified.')
    print('checkfc: ' + ('PASS' if args.checkfc else 'NOT RUN (tool not supplied)'))


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.TimeoutExpired) as error:
        sys.exit(str(error))
