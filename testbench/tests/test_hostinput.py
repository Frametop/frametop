"""The host-input guard (frame_testbench.hostinput, and the CLI's worn): a faked wearer must not
take a streamed host's keyboard and mouse unless the user allows it. No headset needed."""
import contextlib
import fcntl
import io
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from frame_testbench import cli, hostinput

LAYOUT = {'hosts': [
    {'name': 'oma7', 'address': '192.168.1.61', 'input': True, 'displays': []},
    {'name': 'Deej-5090', 'address': '192.168.1.93', 'mic': True, 'displays': []},
    {'name': "Daenon's Pro14", 'address': '192.168.1.92', 'input': False, 'displays': []},
    {'name': 'broken'}, 'not a host']}


class Fixture(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__('shutil').rmtree(self.tmp))
        self.layout = self.tmp / 'layout.json'
        self.layout.write_text(json.dumps(LAYOUT))
        self.proc = self.tmp / 'proc'
        self.proc.mkdir()
        self.run = self.tmp / 'run'
        self.run.mkdir()
        env = mock.patch.dict(os.environ, {'XDG_RUNTIME_DIR': str(self.run)})
        env.start()
        self.addCleanup(env.stop)

    def process(self, pid, *argv):
        d = self.proc / str(pid)
        d.mkdir()
        (d / 'cmdline').write_bytes(b'\0'.join(a.encode() for a in argv) + b'\0')

    def stream(self, pid, host, cid='frametop-x'):
        self.process(pid, '/home/u/.local/share/frametop/releases/r/stream/build/ft-stream', 'stream', host,
                     '--id', cid, '--fd', '3', '--app', 'Desktop')


class HostInputTests(Fixture):
    def test_streams_finds_ft_stream_and_strips_a_port(self):
        self.stream(11, '192.168.1.61', 'frametop-oma7-edp-2')
        self.stream(12, '192.168.1.92:48089', 'frametop-mac-oled')
        self.process(13, '/usr/bin/python3', 'ft-stream', 'stream', '192.168.1.61')
        self.process(14, 'ft-stream', 'pair', '192.168.1.61', '--id', 'x')
        (self.proc / 'self').mkdir()
        (self.proc / '15').mkdir()   # gone before its cmdline was read
        found = sorted(hostinput.streams(self.proc), key=lambda s: s['pid'])
        self.assertEqual(found, [{'pid': 11, 'host': '192.168.1.61', 'id': 'frametop-oma7-edp-2'},
                                 {'pid': 12, 'host': '192.168.1.92', 'id': 'frametop-mac-oled'}])

    def test_only_hosts_with_input_true_count(self):
        self.assertEqual(hostinput.input_hosts(self.layout), {'192.168.1.61': 'oma7'})
        self.assertEqual(hostinput.input_hosts(self.tmp / 'missing.json'), {})
        self.layout.write_text('{not json')
        self.assertEqual(hostinput.input_hosts(self.layout), {})

    def test_refusal_names_the_host_only_while_its_stream_is_live(self):
        self.stream(11, '192.168.1.92')   # input off
        self.stream(12, '192.168.1.93')   # no input key
        st = hostinput.state(self.layout, self.proc)
        self.assertEqual(st['input_hosts'], ['oma7'])
        self.assertIsNone(hostinput.refusal(st))
        self.stream(13, '192.168.1.61')
        st = hostinput.state(self.layout, self.proc)
        self.assertEqual([s['name'] for s in st['input_streams']], ['oma7'])
        self.assertIn('oma7', hostinput.refusal(st))

    def test_locks_say_which_host_lock_is_held_without_taking_it(self):
        held, free = self.run / 'frametop-input-192.168.1.61.lock', self.run / 'frametop-input-192.168.1.93.lock'
        held.touch(), free.touch()
        with open(held) as f:
            fcntl.flock(f, fcntl.LOCK_EX)   # as ft-stream holds it: read back from the real /proc/locks
            found = {lk['host']: lk for lk in hostinput.locks()}
            self.assertEqual(found['192.168.1.61'], {'host': '192.168.1.61', 'held': True, 'pid': os.getpid()})
            self.assertEqual(found['192.168.1.93'], {'host': '192.168.1.93', 'held': False, 'pid': None})
        self.assertFalse(hostinput.locks()[0]['held'])
        (self.proc / 'locks').write_text('1: FLOCK  ADVISORY  WRITE 7 00:00:1 0 EOF\nnot a lock line\n')
        self.assertEqual([lk['held'] for lk in hostinput.locks(self.proc)], [False, False])
        self.assertEqual(hostinput.locks(self.tmp / 'no-proc')[0]['held'], False)

    def test_log_lines_keep_only_the_last_hostinput_lines(self):
        (self.run / 'frametop-remote-105.log').write_text(
            'stream: connected\n' + ''.join(f'hostinput: line {i}\n' for i in range(30)) + 'shown: in 30 fps\n')
        (self.run / 'frametop-remote-106.log').write_text('stream: connected\n')
        logs = hostinput.log_lines()
        self.assertEqual([lg['log'] for lg in logs], ['frametop-remote-105.log'])
        self.assertEqual(logs[0]['lines'], [f'hostinput: line {i}' for i in range(10, 30)])
        self.assertNotIn('locks', hostinput.state(self.layout, self.proc))
        detail = hostinput.state(self.layout, self.proc, detail=True)
        self.assertEqual(detail['logs'], logs)
        self.assertEqual(detail['locks'], [])

    def test_flag_round_trip_and_garbled_flag_is_not_an_allow(self):
        self.assertIsNone(hostinput.read_flag())
        hostinput.set_flag(True, 5.0)
        self.assertEqual(hostinput.read_flag()['allow_host_input'], True)
        self.assertEqual(oct((self.run / 'frametop-fake-worn.json').stat().st_mode & 0o777), '0o600')
        hostinput.clear_flag()
        hostinput.clear_flag()   # twice is fine
        self.assertIsNone(hostinput.read_flag())
        (self.run / 'frametop-fake-worn.json').write_text('garbage')
        self.assertEqual(hostinput.read_flag(), {'allow_host_input': False})


