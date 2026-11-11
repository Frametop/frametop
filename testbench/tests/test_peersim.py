"""ft-peer-sim (frame_testbench/peersim.py): what it is asked, with stand-ins for its programs,
and one fake peer for real in the room of an `ft-together --loopback`: it comes in, is seen
standing, moves when told to walk, is heard humming with its mouth moving, and leaves."""
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

from frame_testbench import peersim

TESTBENCH = Path(__file__).resolve().parents[1]
POSE_HEAD = '<BH3f'   # together/proto/posewire.h: kind, count, the eyes' place
MOUTH = slice(26, 31)  # ...and the five mouth shapes


class Child:
    """A program that was started, for a Sim that starts none."""

    def __init__(self, argv, env=None):
        self.argv, self.env, self.ended = argv, env, False

    def poll(self):
        return 0 if self.ended else None

    def terminate(self):
        self.ended = True

    def wait(self, timeout=None):
        return 0


class RulesTest(unittest.TestCase):
    def setUp(self):
        self.started, self.asked, self.now = [], [], 100.0
        programs = {name: Path('/programs') / name for name in peersim.PROGRAMS}

        def spawn(argv, env=None):
            self.started.append(Child(argv, env))
            return self.started[-1]

        self.sim = peersim.Sim(2, 'Bo', 47195, programs, loopback=True, models='/models', tag='_t', spawn=spawn, clock=lambda: self.now)
        self.sim.ask = lambda text: self.asked.append(text) or ('ok {"mode": "idle"}' if text == 'state' else 'ok')
        self.sim.start()
        self.addCleanup(self.sim.asker.close)

    def test_it_starts_a_session_of_its_own_and_someone_standing(self):
        together, body = self.started
        self.assertEqual(together.argv, ['/programs/ft-together', '--port', '47195', '--socket', 'ft_peer_2_t', '--name', 'Bo', '--loopback'])
        self.assertEqual(together.env['FT_AVATARS_DIR'], '/models')
        self.assertEqual(body.argv, ['/programs/ft-avatars', '--pretend', 'stand', '--together', 'ft_peer_2_t', '--walked', '0,0,0'])

    def test_a_pose_or_a_walk_puts_another_wearer_in_the_first_ones_place(self):
        self.assertEqual(self.sim.answer('pose wave'), 'ok')
        self.assertEqual(self.sim.answer('walk 1.5 -0.5 90'), 'ok')
        first, second, third = self.started[1:]
        self.assertTrue(first.ended and second.ended and not third.ended)
        self.assertEqual(third.argv[2:], ['wave', '--together', 'ft_peer_2_t', '--walked', '1.5,-0.5,90'])
        self.assertEqual(json.loads(self.sim.answer('state')[3:])['walked'], [1.5, -0.5, 90.0])

    def test_speaking_is_a_program_and_quiet_is_none(self):
        self.assertEqual(self.sim.answer('speak 440'), 'ok')
        voice = self.started[-1]
        self.assertEqual(voice.argv, ['/programs/ft-roomvoice', '--socket', 'ft_peer_2_t_voice', '--together', 'ft_peer_2_t',
                                      '--screens', 'ft_peer_2_t_screens', '--hum', '440', '--probe'])
        self.assertEqual(self.sim.answer('speak off'), 'ok')
        self.assertTrue(voice.ended)
        self.assertEqual(len(self.started), 3)
        self.assertFalse(json.loads(self.sim.answer('state')[3:])['running']['ft-roomvoice'])

    def test_the_session_is_asked_what_is_its_to_answer(self):
        for text in ('join 100.64.0.1 47193 ABCD2345', 'leave', 'away game', 'away back', 'wear Robot one', 'wear'):
            self.assertEqual(self.sim.answer(text), 'ok', text)
        self.assertEqual(self.asked, ['join 100.64.0.1 47193 ABCD2345', 'leave', 'away game', 'away back', 'wear Robot one', 'wear'])

    def test_nobody_stands_there_while_it_is_away(self):
        body = self.started[1]
        self.assertEqual(self.sim.answer('away game'), 'ok')
        self.assertTrue(body.ended)
        self.assertEqual(self.sim.answer('away off'), 'ok')
        self.assertEqual(self.sim.answer('pose wave'), 'ok')   # kept for when it is back
        state = json.loads(self.sim.answer('state')[3:])
        self.assertEqual((len(self.started), state['away'], state['running']['ft-avatars']), (2, True, False))
        self.assertEqual(self.sim.answer('away back'), 'ok')
        self.assertEqual(self.started[2].argv[2], 'wave')
        self.sim.ask = lambda text: 'error not now'
        self.assertEqual(self.sim.answer('away game'), 'error not now')
        self.assertFalse(self.started[2].ended)

    def test_an_avatar_is_told_again_every_two_seconds_and_none_is_not(self):
        self.sim.answer('wear Robot')
        self.sim.tick()
        self.now += 2.5
        self.sim.tick()
        self.sim.tick()
        self.assertEqual(self.asked, ['wear Robot', 'wear Robot'])
        self.sim.answer('wear')
        self.now += 5
        self.sim.tick()
        self.assertEqual(self.asked[2:], ['wear'])

    def test_what_isnt_a_question_starts_nothing_and_asks_nothing(self):
        for text in ('', 'pose fly', 'pose', 'walk 1 2', 'walk 99 0 0', 'walk nan 0 0', 'walk a b c', 'speak', 'speak 10', 'speak nan',
                     'speak 440 880', 'away', 'away gone', 'join host 47193', 'join ho;st 47193 CODE', 'join host 0 CODE',
                     'join host 70000 CODE', 'join host port CODE', 'join host 47193 co/de', 'wear ../x', 'wear ' + 'a' * 65,
                     'state now', 'leave now', 'run ls', 'join -h 1 CODE'):
            self.assertTrue(self.sim.answer(text).startswith('error state | join'), text)
        self.assertEqual((len(self.started), self.asked), (2, []))

    def test_ending_leaves_the_room_and_ends_every_program(self):
        self.sim.answer('speak 300')
        self.sim.end()
        self.assertEqual(self.asked, ['leave'])
        self.assertTrue(all(child.ended for child in self.started))

    def test_a_command_line(self):
        self.assertEqual(peersim.arguments(['2'])[1]['--port'], 47195)
        n, options = peersim.arguments(['3', '--name', 'Cy', '--loopback', '--port', '5000', '--tag', '_x'])
        self.assertEqual((n, options['--name'], options['--loopback'], options['--port'], options['--tag']), (3, 'Cy', True, 5000, '_x'))
        for bad in ([], ['0'], ['4'], ['one'], ['1', '--port', '0'], ['1', '--port'], ['1', '--tag', 'a/b'], ['1', '--what', 'x']):
            self.assertIsNone(peersim.arguments(bad), bad)


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


