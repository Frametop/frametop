#!/usr/bin/env python3
"""Offline tests for scripts/fast-install: a temporary repo and release tree, no headset."""
import importlib.machinery
import importlib.util
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
loader = importlib.machinery.SourceFileLoader('fast_install', str(HERE / 'fast-install'))
spec = importlib.util.spec_from_loader('fast_install', loader)
fi = importlib.util.module_from_spec(spec)
loader.exec_module(fi)

DOCKERIGNORE = (HERE.parent / '.dockerignore').read_text()


def sh(cwd, *args):
    return subprocess.run(args, cwd=cwd, check=True, capture_output=True, text=True).stdout.strip()


class Paths(unittest.TestCase):
    def test_ignored_follows_dockerignore(self):
        patterns = [p.strip().rstrip('/') for p in DOCKERIGNORE.splitlines()
                    if p.strip() and not p.startswith('#')]
        for path in ('screens/build/ft-screens', 'testbench/README.md', '.claude/skills/x/SKILL.md',
                     'hands/models/student/a.bin', 'hands/models/.student-v1', 'gaze/x.pyc',
                     'a/b/__pycache__/c.py', 'scripts/fast-install', 'scripts/fast-install-test.py',
                     'frametop-report-1.txt'):
            self.assertTrue(fi.ignored(path, patterns), path)
        for path in ('screens/vr.cpp', 'scripts/in-box', 'docs/frametop-report-1.txt', 'hands/models.pin'):
            self.assertFalse(fi.ignored(path, patterns), path)

    def test_component_is_the_deepest_folder(self):
        self.assertEqual(fi.component_of('gaze/tracker/grab.c'), 'gaze/tracker')
        self.assertEqual(fi.component_of('gaze/ft-gaze.cpp'), 'gaze')
        self.assertEqual(fi.component_of('hands/rec/panel.cpp'), 'hands/rec')
        self.assertIsNone(fi.component_of('frunner/ft-spotpanel.cpp'))

    def test_inputs_hash_counts_native_inputs_only(self):
        base = {'screens/vr.cpp': ('100644', 'a'), 'screens/x.py': ('100644', 'b'),
                'frunner/ft-spotpanel.cpp': ('100644', 'c'), 'scripts/openvr.sh': ('100644', 'd'),
                'gaze/tracker/grab.c': ('100644', 'e'), 'voice/build.sh': ('100755', 'f')}

        def changed(path):
            after = dict(base, **{path: ('100644', 'z')})
            return {c for c in fi.COMPONENTS if fi.inputs_hash(c, base) != fi.inputs_hash(c, after)}

        self.assertEqual(changed('screens/x.py'), set())
        self.assertEqual(changed('screens/vr.cpp'), {'screens'})
        self.assertEqual(changed('frunner/ft-spotpanel.cpp'), {'screens'})
        self.assertEqual(changed('gaze/tracker/grab.c'), {'gaze/tracker'})
        self.assertEqual(changed('voice/build.sh'), {'voice'})
        openvr = changed('scripts/openvr.sh')
        self.assertIn('screens', openvr)
        self.assertNotIn('voice', openvr)
        self.assertNotIn('hands', openvr)

    def test_install_steps(self):
        self.assertIsNone(fi.install_step('screens/x.py'))
        self.assertIsNone(fi.install_step('scripts/in-box'))
        for path in ('pointer/driver/build/driver_ft_pointer.so', 'install.sh', 'pack/release-box.sh',
                     'gaze/install.sh', 'screens/frametop-handview.service', 'tour/frametop-tour.desktop',
                     'setup/bluetooth/main.conf'):
            self.assertIsNotNone(fi.install_step(path), path)


