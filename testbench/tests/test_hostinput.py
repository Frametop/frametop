"""The host-input guard (frame_testbench.hostinput, and the CLI's worn): a faked wearer must not
take a streamed host's keyboard and mouse unless the user allows it. No headset needed."""
import contextlib
import fcntl
import io
import json
import os
import signal
import subprocess
import sys
import tempfile
import time
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


class GuardProcessTests(Fixture):
    """The real guard process, against a temp runtime dir and layout, and an input proxy socket
    that isn't there (it must never reach this machine's)."""
    def start(self):
        env = dict(os.environ, XDG_RUNTIME_DIR=str(self.run), FRAME_TESTBENCH_SOCKET=str(self.tmp / 'no.sock'))
        guard = subprocess.Popen([sys.executable, '-m', 'frame_testbench.hostinput', 'guard', str(self.layout)],
                                 cwd=Path(hostinput.__file__).resolve().parent.parent, env=env,
                                 stdout=subprocess.PIPE, text=True)
        self.addCleanup(self.stop, guard)
        return guard

    def stop(self, guard):
        if guard.poll() is None:
            guard.kill()
            guard.wait()
        guard.stdout.close()

    def wait_flag(self, guard):
        end = time.monotonic() + 10
        while time.monotonic() < end and guard.poll() is None:
            if (hostinput.read_flag() or {}).get('pid') == guard.pid:
                return
            time.sleep(0.02)
        self.fail('the guard wrote no flag')

    def lock_free(self, host='192.168.1.61'):
        held = {}
        busy = hostinput.take_locks([host], held)
        for fd in held.values():
            os.close(fd)
        return not busy

    def test_it_holds_the_locks_while_the_flag_names_it(self):
        guard = self.start()
        self.wait_flag(guard)
        self.assertFalse(hostinput.read_flag()['allow_host_input'])
        self.assertFalse(self.lock_free())   # no stream can lease oma7 now
        self.assertTrue(self.lock_free('192.168.1.92'))   # input off: not its business
        hostinput.clear_flag()   # worn off, physical or release
        self.assertEqual(guard.wait(5), 0)
        self.assertTrue(self.lock_free())

    def test_it_lets_go_when_an_allowed_wearer_replaces_it(self):
        guard = self.start()
        self.wait_flag(guard)
        hostinput.set_flag(True, 2.0)
        self.assertEqual(guard.wait(5), 0)
        self.assertTrue(hostinput.read_flag()['allow_host_input'])

    def test_a_busy_lock_means_no_guard_and_no_flag(self):
        held = {}
        hostinput.take_locks(['192.168.1.61'], held)   # a live stream holds it
        try:
            guard = self.start()
            self.assertEqual(guard.wait(10), 3)
            self.assertIn('busy: 192.168.1.61', guard.stdout.read())
            self.assertIsNone(hostinput.read_flag())
        finally:
            os.close(held['192.168.1.61'])

    def test_a_stopped_guard_or_a_new_busy_host_takes_the_wearer_and_flag_with_it(self):
        guard = self.start()
        self.wait_flag(guard)
        guard.send_signal(signal.SIGTERM)   # its unit stopped while the wearer is faked
        self.assertEqual(guard.wait(5), 0)
        self.assertIsNone(hostinput.read_flag())
        guard = self.start()
        self.wait_flag(guard)
        held = {}
        hostinput.take_locks(['192.168.1.92'], held)   # its stream holds the lock...
        try:
            layout = json.loads(self.layout.read_text())
            layout['hosts'][2]['input'] = True   # ...when it starts lending its input
            self.layout.write_text(json.dumps(layout))
            self.assertEqual(guard.wait(5), 0)
            self.assertIn('releasing the wearer', guard.stdout.read())
            self.assertIsNone(hostinput.read_flag())
        finally:
            os.close(held['192.168.1.92'])

    def test_guard_alive_knows_a_guard_by_its_command_line(self):
        self.process(31, sys.executable, '-m', 'frame_testbench.hostinput', 'guard')
        self.process(32, sys.executable, 'something-else')
        self.assertTrue(hostinput.guard_alive({'pid': 31}, self.proc))
        self.assertFalse(hostinput.guard_alive({'pid': 32}, self.proc))
        self.assertFalse(hostinput.guard_alive({'pid': 33}, self.proc))
        self.assertFalse(hostinput.guard_alive({'allow_host_input': False}, self.proc))

    def test_runtime_dir_is_never_a_nested_desktops(self):
        base = f'/run/user/{os.getuid()}'
        with mock.patch.dict(os.environ, {'XDG_RUNTIME_DIR': base + '/frametop'}):
            self.assertEqual(hostinput.runtime_dir(), Path(base))
        self.assertEqual(hostinput.runtime_dir(), self.run)


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
        self.guards = []   # each fake guard: what start_guard would have run, as a live pid

        def start_guard():
            self.guards.append(os.getpid())
            hostinput.set_flag(False, 1.0)   # the guard writes the flag naming itself
            return os.getpid()
        self.alive = True
        for patch in (mock.patch.object(hostinput, 'state', lambda detail=False: real(self.layout, self.proc, detail)),
                      mock.patch.object(hostinput, 'start_guard', start_guard),
                      mock.patch.object(hostinput, 'guard_alive', lambda flag, proc='/proc': self.alive)):
            patch.start()
            self.addCleanup(patch.stop)

    def worn(self, *args):
        return cli.dispatch(cli.parser().parse_args(['worn', *args]), self.bench)

    def test_worn_on_starts_the_guard_before_the_wearer_and_off_clears_the_flag(self):
        self.worn('on')
        self.assertEqual((self.guards, self.bench.commands), ([os.getpid()], [('worn 1', True)]))
        self.assertFalse(hostinput.read_flag()['allow_host_input'])
        self.worn('on')   # its guard still holds the locks: no second one
        self.assertEqual(len(self.guards), 1)
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
        self.assertEqual((self.bench.commands, self.guards), ([], []))
        self.assertIsNone(hostinput.read_flag())
        self.worn('on', '--allow-host-input')
        self.assertEqual((self.bench.commands, self.guards), ([('worn 1', True)], []))
        self.assertTrue(hostinput.read_flag()['allow_host_input'])

    def test_worn_on_is_refused_when_the_guard_cant_take_the_locks(self):
        with mock.patch.object(hostinput, 'start_guard', mock.Mock(side_effect=RuntimeError('lock busy'))):
            with self.assertRaisesRegex(ValueError, 'worn on refused: lock busy'):
                self.worn('on')
        self.assertEqual(self.bench.commands, [])

    def test_a_failed_worn_on_leaves_no_flag(self):
        def broken(text):
            raise OSError('no proxy')
        self.bench.control = broken
        with self.assertRaises(OSError):
            self.worn('on')
        self.assertIsNone(hostinput.read_flag())

    def test_any_command_drops_a_faked_wearer_whose_guard_has_gone(self):
        self.worn('on')
        args = cli.parser().parse_args(['pointer', 'state'])
        self.stream(22, '192.168.1.61')   # its lock is the guard's: it can't lease
        self.assertIsNone(cli.drop_unsafe_worn(self.bench, args))
        self.alive = False
        with contextlib.redirect_stderr(io.StringIO()) as err:
            reason = cli.drop_unsafe_worn(self.bench, args)
        self.assertIn('guard has gone', reason)
        self.assertIn('released the faked wearer', err.getvalue())
        self.assertEqual(self.bench.commands[-1], ('worn-release', True))
        self.assertIsNone(hostinput.read_flag())
        hostinput.set_flag(False, 1.0)   # left by a guard that died, with no input proxy now

        def gone(text):
            raise ConnectionRefusedError('no proxy')
        self.bench.control = gone
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertIn('guard', cli.drop_unsafe_worn(self.bench, args))
        self.assertIsNone(hostinput.read_flag())

    def test_main_reports_a_dropped_wearer(self):
        hostinput.set_flag(False, 1.0)
        self.alive = False
        with mock.patch.object(cli, 'Bench', lambda socket=None: self.bench), \
                contextlib.redirect_stdout(io.StringIO()) as out, \
                contextlib.redirect_stderr(io.StringIO()):
            cli.main(['host-input'])
        self.assertIn('guard has gone', json.loads(out.getvalue())['worn_released'])

    def test_an_allowed_wearer_stays(self):
        self.stream(22, '192.168.1.61')
        self.worn('on', '--allow-host-input')
        self.alive = False
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
