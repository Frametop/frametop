import tempfile
import unittest
from pathlib import Path


class InstallTests(unittest.TestCase):
    def module(self):
        try:
            from frame_testbench import install
        except ImportError:
            self.fail('installer missing')
        return install

    def test_dropin_only_targets_steamvr_and_scoped_libraries(self):
        mod = self.module()
        text = mod.dropin(Path('/home/test/project/build'))
        self.assertIn('[Service]', text)
        self.assertIn('LD_PRELOAD=', text)
        self.assertIn('FRAME_TESTBENCH_PROXY=', text)
        self.assertNotIn('ExecStart=', text)
        self.assertNotIn('sshd', text)

    def test_stage_immutable_and_remove_only_owned_dropin(self):
        mod = self.module()
        with tempfile.TemporaryDirectory() as tmp:
            home = Path(tmp) / 'home'
            build = Path(tmp) / 'build'
            build.mkdir()
            for name in mod.LIBRARIES:
                (build / name).write_bytes(b'fixture-' + name.encode())
            record = mod.stage(build, home)
            target = Path(record['dropin'])
            self.assertTrue(target.exists())
            self.assertTrue(all((Path(record['library_dir']) / n).exists() for n in mod.LIBRARIES))
            unrelated = target.parent / 'other.conf'
            unrelated.write_text('leave alone')
            self.assertEqual(mod.stage(build, home), record)
            mod.remove(home)
            self.assertFalse(target.exists())
            self.assertEqual(unrelated.read_text(), 'leave alone')
            self.assertTrue(Path(record['library_dir']).exists())

    def test_refuse_unowned_override_and_missing_library(self):
        mod = self.module()
        with tempfile.TemporaryDirectory() as tmp:
            home = Path(tmp) / 'home'
            build = Path(tmp) / 'build'
            build.mkdir()
            with self.assertRaises(FileNotFoundError):
                mod.stage(build, home)
            for name in mod.LIBRARIES:
                (build / name).write_bytes(b'test')
            target = mod.target(home)
            target.parent.mkdir(parents=True)
            target.write_text('unrelated user file')
            with self.assertRaises(ValueError):
                mod.stage(build, home)
            with self.assertRaises(ValueError):
                mod.remove(home)
            self.assertEqual(target.read_text(), 'unrelated user file')

    def test_systemd_escape(self):
        mod = self.module()
        text = mod.dropin(Path('/home/test/a%"b'))
        self.assertIn('%%', text)
        self.assertIn('\\"', text)
        for name in ['/home/test/new\nline', '/home/test/a b', '/home/test/a:b']:
            with self.assertRaises(ValueError):
                mod.dropin(Path(name))


if __name__ == '__main__':
    unittest.main()
