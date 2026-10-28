#!/usr/bin/python3
"""Offline tests for scripts/fast-install: a made-up repo and release tree, no headset."""
import importlib.machinery
import importlib.util
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

SCRIPTS = Path(__file__).resolve().parents[1]
loader = importlib.machinery.SourceFileLoader('fast_install', str(SCRIPTS / 'fast-install'))
spec = importlib.util.spec_from_loader('fast_install', loader)
fi = importlib.util.module_from_spec(spec)
loader.exec_module(fi)


def sh(cwd, *args):
    return subprocess.run(args, cwd=cwd, check=True, capture_output=True, text=True).stdout.strip()


class Paths(unittest.TestCase):
    def test_ignored_follows_dockerignore(self):
        patterns = fi.parse_ignore((SCRIPTS.parent / '.dockerignore').read_text())
        for path in ('screens/build/ft-screens', 'testbench/README.md', '.claude/skills/x/SKILL.md',
                     'hands/models/student/a.bin', 'hands/models/.student-v1', 'gaze/x.pyc',
                     'a/b/__pycache__/c.py', 'scripts/fast-install', 'scripts/tests/test_fast_install.py'):
            self.assertTrue(fi.ignored(path, patterns), path)
        for path in ('screens/vr.cpp', 'scripts/in-box', 'hands/models.pin'):
            self.assertFalse(fi.ignored(path, patterns), path)

    def test_ignore_pattern_forms(self):
        patterns = fi.parse_ignore('# a comment\n**/build/\ntop/\nreport-*.txt\n')
        self.assertTrue(fi.ignored('a/b/build/x', patterns))
        self.assertTrue(fi.ignored('top/x/y', patterns))
        self.assertFalse(fi.ignored('a/top/y', patterns))      # no **/: only at the top
        self.assertTrue(fi.ignored('report-1.txt', patterns))
        self.assertFalse(fi.ignored('docs/report-1.txt', patterns))

    def test_component_is_the_deepest_folder(self):
        self.assertEqual(fi.component_of('gaze/tracker/grab.c'), 'gaze/tracker')
        self.assertEqual(fi.component_of('gaze/ft-gaze.cpp'), 'gaze')
        self.assertEqual(fi.component_of('hands/rec/panel.cpp'), 'hands/rec')
        self.assertIsNone(fi.component_of('frunner/ft-spotpanel.cpp'))

    def test_what_a_build_reads(self):
        base = {p: ('100644', 'a') for p in (
            'screens/vr.cpp', 'screens/README.md', 'screens/tests/t.cpp', 'frunner/ft-spotpanel.cpp',
            'frunner/reserve.h', 'frunner/ft_frunner.py', 'scripts/openvr.sh', 'gaze/tracker/grab.c',
            'hands/include/fh_hands.h', 'keyboard/layouts/us.json', 'voice/build.sh')}

        def rebuilt(path):
            after = dict(base, **{path: ('100644', 'z')})
            return {c for c in fi.COMPONENTS if fi.inputs_hash(c, base) != fi.inputs_hash(c, after)}

        self.assertEqual(rebuilt('screens/vr.cpp'), {'screens'})
        self.assertEqual(rebuilt('screens/README.md'), set())
        self.assertEqual(rebuilt('screens/tests/t.cpp'), set())
        self.assertEqual(rebuilt('frunner/ft-spotpanel.cpp'), {'screens'})
        self.assertEqual(rebuilt('frunner/reserve.h'), {'screens'})
        self.assertEqual(rebuilt('frunner/ft_frunner.py'), set())
        self.assertEqual(rebuilt('gaze/tracker/grab.c'), {'gaze/tracker'})
        self.assertEqual(rebuilt('hands/include/fh_hands.h'), {'hands', 'screens', 'pointer/helper'})
        self.assertEqual(rebuilt('keyboard/layouts/us.json'), {'keyboard'})
        self.assertEqual(rebuilt('voice/build.sh'), {'voice'})
        openvr = rebuilt('scripts/openvr.sh')
        self.assertTrue({'screens', 'power', 'keyboard'} <= openvr)
        self.assertFalse({'voice', 'hands', 'gaze/tracker'} & openvr)

    def test_install_steps(self):
        self.assertIsNone(fi.install_step('screens/x.py'))
        self.assertIsNone(fi.install_step('scripts/in-box'))
        for path in ('pointer/driver/build/driver_ft_pointer.so', 'install.sh', 'pack/release-box.sh',
                     'gaze/install.sh', 'screens/frametop-handview.service', 'tour/frametop-tour.desktop',
                     'setup/bluetooth/main.conf', 'gaze/tracker/atomic-update.conf'):
            self.assertIsNotNone(fi.install_step(path), path)

    def test_what_runs(self):
        for path in ('input/relay.py', 'screens/build/ft-screens', 'keyboard/layouts/us.json'):
            self.assertTrue(fi.runs(path), path)
        for path in ('screens/README.md', 'screens/vr.cpp', 'hands/include/fh.h', 'float/test/t.py',
                     'docs/design.md'):
            self.assertFalse(fi.runs(path), path)

    def test_units_and_the_folders_they_use(self):
        tree = Path('/r/0.1')
        (roots) = ('/r/0.1/', '/r/current/')
        with mock.patch.object(Path, 'is_dir', lambda self: str(self) == '/r/0.1/keyboard'):
            self.assertEqual(fi.dirs_used(tree, 'frametop-pointer.service',
                                          ['/r/0.1/scripts/in-box', '/r/0.1/pointer/helper/build/ft-pointer'],
                                          roots), {'pointer/helper'})
            self.assertEqual(fi.dirs_used(tree, 'frametop-gaze.service', ['python3', '/r/0.1/gaze/ft-gazed'],
                                          roots), {'gaze', 'models'})
            self.assertEqual(fi.dirs_used(tree, 'frametop-keyboard.service', ['--data-dir', '/r/0.1/keyboard'],
                                          roots), {'keyboard'})
            desktop = fi.dirs_used(tree, fi.DESKTOP, ['/r/current/session/../input/ft-textinput'], roots)
            self.assertIn('input/ft-textinput', desktop)   # that file only, not all of input/
            self.assertNotIn('input', desktop)