class InstallAndUndo(unittest.TestCase):
    """A repo with two commits and a release tree made from the first."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        top = Path(self.tmp.name)
        self.repo = top / 'repo'
        self.repo.mkdir()
        sh(self.repo, 'git', 'init', '-q')
        sh(self.repo, 'git', 'config', 'user.email', 't@example.com')
        sh(self.repo, 'git', 'config', 'user.name', 't')
        files = {'.dockerignore': '**/build/\ntestbench/\n', 'input/relay.py': 'v1\n',
                 'input/old.py': 'gone soon\n', 'docs/a.md': 'same\n', 'testbench/t.py': 'dev\n',
                 'uv.lock': 'lock1\n'}
        for path, text in files.items():
            (self.repo / path).parent.mkdir(parents=True, exist_ok=True)
            (self.repo / path).write_text(text)
        sh(self.repo, 'git', 'add', '-A')
        sh(self.repo, 'git', 'commit', '-qm', 'base')
        self.base = sh(self.repo, 'git', 'rev-parse', 'HEAD')

        releases = top / 'releases'
        self.tree = releases / '0.1-test'
        sh(top, 'git', 'clone', '-q', str(self.repo), str(self.tree))
        sh(self.tree, 'rm', '-rf', '.git', 'testbench')
        (self.tree / '.frametop-release').write_text(f'VERSION=0.1-test\nCOMMIT={self.base}\nBOX=frametop-x\n')
        (releases / 'current').symlink_to('0.1-test')

        patches = [mock.patch.object(fi, 'ROOT', self.repo), mock.patch.object(fi, 'RELEASES', releases),
                   mock.patch.object(fi, 'restart', lambda tree, changed, enabled: ([], [])),
                   mock.patch.object(fi, 'say', lambda *a: None)]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)

    def tearDown(self):
        self.tmp.cleanup()

    def commit(self, edits):
        for path, text in edits.items():
            f = self.repo / path
            if text is None:
                f.unlink()
            else:
                f.parent.mkdir(parents=True, exist_ok=True)
                f.write_text(text)
        sh(self.repo, 'git', 'add', '-A')
        sh(self.repo, 'git', 'commit', '-qm', 'change')
        return sh(self.repo, 'git', 'rev-parse', 'HEAD')

    def test_install_then_undo(self):
        self.commit({'input/relay.py': 'v2\n', 'input/old.py': None, 'input/new.py': 'new\n',
                     'testbench/t.py': 'dev2\n'})
        os.chmod(self.repo / 'input/relay.py', 0o755)
        self.commit({})   # a mode change alone
        self.assertEqual(fi.install('HEAD', dry=False, restart_units=False, partial=False), 0)
        self.assertEqual((self.tree / 'input/relay.py').read_text(), 'v2\n')
        self.assertTrue(os.access(self.tree / 'input/relay.py', os.X_OK))
        self.assertEqual((self.tree / 'input/new.py').read_text(), 'new\n')
        self.assertFalse((self.tree / 'input/old.py').exists())
        self.assertFalse((self.tree / 'testbench').exists())   # .dockerignore: never in a release
        state = fi.load_state(self.tree)
        self.assertEqual(state['commit'], sh(self.repo, 'git', 'rev-parse', 'HEAD'))
        self.assertEqual(state['files'], {'input/new.py': 'absent', 'input/old.py': 'saved',
                                          'input/relay.py': 'saved'})

        # a second commit on top: the first original stays the one kept
        self.commit({'input/relay.py': 'v3\n'})
        self.assertEqual(fi.install('HEAD', dry=False, restart_units=False, partial=False), 0)
        self.assertEqual((self.tree / 'input/relay.py').read_text(), 'v3\n')
        self.assertEqual((self.tree / '.fast-install/orig/input/relay.py').read_text(), 'v1\n')

        self.assertEqual(fi.undo(restart_units=False), 0)
        self.assertEqual((self.tree / 'input/relay.py').read_text(), 'v1\n')
        self.assertEqual((self.tree / 'input/old.py').read_text(), 'gone soon\n')
        self.assertFalse((self.tree / 'input/new.py').exists())
        self.assertFalse((self.tree / '.fast-install').exists())

    def test_dry_run_changes_nothing(self):
        self.commit({'input/relay.py': 'v2\n'})
        self.assertEqual(fi.install('HEAD', dry=True, restart_units=False, partial=False), 0)
        self.assertEqual((self.tree / 'input/relay.py').read_text(), 'v1\n')
        self.assertIsNone(fi.load_state(self.tree))

    def test_image_change_needs_the_full_build(self):
        self.commit({'uv.lock': 'lock2\n', 'input/relay.py': 'v2\n'})
        with self.assertRaises(fi.Refused):
            fi.install('HEAD', dry=False, restart_units=False, partial=False)
        self.assertEqual((self.tree / 'input/relay.py').read_text(), 'v1\n')
        # --partial installs the rest, and lists the image change as a step left
        self.assertEqual(fi.install('HEAD', dry=False, restart_units=False, partial=True), 3)
        self.assertEqual((self.tree / 'input/relay.py').read_text(), 'v2\n')
        self.assertIn('uv.lock', fi.load_state(self.tree)['steps'])

    def test_installer_files_are_steps_left(self):
        self.commit({'input/frametop-relay.service': '[Service]\n'})
        self.assertEqual(fi.install('HEAD', dry=False, restart_units=False, partial=False), 3)
        self.assertIn('input/frametop-relay.service', fi.load_state(self.tree)['steps'])


if __name__ == '__main__':
    unittest.main()
