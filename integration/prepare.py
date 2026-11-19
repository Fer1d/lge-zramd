#!/usr/bin/env python3
"""Host-only, one-shot delta for the pinned demand-policy/packaging baselines.

Run with python3 -I -B prepare.py --help. Inputs are never modified. --out must
be a NEW directory outside this repository; generated OEM files are private
build outputs, not redistribution artifacts. No install, device or git actions.
Optional compilation uses the supplied matched platform/product/mapping/public
cohort: secilc -m -M true -G -c 30 (neverallow checks enabled, no bypass).
Host success does not establish boot, labeling, log delivery or device safety.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys

BASELINES = {
    "policy": "baf9125d2730ec509b6fae781848b5687bb30a198e92ac32fcc726a413fb4e3d",
    "init": "cb43ab197ab5b2c15f619461ae074fd68e81b61d60af014472c297ba10ff7d85",
    "contexts": "69bd187d0385480412b36336d1d21fe06236eb78b68fc0af5441a86d9bfc7dd4",
}
# Exact runtime-target rules only. Keep the inert type declaration/membership;
# do not rewrite attribute expressions or unrelated shared-parent permissions.
REMOVE = (
    '(typetransition zramswap device_29_0 dir "lge-zram-manager" lge_zram_runtime_file)',
    '(allow zramswap lge_zram_runtime_file (dir (create read write getattr setattr open search add_name remove_name rmdir)))',
    '(allow zramswap lge_zram_runtime_file (file (create read write append getattr open setattr rename unlink)))',
    '(allow init_29_0 lge_zram_runtime_file (dir (create read write getattr setattr open search add_name remove_name relabelfrom relabelto)))',
    '(allow init_29_0 lge_zram_runtime_file (file (getattr relabelfrom relabelto)))',
    '(allow zramswap lge_zram_runtime_file (dir (lock)))',
)
NAMES = ("boot-state", "boot-state.new", "boot-state.txn", "ledger.v1.bak")
LOCK = '(allow zramswap extm_wear_authority (file (lock)))'
NEVERALLOW = '(neverallow zramswap extm_wear_authority (file (create write append unlink rename setattr)))'
ADDITIONS = (LOCK,) + tuple(
    f'(typetransition zramswap extm_wear_dir file "{name}" extm_wear_ledger)'
    for name in NAMES
)
RUNTIME_CONTEXT = '/dev/lge-zram-manager(/.*)?    u:object_r:lge_zram_runtime_file:s0'
INIT_BLOCK = ('on boot\n'
              '    mkdir /dev/lge-zram-manager 0700 root root\n'
              '    restorecon /dev/lge-zram-manager\n\n')
LOGGER = (
    '(allow domain logdw_socket (sock_file (write)))',
    '(allow domain logd (unix_dgram_socket (sendto)))',
    '(allow domain socket_device (dir (ioctl read getattr lock search open)))',
    '(allow domain self (unix_dgram_socket (ioctl read write create getattr setattr lock append map bind connect getopt setopt shutdown sendto)))',
)


def require(ok, message):
    if not ok:
        raise ValueError(message)


def once(text, old, new=""):
    require(text.count(old) == 1, "expected one exact baseline fragment: " + old)
    return text.replace(old, new, 1)


def transform(policy, init, contexts, platform):
    for rule in LOGGER:
        require(rule in platform.splitlines(), "missing inherited logger rule: " + rule)
    require(any(line.startswith('(typeattributeset domain (') and
                'zramswap' in line.replace('(', ' ').replace(')', ' ').split()
                for line in policy.splitlines()), "zramswap must inherit domain logger access")
    require(NEVERALLOW in policy.splitlines(), "authority mutation neverallow missing")
    for rule in ADDITIONS:
        require(rule not in policy, "delta already present")
    for rule in REMOVE:
        policy = once(policy, rule + '\n')
    require(not any('lge_zram_runtime_file' in line and
                    line.startswith(('(allow ', '(typetransition '))
                    for line in policy.splitlines()), "unexpected remaining runtime rule")
    policy += '\n; Authority flock and exact LCTX creation labels; no mutation grant.\n'
    policy += '\n'.join(ADDITIONS) + '\n'
    init = once(init, INIT_BLOCK)
    contexts = once(contexts, RUNTIME_CONTEXT + '\n')
    for name in NAMES:
        path = '/mnt/vendor/persist/lge-zram-wear/' + name.replace('.', r'\.')
        require(path + ' ' not in contexts, "state context already present")
        contexts += path + ' -- u:object_r:extm_wear_ledger:s0\n'
    require('/dev/lge-zram-manager' not in init + contexts, "runtime path remains")
    return policy, init, contexts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in (*BASELINES, 'platform', 'out'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--secilc', type=Path, help='optional host compiler executable')
    cohort = ('system-mapping', 'product', 'product-mapping', 'public')
    for name in cohort:
        parser.add_argument('--' + name, type=Path)
    args = parser.parse_args()
    require(sys.flags.isolated, 'run with python3 -I -B')
    compile_inputs = [getattr(args, name.replace('-', '_')) for name in cohort]
    require(all(compile_inputs) if args.secilc else not any(compile_inputs),
            'compilation requires secilc and the complete matched cohort')
    inputs = {name: getattr(args, name).resolve(strict=True) for name in BASELINES}
    texts = {}
    for name, path in inputs.items():
        data = path.read_bytes()
        require(hashlib.sha256(data).hexdigest() == BASELINES[name],
                'baseline SHA-256 mismatch: ' + name)
        texts[name] = data.decode('utf-8')
    platform = args.platform.resolve(strict=True)
    outputs = transform(**texts, platform=platform.read_text())
    out = args.out.resolve()
    repo = Path(__file__).resolve().parent.parent
    require(not out.is_relative_to(repo), 'output must be outside the repository')
    require(not out.exists(), 'output directory must not exist')
    if args.secilc:
        compiler = args.secilc.resolve(strict=True)
        compile_inputs = [p.resolve(strict=True) for p in compile_inputs]
    out.mkdir(parents=False, mode=0o700)
    for name, text in zip(('vendor_sepolicy.cil', 'init.lge.svelte.rc',
                           'vendor_file_contexts'), outputs):
        (out / name).write_text(text)
    print('Prepared exact delta; inherited logd permissions checked; no logger grant added.')
    if args.secilc:
        system_mapping, product, product_mapping, public = compile_inputs
        command = [str(compiler), '-m', '-M', 'true', '-G', '-c', '30',
                   *map(str, (platform, system_mapping, product, product_mapping,
                              public, out / 'vendor_sepolicy.cil')),
                   '-o', str(out / 'precompiled_sepolicy'), '-f', str(out / 'compiler.fc')]
        with (out / 'compile.log').open('wb') as log:
            result = subprocess.run(command, cwd=Path(__file__).resolve().parent, stdin=subprocess.DEVNULL,
                                    stdout=log, stderr=subprocess.STDOUT, timeout=120)
        require(result.returncode == 0,
                'secilc failed; inspect compile.log; no retry or neverallow weakening')
        print('Host secilc passed with neverallow checks enabled; NOT device-qualified.')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.TimeoutExpired) as error:
        sys.exit(str(error))