class Restarts(unittest.TestCase):
    def setUp(self):
        units = {'frametop-input-relay.service': {'input', 'commands'},
                 'frametop-voice.service': {'voice', 'input', 'commands'},
                 fi.DESKTOP: {'screens', 'session', 'input/ft-textinput'}}
        self.calls = []
        for p in (mock.patch.object(fi, 'unit_dirs', lambda tree: units),
                  mock.patch.object(fi, 'say', lambda *a: None),
                  mock.patch.object(fi.subprocess, 'run',
                                    lambda cmd, **kw: self.calls.append(cmd) or mock.Mock(returncode=0))):
            p.start()
            self.addCleanup(p.stop)

    def restarted(self, changed, worn=False, worn_ok=False, enabled=True):
        with mock.patch.object(fi, 'frame_status', lambda: (worn, 1)):
            hit, ok = fi.restart(Path('/r'), changed, enabled, worn_ok)
        return hit, [c[-1] for c in self.calls]

    def test_runtime_files_only(self):
        self.assertEqual(self.restarted(['screens/README.md', 'screens/vr.cpp', 'input/test/t.py']), ([], []))

    def test_desktop_goes_last(self):
        hit, calls = self.restarted(['screens/build/ft-screens', 'commands/ft_commands.py'])
        self.assertEqual(hit, [fi.DESKTOP, 'frametop-input-relay.service', 'frametop-voice.service'])
        self.assertEqual(calls, ['frametop-input-relay.service', 'frametop-voice.service', 'restart'])

    def test_worn_headset_keeps_the_desktop(self):
        _, calls = self.restarted(['screens/build/ft-screens'], worn=True)
        self.assertEqual(calls, [])
        _, calls = self.restarted(['screens/build/ft-screens'], worn=True, worn_ok=True)
        self.assertEqual(calls, ['restart'])

    def test_no_restart(self):
        hit, calls = self.restarted(['input/vrws.py'], enabled=False)
        self.assertEqual((hit, calls), (['frametop-input-relay.service', 'frametop-voice.service'], []))


