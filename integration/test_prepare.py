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
            '(type lge_zram_runtime_file)',
            '(typeattributeset file_type (lge_zram_runtime_file))',
            '(typeattributeset domain (zramswap other_domain))',
            p.NEVERALLOW,
            '(allow zramswap extm_wear_authority (file (getattr open read)))',
            '(allow zramswap device_29_0 (dir (write add_name)))',
            *p.REMOVE,
        )) + '\n'
        self.init = 'service lge-zramd /vendor/bin/lge-zramd daemon\n\n' + p.INIT_BLOCK
        self.contexts = p.RUNTIME_CONTEXT + '\n# unrelated context retained\n'
        self.platform = '\n'.join(p.LOGGER)

    def transform(self, **changes):
        args = dict(policy=self.policy, init=self.init, contexts=self.contexts,
                    platform=self.platform)
        args.update(changes)
        return p.transform(**args)

    def test_exact_delta_and_no_attribute_stripping(self):
        policy, init, contexts = self.transform()
        removed = set(self.policy.splitlines()) - set(policy.splitlines())
        added = set(policy.splitlines()) - set(self.policy.splitlines())
        self.assertEqual(removed, set(p.REMOVE))
        self.assertEqual({x for x in added if x.startswith('(')}, set(p.ADDITIONS))
        self.assertIn(p.NEVERALLOW, policy)
        self.assertIn('(typeattributeset file_type (lge_zram_runtime_file))', policy)
        self.assertEqual(init, self.init.replace(p.INIT_BLOCK, ''))
        self.assertNotIn('/dev/lge-zram-manager', contexts)
        lines = contexts.splitlines()
        self.assertEqual(len(lines), 5)
        for name in p.NAMES:
            self.assertIn('/mnt/vendor/persist/lge-zram-wear/' + name.replace('.', r'\.')
                          + ' -- u:object_r:extm_wear_ledger:s0', lines)

    def test_guard_missing_or_duplicate_runtime_rules(self):
        for rule in p.REMOVE:
            for policy in (self.policy.replace(rule + '\n', ''), self.policy + rule + '\n'):
                with self.subTest(rule=rule), self.assertRaises(ValueError):
                    self.transform(policy=policy)

    def test_guard_invariants_and_reapplication(self):
        for changes in (
            dict(platform=''),
            dict(policy=self.policy.replace(p.NEVERALLOW, '')),
            dict(policy=self.policy.replace('(zramswap other_domain)', '(other_domain)')),
            dict(policy=self.policy + '(allow other lge_zram_runtime_file (file (read)))\n'),
            dict(init=''), dict(init=self.init + p.INIT_BLOCK), dict(contexts=''),
        ):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                self.transform(**changes)
        policy, init, contexts = self.transform()
        with self.assertRaises(ValueError):
            self.transform(policy=policy, init=init, contexts=contexts)

    def test_cli_rejects_wrong_baselines_without_output(self):
        with tempfile.TemporaryDirectory(prefix='integration-test-') as tmp:
            root = Path(tmp)
            fake = root / 'input'
            fake.write_text('not the pinned baseline\n')
            out = root / 'candidate'
            result = subprocess.run(
                [sys.executable, '-I', '-B', str(SCRIPT),
                 '--policy', str(fake), '--init', str(fake), '--contexts', str(fake),
                 '--platform', str(fake), '--out', str(out)],
                capture_output=True, text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('baseline SHA-256 mismatch', result.stderr)
            self.assertFalse(out.exists())


if __name__ == '__main__':
    unittest.main()
