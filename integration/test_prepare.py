"""Bounded host tests: python3 -I -B /path/to/integration/test_prepare.py."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().with_name('prepare.py')
spec = importlib.util.spec_from_file_location('integration_prepare', SCRIPT)
p = importlib.util.module_from_spec(spec)
spec.loader.exec_module(p)


class DeltaTest(unittest.TestCase):
    def setUp(self):
        self.policy = '\n'.join((
            '(typeattributeset domain (zramswap other_domain))', p.NEVERALLOW,
            '(allow zramswap extm_wear_authority (file (getattr open read)))',
            '(allow zramswap extm_wear_dir (dir (getattr open read search write add_name remove_name)))',
            '(typetransition init_29_0 zramswap_exec process zramswap)', *p.REMOVE,
        )) + '\n'
        self.contexts = '/data/vendor/lge-zram(/.*)? u:object_r:lge_zram_data_file:s0\n' + ''.join(
            f'{path}  {mode} u:object_r:{label}:s0\n' for path, (mode, label) in p.CONTEXTS.items()
        )
        self.platform = ('(type vendor_data_file)\n'
                         '(allow domain logdw_socket (sock_file (write)))\n'
                         '(allow domain logd (unix_dgram_socket (sendto)))\n'
                         '(allow domain exported_vold_prop (file (read getattr map open)))\n')

    def transform(self, **changes):
        args = dict(policy=self.policy, init='old vendor rc', root_init='old root rc',
                    contexts=self.contexts, platform=self.platform)
        args.update(changes)
        return p.transform(**args)

    def test_exact_delta(self):
        policy, init, root_init, contexts = self.transform()
        self.assertEqual(set(self.policy.splitlines()) - set(policy.splitlines()), set(p.REMOVE))
        self.assertEqual({x for x in set(policy.splitlines()) - set(self.policy.splitlines()) if x.startswith('(')}, set(p.additions()))
        self.assertIn(p.NEVERALLOW, policy)
        self.assertIn('(typeattributeset domain (zramswap other_domain))', policy)
        self.assertIn('(typetransition init_29_0 zramswap_exec process zramswap)', policy)
        self.assertNotIn('zramswap_exec process lge_zram_bootstrap', policy)
        self.assertEqual(init, p.VENDOR_INIT)
        self.assertEqual(root_init, p.ROOT_INIT)
        self.assertIn('on zygote-start\n    exec_start lge-zram-bootstrap', root_init)
        self.assertIn('on property:sys.boot_completed=1 && property:init.svc.qcom-post-boot=stopped', init)
        self.assertNotIn('on post-fs-data', root_init)
        self.assertIn('seclabel u:r:lge_zram_bootstrap:s0', root_init)
        for command in ('mkdir', 'restorecon', 'chown', 'chmod', 'setprop'):
            self.assertNotIn(f'\n    {command} ', root_init)
        self.assertNotIn('/mnt/vendor/persist', contexts)
        self.assertNotIn('/data/extm', contexts)
        self.assertIn('/data/vendor/lge-zram(/.*)? u:object_r:lge_zram_data_file:s0', contexts)
        extm = [line.split() for line in contexts.splitlines() if line.startswith('/data/vendor/extm')]
        self.assertEqual(len(extm), len(p.CONTEXTS))
        self.assertEqual({(path, qualifier, label) for path, qualifier, label in extm}, {
            ('/data/vendor/extm' + suffix, '-d', f'u:object_r:{label}:s0')
            for suffix, label in (('', 'extm_root_data'), ('/state', 'extm_wear_dir'))
        } | {
            ('/data/vendor/extm' + suffix, '--', f'u:object_r:{label}:s0')
            for suffix, label in (
                ('/owner', 'extm_owner_data'), ('/file', 'extm_backing_data'),
                ('/state/authority', 'extm_wear_authority'), ('/state/pause', 'extm_wear_pause'),
                *(('/state/' + name, 'extm_wear_ledger') for name in (
                    'ledger', r'ledger\.new', r'ledger\.txn', r'ledger\.v1\.bak',
                    r'exec\.intent', 'boot-state', r'boot-state\.new', r'boot-state\.txn',
                )),
            )
        })

    def test_guards(self):
        for rule in p.REMOVE:
            for policy in (self.policy.replace(rule + '\n', ''), self.policy + rule + '\n'):
                with self.subTest(rule=rule), self.assertRaises(ValueError):
                    self.transform(policy=policy)
        for changes in (dict(platform=''), dict(policy=self.policy.replace(p.NEVERALLOW, '')), dict(contexts='')):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                self.transform(**changes)
        for contexts in (
            self.contexts + '/data/vendor/extm(/.*)? u:object_r:extm_root_data:s0\n',
            self.contexts + '/data/extm -d u:object_r:extm_root_data:s0\n',
            self.contexts.replace('extm_backing_data', 'extm_root_data'),
            self.contexts.replace('/data/extm  -d', '/data/extm  --'),
            '\n'.join(self.contexts.splitlines()[:-1]) + '\n',
        ):
            with self.subTest(contexts=contexts), self.assertRaises(ValueError):
                self.transform(contexts=contexts)
        policy, init, root_init, contexts = self.transform()
        with self.assertRaises(ValueError):
            self.transform(policy=policy, init=init, root_init=root_init, contexts=contexts)

    def test_bootstrap_minimum_access(self):
        rules = p.additions()
        b = 'lge_zram_bootstrap'
        self.assertIn(p.allow(b, 'system_data_file', 'dir', 'getattr search'), rules)
        self.assertIn(p.allow(b, 'vendor_data_file', 'dir', 'getattr search open read write add_name ioctl'), rules)
        self.assertIn(f'(typetransition {b} vendor_data_file dir "extm" extm_root_data)', rules)
        self.assertFalse(any('typetransition' in r and ' process ' in r for r in rules))
        for domain in ('zramswap', 'extm_file_helper'):
            self.assertIn(p.allow(domain, 'vendor_data_file', 'dir', 'getattr search open read'), rules)
            self.assertTrue(any(r.startswith(f'(neverallow {domain} vendor_data_file ') and 'write' in r for r in rules))
        self.assertFalse(any('lge_zram_bootstrap extm_wear_ledger' in r for r in rules))
        for rule in rules:
            if rule.startswith('(allow lge_zram_bootstrap'):
                self.assertNotIn('setattr', rule)
                self.assertNotIn('relabelfrom', rule)
                self.assertNotIn('relabelto', rule)
                if 'blk_file' in rule:
                    self.assertTrue(rule.endswith('(getattr)))'))
        self.assertFalse(any('data_between_core_and_vendor_violators' in r for r in rules))
        self.assertEqual([r for r in rules if r.startswith('(allowx ')], [
            '(allowx lge_zram_bootstrap vendor_data_file (ioctl dir (0x6615)))',
            '(allowx lge_zram_bootstrap extm_root_data (ioctl dir (0x6615)))',
            '(allowx lge_zram_bootstrap extm_wear_dir (ioctl dir (0x6615)))'])

    def test_cli_rejects_wrong_baselines(self):
        with tempfile.TemporaryDirectory(prefix='integration-test-') as tmp:
            root = Path(tmp)
            fake = root / 'input'
            fake.write_text('not the pinned baseline\n')
            out = root / 'candidate'
            cmd = [sys.executable, '-I', '-B', str(SCRIPT)]
            for name in (*p.BASELINES, 'platform', 'system_mapping', 'product', 'product_mapping', 'public', 'secilc'):
                cmd += ['--' + name.replace('_', '-'), str(fake)]
            cmd += ['--out', str(out)]
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('baseline SHA-256 mismatch', result.stderr)
            self.assertFalse(out.exists())


if __name__ == '__main__':
    unittest.main()