class Release(unittest.TestCase):
    """A repo with a base commit and a release tree made from it."""

    FILES = {'.dockerignore': '**/build/\ntestbench/\n', 'input/relay.py': 'v1\n', 'input/old.py': 'gone soon\n',
             'docs/a.md': 'same\n', 'testbench/t.py': 'dev\n', 'uv.lock': 'lock1\n',
             'hands/track/main.cpp': 'track1\n', 'hands/camd/camd.c': 'camd1\n', 'power/powerd.c': 'p1\n'}
    BUILT = {'hands/build/ft-hands': b'hands1', 'hands/build/ft-camd': b'camd1', 'power/build/ft-powerd': b'p1'}

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        top = Path(self.tmp.name)
        self.repo = top / 'repo'
        self.repo.mkdir()
        sh(self.repo, 'git', 'init', '-q')
        sh(self.repo, 'git', 'config', 'user.email', 't@example.com')
        sh(self.repo, 'git', 'config', 'user.name', 't')
        self.commit(self.FILES)
        base = sh(self.repo, 'git', 'rev-parse', 'HEAD')

        releases = top / 'releases'
        self.tree = releases / '0.1-test'
        sh(top, 'git', 'clone', '-q', str(self.repo), str(self.tree))
        sh(self.tree, 'rm', '-rf', '.git', 'testbench')
        (self.tree / '.frametop-release').write_text(f'VERSION=0.1-test\nCOMMIT={base}\nBOX=frametop-x\n')
        (releases / 'current').symlink_to('0.1-test')
        for rel, data in self.BUILT.items():
            (self.tree / rel).parent.mkdir(parents=True, exist_ok=True)
            (self.tree / rel).write_bytes(data)
            (self.tree / rel).chmod(0o755)

        self.stage = top / 'stage'
        self.builds = []
        self.restarts = []
        for p in (mock.patch.object(fi, 'ROOT', self.repo), mock.patch.object(fi, 'RELEASES', releases),
                  mock.patch.object(fi, 'STAGE', self.stage),
                  mock.patch.object(fi, 'restart', lambda tree, changed, enabled, worn_ok:
                                    self.restarts.append(sorted(changed)) or ([], True)),
                  mock.patch.object(fi, 'say', lambda *a: None),
                  mock.patch.object(fi, 'frame_status', lambda: (False, 1)),
                  mock.patch.object(fi, 'stage_checkout', lambda rev, box: None),
                  mock.patch.object(fi, 'build', self.fake_build)):
            p.start()
            self.addCleanup(p.stop)
        self.outputs = {}

    def fake_build(self, comp, box, cpus):
        self.builds.append(comp)
        if comp in getattr(self, 'failing', ()):
            raise fi.Refused(f'{comp}/build.sh failed')
        for rel, data in self.outputs.items():
            if rel.startswith(comp + '/build/'):
                (self.stage / rel).parent.mkdir(parents=True, exist_ok=True)
                (self.stage / rel).write_bytes(data)
                (self.stage / rel).chmod(0o755)

    def commit(self, edits):
        for path, text in edits.items():
            f = self.repo / path
            if text is None:
                f.unlink()
            else:
                f.parent.mkdir(parents=True, exist_ok=True)
                f.write_text(text)
        sh(self.repo, 'git', 'add', '-A')
        sh(self.repo, 'git', 'commit', '-qm', 'change', '--allow-empty')

    def install(self, **kw):
        args = dict(dry=False, restart_units=False, worn_ok=False, partial=False)
        return fi.install('HEAD', **{**args, **kw})

    def text(self, rel):
        return (self.tree / rel).read_text()


