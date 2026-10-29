"""The verify-frametop skill's helper (.claude/skills/verify-frametop/scripts/verify-frametop),
with the testbench, the Frame's services and the clock faked: no headset needed."""
import contextlib
import importlib.machinery
import importlib.util
import io
import json
import os
import re
import socket
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

SKILL = Path(__file__).resolve().parents[2] / '.claude/skills/verify-frametop'
HELPER = SKILL / 'scripts/verify-frametop'


def load():
    loader = importlib.machinery.SourceFileLoader('verify_frametop', str(HELPER))
    spec = importlib.util.spec_from_loader('verify_frametop', loader)
    module = importlib.util.module_from_spec(spec)
    with mock.patch.object(sys, 'dont_write_bytecode', True):   # no __pycache__ in the skill
        loader.exec_module(module)
    return module


STILL = {'position': [0.0, -0.09, 0.12], 'quaternion': [0.41, -0.1, 0.03, 0.9], 'valid': True}


def proxy(**changes):
    status = {'ok': True, 'fakes': {'running': False}, 'input': {
        'available': True, 'proximity_ready': True, 'physical_worn': False, 'worn_override': None,
        'physical_pose': STILL,
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
        self.sent, self.asked, self.log = [], [], []   # log: both, in the order they happened
        self.statuses = []   # each is the reply to one `status` call, before replies[('status',)]

        def bench(*args, timeout=60):
            self.sent.append(args)
            self.log.append(('bench', *args))
            if args == ('status',) and self.statuses:
                return self.statuses.pop(0)
            return self.replies.get(args, {'ok': True})
        self.answers = {('ft_powerd', 'status'): 'ok on 5 0', ('ft_powerd', 'on'): 'ok',
                        ('ft_gazed', 'status'): '{"last": [1, 2]}'}

        def ask(name, message, timeout=2.0, listen=0.0):
            self.asked.append((name, message))
            self.log.append(('ask', name, message))
            if message.startswith('gaze'):
                return 'ok ' + message.split()[1]
            return self.answers[name, message]   # an ask the test didn't expect fails it
        self.units = dict.fromkeys((*self.v.UNITS, self.v.HANDS), 'active')
        self.heard = set(self.v.SOCKETS)
        self.real_ask, self.real_bench = self.v.ask, self.v.bench
        for name, value in (('bench', bench), ('ask', ask), ('LOCK', tmp / 'lock.json'), ('EVIDENCE', tmp / 'verify'),
                            ('listening', lambda: set(self.heard)), ('running', lambda name: True),
                            ('unit_states', lambda: dict(self.units)), ('git', lambda *args: 'abc1234'),
                            ('RELEASES', tmp / 'releases'), ('SETTLE_GAP', 0),
                            ('RUNTIME', tmp)):   # (the claim file: CI's root has no /run/user/0)
            patch = mock.patch.object(self.v, name, value)
            patch.start()
            self.addCleanup(patch.stop)

    def check(self, report, name):
        return next(c for c in report['checks'] if c['name'] == name)

    def run_main(self, *argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
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

    def test_doctor_needs_the_services_but_only_hints_at_hands_and_the_release(self):
        self.replies[('status',)] = proxy()
        self.units[self.v.HANDS] = 'inactive'   # opt-in, and no release linked at all
        report, code = self.v.doctor()
        self.assertEqual(code, 0)
        self.assertFalse(self.check(report, 'hand tracking')['ok'])
        self.assertFalse(self.check(report, 'Frametop release')['ok'])
        self.units['frametop-pointer'] = 'failed'
        report, code = self.v.doctor()
        self.assertEqual(code, 1)
        self.assertIn('frametop-pointer=failed', self.check(report, 'Frametop services active')['detail'])
        self.units['frametop-pointer'] = 'active'
        self.heard.discard('ft_powerd')
        report, code = self.v.doctor()
        self.assertEqual(code, 1)
        self.assertEqual(self.check(report, 'Frametop sockets listening')['detail'], '@ft_powerd')

    def test_launch_refuses_a_worn_headset_and_a_run_under_way(self):
        self.replies[('status',)] = proxy(physical_worn=True)
        with self.assertRaisesRegex(SystemExit, 'worn'), contextlib.redirect_stdout(io.StringIO()):
            self.v.launch('x')
        self.replies[('status',)] = proxy(physical_worn=None)   # no sample yet: can't tell
        with self.assertRaisesRegex(SystemExit, 'not ready'), contextlib.redirect_stdout(io.StringIO()):
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
        self.assertEqual(self.v.LOCK.stat().st_mode & 0o777, 0o600)

    def test_launch_loses_to_a_launch_that_claimed_the_run_after_its_check(self):
        doctor = self.v.doctor

        def racing_doctor():
            self.v.LOCK.write_text(json.dumps({'run': 'other'}))   # another agent, mid-launch
            return doctor()
        with mock.patch.object(self.v, 'doctor', racing_doctor), \
                self.assertRaisesRegex(SystemExit, 'other is under way'):
            self.v.launch('late')
        self.assertFalse(self.v.EVIDENCE.exists())

    def test_snapshot_keeps_the_readbacks_and_renews_the_lease(self):
        self.run_main('launch', 'snap')
        old = self.v.LOCK.stat().st_mtime - 100
        os.utime(self.v.LOCK, (old, old))
        _, out = self.run_main('snapshot', '2 after/pinch')
        folder = Path(json.loads(out)['snapshot'])
        self.assertEqual(folder.name, '2-after-pinch')
        readbacks = json.loads((folder / 'readbacks.json').read_text())
        self.assertEqual(readbacks['gaze'], {'last': [1, 2]})
        self.assertGreater(self.v.LOCK.stat().st_mtime, old + 50)
        self.assertNotIn(('capture', '--output', str(folder / 'capture')), self.sent)
        self.run_main('snapshot', '--capture', '3')
        self.assertIn(('capture', '--output', str(folder.parent / '3' / 'capture')), self.sent)
        with self.assertRaisesRegex(SystemExit, 'new label'):
            self.run_main('snapshot', '3')
        for argv in (['--capture'], [], ['a', 'b'], ['--force']):
            with self.subTest(argv=argv), self.assertRaisesRegex(SystemExit, 'snapshot LABEL'):
                self.run_main('snapshot', *argv)

    def test_snapshot_needs_a_run_and_unknown_commands_fail(self):
        with self.assertRaisesRegex(SystemExit, 'no run'):
            self.run_main('snapshot', '1-before')
        with self.assertRaisesRegex(SystemExit, 'unknown command'):
            self.run_main('drive')
        self.assertEqual(self.run_main('--help')[0], 0)

    def test_cleanup_restores_what_the_run_changed_and_keeps_the_evidence(self):
        self.run_main('launch', 'clean')
        run = Path(json.loads(self.v.LOCK.read_text())['run'])
        self.answers[('ft_powerd', 'status')] = 'ok off 5 0'
        self.replies[('pointer', 'state')] = {'ok': True, 'gaze_mode': True}
        self.sent.clear()
        code, out = self.run_main('cleanup', str(run))
        result = json.loads(out)
        self.assertEqual(code, 0)
        self.assertEqual(list(result['steps']), ['power', 'release', 'compositor', 'gaze_mode'])
        self.assertEqual(self.sent[:2], [('release',), ('compositor', 'auto')])
        # The displays come on before the release lets standby in.
        self.assertLess(self.log.index(('ask', 'ft_powerd', 'on')), self.log.index(('bench', 'release')))
        self.assertIn(('ft_pointer_helper', 'gaze off'), self.asked)
        self.assertFalse(self.v.LOCK.exists())
        self.assertTrue((run / 'baseline.json').exists() and (run / 'cleanup.json').exists())

    def test_cleanup_leaves_displays_off_that_were_off_and_reports_leftovers(self):
        self.replies[('power', 'status')] = {'ok': True, 'displays': 'off'}
        self.run_main('launch', 'dark')
        run = json.loads(self.v.LOCK.read_text())['run']
        self.answers[('ft_powerd', 'status')] = 'ok off 5 0'
        self.replies[('status',)] = proxy(pose_override=True)   # release didn't take
        code, out = self.run_main('cleanup', run)
        self.assertEqual(code, 1)
        self.assertNotIn(('ft_powerd', 'on'), self.asked)
        self.assertIn('pose_override', json.loads(out)['overrides'])

    def test_cleanup_with_no_run_releases_and_leaves_displays_and_gaze_mode_alone(self):
        self.answers[('ft_powerd', 'status')] = 'ok off 5 0'   # maybe the user's own choice
        self.replies[('pointer', 'state')] = {'ok': True, 'gaze_mode': True}
        code, out = self.run_main('cleanup')
        result = json.loads(out)
        self.assertEqual(code, 0)
        self.assertEqual(self.sent[:2], [('release',), ('compositor', 'auto')])
        self.assertEqual(set(result['steps']), {'release', 'compositor'})
        self.assertIn('no baseline', result['note'])

    def test_cleanup_leaves_another_agents_run_alone_unless_forced(self):
        self.run_main('launch', 'theirs')
        with self.assertRaisesRegex(SystemExit, 'not the one named'):
            self.run_main('cleanup')
        with self.assertRaisesRegex(SystemExit, 'not the one named'):
            self.run_main('cleanup', str(self.v.EVIDENCE / 'mine'))
        self.assertNotIn(('release',), self.sent)
        self.assertEqual(self.run_main('cleanup', '--force')[0], 0)
        self.assertFalse(self.v.LOCK.exists())

    def test_cleanup_restores_a_run_whose_lease_ran_out_from_its_baseline(self):
        self.run_main('launch', 'abandoned')
        run = Path(json.loads(self.v.LOCK.read_text())['run'])
        old = self.v.LOCK.stat().st_mtime - self.v.LEASE - 1
        os.utime(self.v.LOCK, (old, old))
        self.answers[('ft_powerd', 'status')] = 'ok off 5 0'
        code, out = self.run_main('cleanup')
        self.assertEqual(code, 0)
        self.assertIn('power', json.loads(out)['steps'])
        self.assertTrue((run / 'cleanup.json').exists())

    def test_cleanup_releases_even_with_a_garbled_lock_or_a_missing_testbench(self):
        self.v.LOCK.write_text('[1, 2]')   # or a launch between creating the lock and writing it
        with self.assertRaisesRegex(SystemExit, 'being launched'):
            self.run_main('cleanup')
        with self.assertRaisesRegex(SystemExit, 'no run'):
            self.run_main('snapshot', '1')
        self.assertEqual(self.run_main('cleanup', '--force')[0], 0)
        self.assertIn(('release',), self.sent)
        self.v.LOCK.write_text('{')
        old = self.v.LOCK.stat().st_mtime - self.v.LEASE - 1
        os.utime(self.v.LOCK, (old, old))
        self.assertEqual(self.run_main('cleanup')[0], 0)
        with mock.patch.object(self.v, 'bench', self.real_bench), \
                mock.patch.object(self.v, 'TESTBENCH', Path(self.tmp.name) / 'nowhere'):
            self.assertIn('nowhere', self.v.bench('release')['error'])

    def test_guard_stops_the_run_when_someone_puts_the_headset_on(self):
        for worn, code in ((True, 2), (None, 1)):
            self.run_main('launch', f'guarded-{code}')
            self.assertEqual(self.run_main('guard')[0], 0)
            self.replies[('status',)] = proxy(physical_worn=worn)
            self.sent.clear()
            with self.subTest(worn=worn), self.assertRaises(SystemExit) as stop:
                self.run_main('snapshot', '2-after')
            self.assertEqual(stop.exception.code, code)
            released = self.sent.index(('release',))
            self.assertEqual(self.sent[released:released + 2], [('release',), ('compositor', 'auto')])
            self.assertEqual(set(self.sent[:released]), {('status',)})   # nothing driven first
            self.assertFalse(self.v.LOCK.exists())
            self.replies[('status',)] = proxy()

    def test_a_sensor_blip_on_a_still_headset_is_not_a_wearer(self):
        self.run_main('launch', 'blips')
        worn, off = proxy(physical_worn=True), proxy()
        turned = dict(STILL, quaternion=[0.41, -0.2, 0.03, 0.88])   # about 11 degrees of yaw
        nudged = dict(STILL, position=[0.0, -0.09, 0.135])   # 1.5 cm
        cases = (
            ('blip', [worn, worn, off, off], 0),
            ('held', [worn] * self.v.WORN_READS, 2),
            ('one off read', [worn, off, worn, worn, worn], 2),
            ('flapping', [worn, off] * self.v.SETTLE_READS, 2),
            ('moved', [worn, proxy(physical_pose=turned), off], 2),
            ('nudged', [worn, proxy(physical_pose=nudged), off], 2),
            ('pose unknown', [proxy(physical_worn=True, physical_pose={'valid': False}), off, off], 2),
            ('pose null', [proxy(physical_worn=True, physical_pose=None), off, off], 2),
            ('pose part null', [worn, proxy(physical_pose=dict(STILL, quaternion=None)), off], 2),
            ('pose missing', [worn, proxy(physical_pose={'valid': True}), off], 2),
            ('unread', [worn, {'ok': False, 'error': 'gone'}], 1),
        )
        for name, statuses, code in cases:
            with self.subTest(name):
                if not self.v.LOCK.exists():
                    self.run_main('launch', name)
                self.statuses[:] = statuses
                if code:
                    with self.assertRaises(SystemExit) as stop:
                        self.run_main('guard')
                    self.assertEqual(stop.exception.code, code)
                    self.assertIn(('release',), self.sent)
                else:
                    self.assertEqual(self.run_main('guard')[0], 0)
                    self.assertTrue(self.v.LOCK.exists())
                self.statuses.clear()
                self.sent.clear()
        self.statuses[:] = [worn, off, off]
        report, code = self.v.doctor()
        self.assertEqual(code, 0)
        self.assertIn('blinked', report['checks'][0]['detail'])

    def test_guard_cleans_up_whatever_goes_wrong_reading_the_sensor(self):
        self.run_main('launch', 'broken')

        def broken(*args, timeout=60):
            self.sent.append(args)
            if args == ('status',):
                raise RuntimeError('a reply nobody expected')
            return {'ok': True}
        with mock.patch.object(self.v, 'bench', broken), self.assertRaises(SystemExit) as stop:
            self.run_main('guard')
        self.assertEqual(stop.exception.code, 1)
        self.assertIn(('release',), self.sent)
        self.assertFalse(self.v.LOCK.exists())

    def test_a_launch_that_fails_after_its_claim_frees_the_run(self):
        with mock.patch.object(self.v, 'readbacks', mock.Mock(side_effect=OSError('disk full'))), \
                self.assertRaises(OSError):
            self.run_main('launch', 'full')
        self.assertFalse(self.v.LOCK.exists())

    def test_bad_arguments_print_the_usage(self):
        for argv in (['ask', 'ft_gazed', 'status', '--listen', 'soon'], ['ask', 'ft_gazed'],
                     ['guard', 'extra'], ['cleanup', 'a', 'b'], ['cleanup', '--bogus'],
                     ['launch', '--force'], ['launch', 'a', 'b']):
            with self.subTest(argv=argv), self.assertRaises(SystemExit) as stop:
                self.run_main(*argv)
            self.assertIsInstance(stop.exception.code, str)
        self.assertNotIn(('release',), self.sent)

    def test_the_docs_name_only_commands_the_helper_has_and_keep_the_feature_contract(self):
        commands = set(re.findall(r"command == '([a-z]+)'", HELPER.read_text()))
        for doc in (SKILL / 'SKILL.md', *sorted((SKILL / 'features').glob('*.md'))):
            text = doc.read_text()
            with self.subTest(doc=doc.name):
                self.assertLessEqual(set(re.findall(r'`V ([a-z]+)', text)), commands)
                if doc.parent.name == 'features' and doc.name != 'README.md':
                    self.assertEqual(re.findall(r'^## (.+)$', text, re.M), [
                        'Sub-features', 'How to get to it (user POV)', 'Driving it with the testbench',
                        'Gotchas'])

    def test_ask_listens_for_every_reply(self):
        server = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        name = f'verify-frametop-test-{os.getpid()}'
        server.bind('\0' + name)
        self.addCleanup(server.close)

        def answer():
            message, addr = server.recvfrom(100)
            for reply in (b'watching', b'event 1', b'event 2'):
                server.sendto(reply, addr)
        threading.Thread(target=answer, daemon=True).start()
        with mock.patch.object(self.v, 'ask', self.real_ask):
            code, out = self.run_main('ask', name, 'watch 1', '--listen', '0.3')
        self.assertEqual((code, out.split()), (0, ['watching', 'event', '1', 'event', '2']))
        self.assertTrue(self.real_ask(name + '-nobody', 'status').startswith('error'))


if __name__ == '__main__':
    unittest.main()
