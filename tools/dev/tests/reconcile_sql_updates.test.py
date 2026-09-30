# This file is part of Project SkyFire https://www.projectskyfire.org.
# See LICENSE.md file for Copyright information
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('reconcile', Path(__file__).parents[1] / 'reconcile_sql_updates.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ReconcileTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='skyfire-sql-test-')
        self.root = Path(self.temp.name)
        self.git('init', '-q')
        self.git('config', 'user.email', 'test@example.invalid')
        self.git('config', 'user.name', 'SQL tests')
        self.old = 'sql/updates/world/2026_09_30_world_00.sql'
        self.put(self.old, b'SELECT 1;\n')
        self.git('add', '.')
        self.git('commit', '-qm', 'Existing clustering release')
        self.base = self.git('rev-parse', 'HEAD').decode().strip()

    def tearDown(self):
        self.temp.cleanup()

    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.root), *args])

    def put(self, path, data):
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)

    def pending(self):
        return list((self.root / 'sql/pending_updates/world').glob('*.sql'))

    def test_collision_preserves_existing_and_stages_incoming(self):
        incoming = 'sql/updates/world/2026_09_30_world_00_upstream.sql'
        self.put(incoming, b'SELECT 2;\n')
        self.assertTrue(module.reconcile(self.root, self.base))
        self.assertTrue((self.root / incoming).exists())
        self.assertFalse(self.pending())
        module.reconcile(self.root, self.base, True)
        self.assertFalse((self.root / incoming).exists())
        self.assertEqual(self.pending()[0].read_bytes(), b'SELECT 2;\n')
        self.assertEqual((self.root / self.old).read_bytes(), b'SELECT 1;\n')
        self.assertEqual(module.reconcile(self.root, self.base, True), [])

    def test_same_filename_overwrite(self):
        self.put(self.old, b'SELECT 3;\n')
        module.reconcile(self.root, self.base, True)
        self.assertEqual((self.root / self.old).read_bytes(), b'SELECT 1;\n')
        self.assertEqual(self.pending()[0].read_bytes(), b'SELECT 3;\n')

    def test_identical_content_is_not_reimported(self):
        incoming = 'sql/updates/world/2026_10_01_world_00.sql'
        self.put(incoming, b'SELECT 1;\r\n')
        module.reconcile(self.root, self.base, True)
        self.assertFalse((self.root / incoming).exists())
        self.assertFalse(self.pending())

    def test_domains_are_independent(self):
        incoming = 'sql/updates/hub/2026_09_30_hub_00.sql'
        self.put(incoming, b'SELECT 1;\n')
        self.assertEqual(module.reconcile(self.root, self.base, True), [])
        self.assertTrue((self.root / incoming).exists())

    def test_two_new_updates_with_same_slot(self):
        self.put('sql/updates/world/2026_10_01_world_00_a.sql', b'SELECT 2;')
        self.put('sql/updates/world/2026_10_01_world_00_b.sql', b'SELECT 3;')
        module.reconcile(self.root, self.base, True)
        self.assertEqual(len(self.pending()), 1)

    def test_deleted_release_fails_without_mutation(self):
        (self.root / self.old).unlink()
        with self.assertRaises(ValueError):
            module.reconcile(self.root, self.base, True)
        self.assertFalse(self.pending())

    def test_identical_collisions_are_staged_only_once(self):
        self.put('sql/updates/world/2026_09_30_world_00_a.sql', b'SELECT 2;')
        self.put('sql/updates/world/2026_09_30_world_00_b.sql', b'SELECT 2;')
        module.reconcile(self.root, self.base, True)
        self.assertEqual(len(self.pending()), 1)

    def test_promoted_collision_requires_review(self):
        incoming = 'sql/updates/world/2026_09_30_world_00_incoming.sql'
        self.put(incoming, b'SELECT 2;')
        self.put(incoming + '.pending-name', b'old.sql')
        with self.assertRaises(ValueError):
            module.reconcile(self.root, self.base, True)
        self.assertTrue((self.root / incoming).exists())


if __name__ == '__main__':
    unittest.main()