class InstallAndUndo(Release):
    def test_install_stack_and_undo(self):
        self.commit({'input/relay.py': 'v2\n', 'input/old.py': None, 'input/new.py': 'new\n',
                     'testbench/t.py': 'dev2\n'})
        self.assertEqual(self.install(), 0)
        self.assertEqual(self.text('input/relay.py'), 'v2\n')
        self.assertEqual(self.text('input/new.py'), 'new\n')
        self.assertFalse((self.tree / 'input/old.py').exists())
        self.assertFalse((self.tree / 'testbench').exists())   # .dockerignore: never in a release
        state = fi.load_state(self.tree)
        self.assertEqual(state['commit'], sh(self.repo, 'git', 'rev-parse', 'HEAD'))
        self.assertEqual(state['files'], {'input/new.py': 'absent', 'input/old.py': 'saved',
                                          'input/relay.py': 'saved'})
        self.assertNotIn('installing', state)
        self.assertEqual(self.restarts, [['input/new.py', 'input/old.py', 'input/relay.py']])

        # a second commit on top: the first original stays the one kept
        self.commit({'input/relay.py': 'v3\n'})
        self.assertEqual(self.install(), 0)
        self.assertEqual(self.text('input/relay.py'), 'v3\n')
        self.assertEqual(self.text('.fast-install/orig/input/relay.py'), 'v1\n')

        self.assertEqual(fi.undo(restart_units=False, worn_ok=False), 0)
        self.assertEqual(self.text('input/relay.py'), 'v1\n')
        self.assertEqual(self.text('input/old.py'), 'gone soon\n')
        self.assertFalse((self.tree / 'input/new.py').exists())
        self.assertFalse((self.tree / '.fast-install').exists())

    def test_mode_change_alone(self):
        os.chmod(self.repo / 'input/relay.py', 0o755)
        self.commit({})
        self.assertEqual(self.install(), 0)
        self.assertTrue(os.access(self.tree / 'input/relay.py', os.X_OK))
        self.assertEqual(self.text('input/relay.py'), 'v1\n')
        os.chmod(self.repo / 'input/relay.py', 0o644)
        self.commit({})
        self.assertEqual(self.install(), 0)
        self.assertFalse(os.access(self.tree / 'input/relay.py', os.X_OK))

    def test_dry_run_changes_nothing(self):
        self.commit({'input/relay.py': 'v2\n', 'power/powerd.c': 'p2\n', 'gaze/install.sh': 'x\n'})
        self.assertEqual(self.install(dry=True), 3)   # the installer is a step left
        self.assertEqual(self.text('input/relay.py'), 'v1\n')
        self.assertEqual(self.builds, [])
        self.assertIsNone(fi.load_state(self.tree))

    def test_image_change_needs_the_full_build(self):
        self.commit({'uv.lock': 'lock2\n', 'input/relay.py': 'v2\n'})
        with self.assertRaises(fi.Refused):
            self.install()
        self.assertEqual(self.text('input/relay.py'), 'v1\n')
        # --partial installs the rest, and lists the image change as a step left
        self.assertEqual(self.install(partial=True), 3)
        self.assertEqual(self.text('input/relay.py'), 'v2\n')
        self.assertIn('uv.lock', fi.load_state(self.tree)['steps'])

    def test_steps_stay_until_done(self):
        self.commit({'input/frametop-relay.service': '[Service]\n'})
        self.assertEqual(self.install(), 3)
        self.commit({'input/relay.py': 'v2\n'})
        self.assertEqual(self.install(), 3)   # still left
        self.assertEqual(fi.done(['input/frametop-relay.service'], False), 0)
        self.assertEqual(self.install(), 0)
        with self.assertRaises(fi.Refused):
            fi.done(['input/nothing.service'], False)