class Asker:
    """A socket of its own for questions to one program, which also gets what the program sends it."""

    def __init__(self, name):
        self.to = '\0' + name
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM | socket.SOCK_CLOEXEC)
        self.sock.bind('')

    def ask(self, text, seconds=3.0):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.sock.settimeout(max(0.05, deadline - time.monotonic()))
            try:
                self.sock.sendto(text.encode(), self.to)
                while True:
                    data = self.sock.recv(1 << 20)
                    if data[:1] not in (b'\x81', b'\x82'):
                        return data.decode()
            except (ConnectionRefusedError, FileNotFoundError):
                time.sleep(0.05)   # not listening yet
            except OSError:
                break
        return 'none'

    def state(self):
        reply = self.ask('state')
        return json.loads(reply[3:]) if reply.startswith('ok {') else {}

    def until(self, what, seconds=10.0):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if what(self.state()):
                return True
            time.sleep(0.05)
        return False

    def got(self, kind, question, seconds):
        """What the program passed on in that time: [(person, datagram)]."""
        self.ask(question)
        got, deadline = [], time.monotonic() + seconds
        while (left := deadline - time.monotonic()) > 0:
            self.sock.settimeout(left)
            try:
                data = self.sock.recv(4096)
            except OSError:
                break
            if data[:1] == kind:
                got.append((data[1], data[2:]))
        return got


