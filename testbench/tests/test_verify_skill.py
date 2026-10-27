"""The verify-frametop skill's helper (.claude/skills/verify-frametop/scripts/verify-frametop),
with the testbench, the Frame's services and the clock faked: no headset needed."""
import contextlib
import importlib.machinery
import importlib.util
import io
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HELPER = Path(__file__).resolve().parents[2] / '.claude/skills/verify-frametop/scripts/verify-frametop'


def load():
    loader = importlib.machinery.SourceFileLoader('verify_frametop', str(HELPER))
    spec = importlib.util.spec_from_loader('verify_frametop', loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


def proxy(**changes):
    status = {'ok': True, 'fakes': {'running': False}, 'input': {
        'available': True, 'proximity_ready': True, 'physical_worn': False, 'worn_override': None,
        'pose_override': False, 'hmd_buttons': {'/input/system/click': {'physical': None, 'override': None}},
        'controllers': {'left': {'pose_override': False, 'inputs': {'/input/a/click': False}}}}}
    status['input'].update(changes)
    return status


@unittest.skipUnless(HELPER.exists(), 'the verify-frametop skill is not in this checkout')
class VerifySkillTests(unittest.TestCase):
    def setUp(self):
        self.v = load()
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        tmp = Path(self.tmp.name)
        self.replies = {('status',): proxy(), ('pointer', 'state'): {'ok': True, 'gaze_mode': False},
                        ('power', 'status'): {'ok': True, 'displays': 'on'},
                        ('pointer', 'overlays'): {'ok': True, 'overlays': {'list': []}}}
        self.sent = []

        def bench(*args, timeout=60):
            self.sent.append(args)
            return self.replies.get(args, {'ok': True})
        self.asked = []

        def ask(name, message, timeout=2.0):
            self.asked.append((name, message))
            return 'ok on' if message.startswith('gaze') else '{"last": [1, 2]}'
        for name, value in (('bench', bench), ('ask', ask), ('LOCK', tmp / 'lock.json'), ('EVIDENCE', tmp / 'verify'),
                            ('listening', lambda: set(self.v.SOCKETS)), ('running', lambda name: True),
                            ('RELEASES', tmp / 'releases')):
            patch = mock.patch.object(self.v, name, value)
            patch.start()
            self.addCleanup(patch.stop)
        units = mock.patch.object(self.v.subprocess, 'run', side_effect=self.systemctl)
        units.start()
        self.addCleanup(units.stop)

    def systemctl(self, argv, **_):
        out = 'active\n' if argv[:3] == ['systemctl', '--user', 'is-active'] else 'experimental\n'
        return mock.Mock(returncode=0, stdout=out)

    def run_main(self, *argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = self.v.main(list(argv))
        return code, out.getvalue()

    def test_doctor_drives_only_an_unworn_healthy_frame_with_nothing_left_over(self):
        report, code = self.v.doctor()
        self.assertEqual((code, report['ok']), (0, True))
        self.replies[('status',)] = proxy(physical_worn=True)
        self.assertEqual(self.v.doctor()[1], 2)
        self.replies[('status',)] = {'ok': False, 'error': 'no proxy'}
        report, code = self.v.doctor()
        self.assertEqual(code, 1)
        self.assertIn('unread', report['checks'][0]['detail'])
        for leftover in (dict(worn_override=True), dict(worn_override=False), dict(pose_override=True),
                         dict(hmd_buttons={'/input/system/click': {'override': False}}),
                         dict(controllers={'right': {'pose_override': False, 'inputs': {'/input/a/click': True}}})):
            self.replies[('status',)] = proxy(**leftover)
            with self.subTest(leftover=leftover):
                self.assertEqual(self.v.doctor()[1], 1)

    def test_launch_refuses_a_worn_headset_and_a_run_under_way(self):
        self.replies[('status',)] = proxy(physical_worn=True)
        with self.assertRaisesRegex(SystemExit, 'worn'), contextlib.redirect_stdout(io.StringIO()):
            self.v.launch('x')
        self.assertFalse(self.v.LOCK.exists())
        self.replies[('status',)] = proxy()
        self.run_main('launch', 'first/run')
        held = json.loads(self.v.LOCK.read_text())
        self.assertTrue(held['run'].endswith('-first-run'))
        self.assertTrue((Path(held['run']) / 'baseline.json').exists())
        with self.assertRaisesRegex(SystemExit, 'under way'):
            self.v.launch('second')
        old = self.v.LOCK.stat().st_mtime - self.v.LEASE - 1
        os.utime(self.v.LOCK, (old, old))   # nobody snapshotted for the whole lease
        self.run_main('launch', 'second')
        self.assertTrue(json.loads(self.v.LOCK.read_text())['run'].endswith('-second'))

    def test_snapshot_keeps_the_readbacks_and_renews_the_lease(self):
        self.run_main('launch', 'snap')
        old = self.v.LOCK.stat().st_mtime - 100
        os.utime(self.v.LOCK, (old, old))
        code, out = self.run_main('snapshot', '2 after/pinch')
        folder = Path(json.loads(out)['snapshot'])
        self.assertEqual(folder.name, '2-after-pinch')
        readbacks = json.loads((folder / 'readbacks.json').read_text())
        self.assertEqual(readbacks['gaze'], {'last': [1, 2]})
        self.assertGreater(self.v.LOCK.stat().st_mtime, old + 50)
        self.assertNotIn(('capture', '--output', str(folder / 'capture')), self.sent)
        self.run_main('snapshot', '3', '--capture')
        self.assertIn(('capture', '--output', str(folder.parent / '3' / 'capture')), self.sent)

    def test_cleanup_restores_what_the_run_changed_and_keeps_the_evidence(self):
        self.run_main('launch', 'clean')
        run = Path(json.loads(self.v.LOCK.read_text())['run'])
        self.replies[('power', 'status')] = {'ok': True, 'displays': 'off'}
        self.replies[('pointer', 'state')] = {'ok': True, 'gaze_mode': True}
        code, out = self.run_main('cleanup')
        result = json.loads(out)
        self.assertEqual(code, 0)
        self.assertEqual(set(result['steps']), {'release', 'compositor', 'power', 'gaze_mode'})
        self.assertIn(('power', 'on'), self.sent)
        self.assertIn(('ft_pointer_helper', 'gaze off'), self.asked)
        self.assertFalse(self.v.LOCK.exists())
        self.assertTrue((run / 'baseline.json').exists() and (run / 'cleanup.json').exists())

    def test_cleanup_leaves_displays_off_that_were_off_and_reports_leftovers(self):
        self.replies[('power', 'status')] = {'ok': True, 'displays': 'off'}
        self.run_main('launch', 'dark')
        self.replies[('status',)] = proxy(pose_override=True)   # release didn't take
        code, out = self.run_main('cleanup')
        self.assertEqual(code, 1)
        self.assertNotIn(('power', 'on'), self.sent)
        self.assertIn('pose_override', json.loads(out)['overrides'])


if __name__ == '__main__':
    unittest.main()