class Bench(cli.Bench):
    def __init__(self, run):
        self.commands, self.run = [], run

    def control(self, text):
        flag = (self.run / 'frametop-fake-worn.json').exists()
        self.commands.append((text, flag))
        return {'ok': True}

    def fakes(self, text, start=False):
        return None


class WornGuardTests(Fixture):
    def setUp(self):
        super().setUp()
        self.bench = Bench(self.run)
        real = hostinput.state
        patch = mock.patch.object(hostinput, 'state', lambda detail=False: real(self.layout, self.proc, detail))
        patch.start()
        self.addCleanup(patch.stop)

    def worn(self, *args):
        return cli.dispatch(cli.parser().parse_args(['worn', *args]), self.bench)

    def test_worn_on_sets_the_flag_before_the_wearer_and_off_clears_it(self):
        self.worn('on')
        self.assertEqual(self.bench.commands, [('worn 1', True)])
        self.assertFalse(hostinput.read_flag()['allow_host_input'])
        self.worn('off')
        self.assertIsNone(hostinput.read_flag())
        self.worn('on')
        self.worn('physical')
        self.assertIsNone(hostinput.read_flag())
        self.worn('on')
        cli.dispatch(cli.parser().parse_args(['release']), self.bench)
        self.assertIsNone(hostinput.read_flag())

    def test_worn_on_is_refused_while_a_host_that_lends_input_streams(self):
        self.stream(21, '192.168.1.61')
        with self.assertRaisesRegex(ValueError, 'oma7.*--allow-host-input'):
            self.worn('on')
        self.assertEqual(self.bench.commands, [])
        self.assertIsNone(hostinput.read_flag())
        self.worn('on', '--allow-host-input')
        self.assertEqual(self.bench.commands, [('worn 1', True)])
        self.assertTrue(hostinput.read_flag()['allow_host_input'])

    def test_a_failed_worn_on_leaves_no_flag(self):
        def broken(text):
            raise OSError('no proxy')
        self.bench.control = broken
        with self.assertRaises(OSError):
            self.worn('on')
        self.assertIsNone(hostinput.read_flag())

    def test_any_command_drops_a_faked_wearer_once_such_a_stream_starts(self):
        self.worn('on')
        args = cli.parser().parse_args(['pointer', 'state'])
        self.assertIsNone(cli.drop_unsafe_worn(self.bench, args))   # no stream yet
        self.stream(22, '192.168.1.61')
        with contextlib.redirect_stderr(io.StringIO()) as err:
            reason = cli.drop_unsafe_worn(self.bench, args)
        self.assertIn('oma7', reason)
        self.assertIn('released the faked wearer', err.getvalue())
        self.assertEqual(self.bench.commands[-1], ('worn-release', True))
        self.assertIsNone(hostinput.read_flag())

    def test_an_allowed_wearer_stays(self):
        self.stream(22, '192.168.1.61')
        self.worn('on', '--allow-host-input')
        self.assertIsNone(cli.drop_unsafe_worn(self.bench, cli.parser().parse_args(['status'])))
        self.assertEqual(self.bench.commands, [('worn 1', True)])

    def test_host_input_command_reports_streams_and_flag(self):
        self.stream(23, '192.168.1.61', 'frametop-oma7-dp-2')
        reply = cli.dispatch(cli.parser().parse_args(['host-input']), self.bench)
        self.assertTrue(reply['ok'])
        self.assertEqual(reply['input_streams'][0]['id'], 'frametop-oma7-dp-2')
        self.assertIsNone(reply['fake_worn'])
        self.assertEqual((reply['locks'], reply['logs']), ([], []))


if __name__ == '__main__':
    unittest.main()
