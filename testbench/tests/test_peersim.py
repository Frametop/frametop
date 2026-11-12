"""ft-peer-sim (frame_testbench/peersim.py): what it is asked, with stand-ins for its programs,
and one fake peer for real in the room of an `ft-together --loopback`: it comes in, is seen
standing, moves when told to walk, is heard humming with its mouth moving, and leaves."""
import json
import os
import socket
import struct
import subprocess
import threading
import time
import unittest
from pathlib import Path

import peerkit
from frame_testbench import peersim

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
        self.sent = []
        self.sim.send = self.sent.append
        self.sim.start()
        self.addCleanup(self.sim.line.close)

    def test_it_starts_a_session_of_its_own_and_someone_standing(self):
        together, body = self.started
        self.assertEqual(together.argv, ['/programs/ft-together', '--port', '47195', '--socket', 'ft_peer_2_t', '--name', 'Bo', '--loopback'])
        self.assertEqual((together.env['FT_AVATARS_DIR'], together.env['FT_TOGETHER_QUALITY']), ('/models', 'balanced'))
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

    def test_its_hand_and_eyes_are_asked_what_is_theirs_and_kept_up(self):
        for text in ('point 3 0.5 0.5', 'look 10 5', 'ask 3', 'act 3 key 30 1', 'put 3 0 1 -2'):
            self.assertEqual(self.sim.answer(text), 'ok', text)
        self.assertEqual(self.asked, ['gaze pointer hand', 'gaze on', 'gaze say 10 5', 'control ask 3', 'control act 3 key 30 1', 'move 3 0 1 -2 1.000000 0 0.000000 0'])
        self.assertEqual(len(self.sent), 1)
        self.now += 1
        self.sim.tick()
        self.assertEqual((self.asked[6:], len(self.sent)), (['gaze pointer hand', 'gaze say 10 5'], 2))
        state = json.loads(self.sim.answer('state')[3:])
        self.assertEqual((state['points']['panel'], state['looks'], state['control'], state['gaze']), (3, [10.0, 5.0], {'error': 'ok'}, {'error': 'ok'}))

    def test_a_pointer_goes_to_its_own_session_and_is_lost_when_none_listens(self):
        del self.sim.send   # the Sim's own again
        self.sim.send(b'nobody there')
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM | socket.SOCK_CLOEXEC) as session:
            session.bind('\0ft_peer_2_t')
            session.settimeout(2)
            self.assertEqual(self.sim.answer('point 3 0.5 0.5 down'), 'ok')
            self.assertEqual(session.recv(64), struct.pack('<BHBHHB', 3, 0, 3, 32768, 32768, 1))

    def test_its_questions_go_to_its_own_session_and_what_comes_unasked_is_its_panels(self):
        del self.sim.ask
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM | socket.SOCK_CLOEXEC) as session:
            session.bind('\0ft_peer_2_t')
            session.settimeout(2)
            asked = []

            def answer():
                data, who = session.recvfrom(64)
                asked.append(data)
                session.sendto(b'\x85\x02' + struct.pack('<BBii', 16, 2, 1, 1), who)
                session.sendto(b'ok ' + data, who)

            thread = threading.Thread(target=answer)
            thread.start()
            self.assertEqual(self.sim.ask('wear Robot'), 'ok wear Robot')
            thread.join()
        self.assertEqual(asked, [b'wear Robot'])
        self.sim.ask = lambda text: 'ok'
        self.sim.answer('share')
        self.assertEqual(self.sim.share.state()['shares']['last'], [{'person': 2, 'what': 'button', 'a': 1, 'b': 1}])

    def test_its_panel_is_a_program_of_its_own_and_is_kept_up_and_ended_with_it(self):
        self.assertEqual(self.sim.answer('share 1920 1080'), 'ok')
        self.assertEqual(self.started[-1].argv, ['/programs/ft-sharepanel', '--panel', '0', '--size', '1920x1080', '--fps', '30', '--socket', 'ft_peer_2_t',
                                                 '--pattern'])
        self.assertEqual(self.asked, ['share 0 1920 1080 0 1.5 -2.5 1.000000 0.000000 0.000000 0.000000 2 0 Test panel'])
        state = json.loads(self.sim.answer('state')[3:])
        self.assertEqual((state['shares']['size'], state['shares']['at'], state['running']['ft-sharepanel']), ([1920, 1080], [0.0, 1.5, -2.5], True))
        del self.asked[:]
        self.sim.tick()
        self.assertEqual(self.asked, ['control here', 'inputs', 'moves'], 'its wearer is there, and what its panel is given and where it was put are asked')
        self.assertEqual(self.sim.answer('away game'), 'ok')
        self.now += 2
        del self.asked[:]
        self.sim.tick()
        self.assertEqual(self.asked, ['inputs', 'moves'], "away, its wearer isn't there")
        for text in ('give 0', 'take 0', 'take', 'unshare'):
            self.assertEqual(self.sim.answer(text), 'ok', text)
        self.assertEqual(self.asked[2:], ['control give 0 0', 'control take 0 0', 'control take 0', 'unshare 0'])
        self.assertTrue(self.started[-1].ended)
        for text in ('leave', 'join 100.64.0.1 47193 ABCD2345'):
            self.sim.answer('share')
            del self.asked[:]
            self.assertEqual(self.sim.answer(text), 'ok')
            self.assertEqual((self.asked[:], self.started[-1].ended, json.loads(self.sim.answer('state')[3:])['shares']), (['unshare 0', text], True, None), text)
        self.sim.answer('share')
        self.sim.end()
        self.assertTrue(all(child.ended for child in self.started))
        self.assertIn('share [W H [X Y Z [DEGREES]]] | unshare | give PERSON | take [PERSON] | quit', self.sim.answer('share 1366 768'))

    def test_it_points_and_looks_at_nothing_once_it_leaves_goes_away_or_joins(self):
        for text in ('leave', 'away off', 'join 100.64.0.1 47193 ABCD2345'):
            self.sim.answer('point 3 0.5 0.5 down')
            self.sim.answer('look 10 5')
            del self.asked[:], self.sent[:]
            self.assertEqual(self.sim.answer(text), 'ok')
            self.assertEqual((self.asked, len(self.sent)), (['gaze off', text], 1), text)
            state = json.loads(self.sim.answer('state')[3:])
            self.assertEqual((state['points'], state['looks']), (None, None), text)
        self.sim.answer('point 3 0.5 0.5')
        self.assertEqual(self.sim.answer('away back'), 'ok')
        self.assertEqual(json.loads(self.sim.answer('state')[3:])['points']['panel'], 3, 'coming back takes its hand off nothing')

    def test_what_isnt_a_question_starts_nothing_and_asks_nothing(self):
        for text in ('', 'pose fly', 'pose', 'walk 1 2', 'walk 99 0 0', 'walk nan 0 0', 'walk a b c', 'speak', 'speak 10', 'speak nan',
                     'speak 440 880', 'away', 'away gone', 'join host 47193', 'join ho;st 47193 CODE', 'join host 0 CODE',
                     'join host 70000 CODE', 'join host port CODE', 'join host 47193 co/de', 'wear ../x', 'wear ' + 'a' * 65,
                     'state now', 'leave now', 'run ls', 'join -h 1 CODE', 'point 32 0 0', 'point 3 2 0', 'look 90 0', 'ask panel', 'act 3 key 99 1',
                     'act 3 press 1 1', 'put 3 0 0', 'control give 0 1', 'gaze on', 'move 3 0 0 0 1 0 0 0'):
            self.assertTrue(self.sim.answer(text).startswith('error state | join'), text)
        self.assertEqual((len(self.started), self.asked, self.sent), (2, [], []))

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


@unittest.skipUnless(peerkit.BUILT, peerkit.NOT_BUILT)
class FakePeerTest(peerkit.RealRoom):
    def test_a_fake_peer_comes_in_stands_walks_speaks_and_leaves(self):
        self.join()

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
        self.assertEqual((state['speaks'], state['walked'], state['running']), (220.0, [1.0, 0.0, 0.0], {'ft-together': True, 'ft-avatars': True, 'ft-roomvoice': True, 'ft-sharepanel': False}))

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