class Builds(Release):
    def test_native_change_builds_that_component(self):
        self.commit({'power/powerd.c': 'p2\n', 'input/relay.py': 'v2\n'})
        self.outputs = {'power/build/ft-powerd': b'p2'}
        self.assertEqual(self.install(), 0)
        self.assertEqual(self.builds, ['power'])
        self.assertEqual((self.tree / 'power/build/ft-powerd').read_bytes(), b'p2')
        self.assertTrue(os.access(self.tree / 'power/build/ft-powerd', os.X_OK))
        self.assertIn('power/build/ft-powerd', self.restarts[0])

        # the same inputs again: no build; a revert to the release's: a build
        self.commit({'input/relay.py': 'v3\n'})
        self.install()
        self.assertEqual(self.builds, ['power'])
        self.commit({'power/powerd.c': 'p1\n'})
        self.outputs = {'power/build/ft-powerd': b'p1'}
        self.install()
        self.assertEqual(self.builds, ['power', 'power'])
        self.assertEqual((self.tree / 'power/build/ft-powerd').read_bytes(), b'p1')

        self.assertEqual(fi.undo(restart_units=False, worn_ok=False), 0)
        self.assertEqual((self.tree / 'power/build/ft-powerd').read_bytes(), b'p1')

    def test_failed_build_changes_nothing(self):
        self.commit({'power/powerd.c': 'p2\n', 'input/relay.py': 'v2\n'})
        self.failing = {'power'}
        with self.assertRaises(fi.Refused):
            self.install()
        self.assertEqual(self.text('input/relay.py'), 'v1\n')
        self.assertIsNone(fi.load_state(self.tree))

    def test_interrupted_install_can_be_undone(self):
        self.commit({'input/relay.py': 'v2\n', 'input/zz.py': 'new\n'})
        real = fi.Live.put

        def crash_on_second(live, rel, data, mode):
            if rel == 'input/zz.py':
                raise KeyboardInterrupt
            real(live, rel, data, mode)

        with mock.patch.object(fi.Live, 'put', crash_on_second), self.assertRaises(KeyboardInterrupt):
            self.install()
        state = fi.load_state(self.tree)
        self.assertEqual(state['installing'], sh(self.repo, 'git', 'rev-parse', 'HEAD'))
        self.assertEqual(self.text('input/relay.py'), 'v2\n')
        self.assertEqual(fi.undo(restart_units=False, worn_ok=False), 0)
        self.assertEqual(self.text('input/relay.py'), 'v1\n')
        self.assertFalse((self.tree / 'input/zz.py').exists())

    def test_build_noise_in_camd_is_ignored(self):
        self.commit({'hands/track/main.cpp': 'track2\n'})
        self.outputs = {'hands/build/ft-hands': b'hands2', 'hands/build/ft-camd': b'camd1-other-path'}
        self.assertEqual(self.install(), 0)
        self.assertEqual((self.tree / 'hands/build/ft-hands').read_bytes(), b'hands2')
        self.assertEqual((self.tree / 'hands/build/ft-camd').read_bytes(), b'camd1')   # caps kept

    def test_camd_change_holds_hands_back(self):
        self.commit({'hands/camd/camd.c': 'camd2\n', 'hands/track/main.cpp': 'track2\n'})
        self.outputs = {'hands/build/ft-hands': b'hands2', 'hands/build/ft-camd': b'camd2'}
        self.assertEqual(self.install(), 3)
        self.assertEqual((self.tree / 'hands/build/ft-hands').read_bytes(), b'hands1')
        self.assertEqual((self.tree / 'hands/build/ft-camd').read_bytes(), b'camd1')
        self.assertIn('hands/build/ft-camd', fi.load_state(self.tree)['steps'])
        self.assertNotIn('hands', fi.load_state(self.tree)['built'])


class Arguments(unittest.TestCase):
    def test_commands_and_flags(self):
        calls = []
        with mock.patch.object(fi, 'locked', lambda fn, *a: calls.append((fn.__name__, a)) or 0), \
                mock.patch.object(fi, 'status', lambda: calls.append(('status', ())) or 0), \
                mock.patch('sys.stderr'):
            self.assertEqual(fi.main([]), 0)
            self.assertEqual(fi.main(['abc123', '--dry-run']), 0)
            self.assertEqual(fi.main(['undo', '--worn-ok']), 0)
            self.assertEqual(fi.main(['status']), 0)
            self.assertEqual(fi.main(['done', '--all']), 0)
            self.assertEqual(fi.main(['done', 'a.service']), 0)
            for bad in (['undo', '--dry-run'], ['status', '--partial'], ['done'], ['done', 'x', '--all'],
                        ['a', 'b'], ['--force']):
                self.assertEqual(fi.main(bad), 2, bad)
        self.assertEqual(calls[0], ('install', ('HEAD', False, True, False, False)))
        self.assertEqual(calls[1], ('install', ('abc123', True, True, False, False)))
        self.assertEqual(calls[2], ('undo', (True, True)))
        self.assertEqual(calls[4], ('done', ([], True)))


if __name__ == '__main__':
    unittest.main()