@unittest.skipUnless(all(path.is_file() for path in peersim.PROGRAMS.values()), 'Frametop Together is not built (together/build.sh, screens/build.sh)')
class FakePeerTest(unittest.TestCase):
    def setUp(self):
        self.tag = '_%d' % os.getpid()
        self.port = free_port()
        self.log = tempfile.TemporaryFile()
        self.addCleanup(self.log.close)
        self.owner_socket = 'ft_peer_test_owner' + self.tag
        self.owner_proc = self.run_program([str(peersim.PROGRAMS['ft-together']), '--loopback', '--name', 'Owner',
                                            '--port', str(self.port), '--socket', self.owner_socket])
        self.sim_proc = self.run_program([sys.executable, str(TESTBENCH / 'ft-peer-sim'), '1', '--loopback', '--name', 'Sim',
                                          '--port', str(free_port()), '--tag', self.tag, '--models', tempfile.gettempdir()])
        self.owner, self.sim = Asker(self.owner_socket), Asker(peersim.sim_socket(1, self.tag))
        self.addCleanup(self.owner.sock.close)
        self.addCleanup(self.sim.sock.close)

    def run_program(self, argv):
        proc = subprocess.Popen(argv, stdout=self.log, stderr=subprocess.STDOUT, cwd=TESTBENCH)
        self.addCleanup(self.end, proc)
        return proc

    @staticmethod
    def end(proc):
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

    def said(self):
        self.log.seek(0)
        return self.log.read().decode(errors='replace')[-3000:]

    def eyes(self, seconds=0.6):
        """Where the owner is told the peer's eyes are: the newest (x, z)."""
        poses = [pose for person, pose in self.owner.got(b'\x81', 'watch', seconds) if person == 1]
        self.assertTrue(poses, 'the owner got no pose of the peer\n' + self.said())
        _, _, x, _, z = struct.unpack_from(POSE_HEAD, poses[-1])
        return x, z, poses

    def test_a_fake_peer_comes_in_stands_walks_speaks_and_leaves(self):
        self.assertEqual(self.owner.ask('open'), 'ok', self.said())
        code = self.owner.ask('invite')
        self.assertTrue(code.startswith('ok '), code)
        self.assertTrue(self.sim.until(lambda s: s.get('together', {}).get('mode') == 'idle'), self.said())
        self.assertEqual(self.sim.ask('join 127.0.0.1 %d %s' % (self.port, code[3:])), 'ok')
        self.assertTrue(self.owner.until(lambda s: s.get('asks')), self.said())
        ask = self.owner.state()['asks'][0]
        self.assertEqual(ask['name'], 'Sim')
        self.assertEqual(self.owner.ask('accept %d' % ask['id']), 'ok')
        self.assertTrue(self.sim.until(lambda s: s['together'].get('mode') == 'joined' and s['together'].get('me') == 1), self.said())

        x, z, poses = self.eyes(1.5)
        self.assertGreater(len(poses), 10, 'a standing peer is told about 30 times a second')
        self.assertTrue(all(pose[MOUTH] == bytes(5) for pose in poses), 'a quiet peer has its mouth shut')

        self.assertEqual(self.sim.ask('walk 1 0 0'), 'ok')
        deadline, moved = time.monotonic() + 5, 0.0
        while time.monotonic() < deadline and not 0.9 < moved < 1.1:
            nx, nz, _ = self.eyes(0.4)
            moved = ((nx - x) ** 2 + (nz - z) ** 2) ** 0.5
        self.assertTrue(0.9 < moved < 1.1, 'a peer told to walk a metre is seen %.2f m from where it was\n%s' % (moved, self.said()))

        self.assertEqual(self.sim.ask('speak 220'), 'ok')
        deadline, voices, mouths = time.monotonic() + 8, 0, 0
        while time.monotonic() < deadline and not (voices > 10 and mouths > 3):
            voices += sum(person == 1 for person, _ in self.owner.got(b'\x82', 'listen', 0.4))
            mouths += sum(pose[MOUTH] != bytes(5) for pose in self.eyes(0.4)[2])
        self.assertGreater(voices, 10, 'the owner hears the peer hum\n' + self.said())
        self.assertGreater(mouths, 3, "the peer's mouth moves while it hums\n" + self.said())
        state = self.sim.state()
        self.assertEqual((state['speaks'], state['walked'], state['running']), (220.0, [1.0, 0.0, 0.0], {'ft-together': True, 'ft-avatars': True, 'ft-roomvoice': True}))

        self.assertTrue(self.sim.ask('wear nothing-here').startswith('error'), 'a model its folder has not got')
        self.assertEqual(self.sim.ask('wear'), 'ok')
        self.assertEqual(self.sim.ask('away game'), 'ok')
        self.assertTrue(self.owner.until(lambda s: len(s['people']) == 2 and s['people'][1]['away'] == 1), self.said())
        self.owner.got(b'\x81', 'watch', 0.3)   # what was on its way
        self.assertEqual([person for person, _ in self.owner.got(b'\x81', 'watch', 0.5)], [], 'nobody stands there while it is away')
        self.assertEqual(self.sim.ask('away back'), 'ok')
        self.eyes()

        self.assertEqual(self.sim.ask('quit'), 'ok')
        self.assertEqual(self.sim_proc.wait(timeout=15), 0, self.said())
        self.assertTrue(self.owner.until(lambda s: len(s['people']) == 1), 'the owner is told the peer left\n' + self.said())
        left = subprocess.run(['pgrep', '-f', 'ft_peer_1' + self.tag], capture_output=True, text=True).stdout.split()
        self.assertEqual([pid for pid in left if int(pid) != os.getpid()], [], 'none of its programs is left')


if __name__ == '__main__':
    unittest.main()
