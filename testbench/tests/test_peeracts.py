"""A fake peer's hand and eyes (frame_testbench/peeracts.py): what each question says to the
peer's ft-together, with a stand-in for it, and one fake peer for real in the room of an
`ft-together --loopback` that shares a panel: the owner's programs are told where it points
and get its look in its poses, it asks for the panel, and what it does there reaches the
program at the owner's desktop only once the owner gave it."""
import struct
import time
import unittest

import peerkit
from frame_testbench import peeracts

POINT = '<BHBHHB'   # together/proto/pointwire.h: kind, count, panel, across, down, flags
LOOKS = 0x08        # in a pose's flags, its byte 25; the gaze is at 31, in half degrees (proto/posewire.h)
SCREEN = '1920 1080 0 1.5 -2 1 0 0 0 2 0 Screen'
NOT_GIVEN = 'error you have no control of that panel'


class RulesTest(unittest.TestCase):
    def setUp(self):
        self.now, self.asked, self.sent, self.replies = 100.0, [], [], {}
        self.acts = peeracts.Acts(self.ask, self.sent.append, lambda: self.now)

    def ask(self, text):
        self.asked.append(text)
        return next((reply for start, reply in self.replies.items() if text.startswith(start)), 'ok')

    def pointer(self, panel, x, y, flags):
        return struct.pack(POINT, 3, 0, panel, x, y, flags)

    def test_a_pointer_is_told_at_once_and_again_each_second(self):
        self.assertEqual(self.acts.act('point', ['3', '0.25', '0.75']), 'ok')
        self.assertEqual((self.asked, self.sent), (['gaze pointer hand'], [self.pointer(3, 16384, 49151, 0)]))
        self.now += 0.9
        self.acts.tick()
        self.assertEqual(len(self.sent), 1)
        self.now += 0.1
        self.acts.tick()
        self.acts.tick()
        self.assertEqual((self.asked, self.sent), (['gaze pointer hand'] * 2, [self.pointer(3, 16384, 49151, 0)] * 2))
        self.assertEqual(self.acts.state()['points'], {'panel': 3, 'x': 0.25, 'y': 0.75, 'down': False})
        self.assertEqual(self.acts.act('point', ['31', '1', '0', 'down']), 'ok')
        self.assertEqual(self.acts.act('point', ['0', '0', '1']), 'ok')
        self.assertEqual(self.sent[2:], [self.pointer(31, 65535, 0, 1), self.pointer(0, 0, 65535, 0)])
        self.assertEqual(self.asked.count('gaze pointer hand'), 2, 'its hand was said within the second')

    def test_a_pointer_taken_off_is_told_once(self):
        self.assertEqual(self.acts.act('point', ['off']), 'ok')
        self.assertEqual(self.sent, [], 'it pointed at nothing before')
        self.acts.act('point', ['3', '0.5', '0.5'])
        self.assertEqual(self.acts.act('point', ['off']), 'ok')
        self.now += 5
        self.acts.tick()
        self.assertEqual(self.sent[1:], [self.pointer(255, 0, 0, 0)])
        self.assertIsNone(self.acts.state()['points'])

    def test_a_look_is_shared_and_said_a_few_times_a_second(self):
        self.assertEqual(self.acts.act('look', ['20', '-10.5']), 'ok')
        self.assertEqual(self.asked, ['gaze on', 'gaze say 20 -10.5'])
        self.now += 0.25
        self.acts.tick()
        self.assertEqual(len(self.asked), 2)
        self.now += 0.25
        self.acts.tick()
        self.acts.tick()
        self.assertEqual(self.asked[2:], ['gaze say 20 -10.5'])
        self.assertEqual(self.acts.state()['looks'], [20.0, -10.5])
        self.assertEqual(self.acts.act('look', ['off']), 'ok')
        self.now += 5
        self.acts.tick()
        self.assertEqual((self.asked[-1], self.acts.looks), ('gaze off', None))
        self.assertEqual(self.acts.act('look', ['63.5', '-63.5']), 'ok')
        self.assertEqual(self.asked[-1], 'gaze say 63.5 -63.5', 'as far as a pose can say')

    def test_a_look_ends_with_what_it_was_shared_with(self):
        self.replies = {'gaze on': "error you aren't in a room"}
        self.assertEqual(self.acts.act('look', ['5', '5']), "error you aren't in a room")
        self.assertEqual((self.asked, self.acts.looks), (['gaze on'], None))
        self.replies = {}
        self.acts.act('look', ['5', '5'])
        self.replies = {'gaze say': 'error its ft-together gave no answer (timeout)'}
        self.now += 1
        self.acts.tick()
        self.assertEqual(self.acts.looks, (5.0, 5.0), 'an answer that came late ends nothing')
        self.replies = {'gaze say': peeracts.NOT_SHARED}
        self.now += 1
        self.acts.tick()
        self.assertIsNone(self.acts.looks)
        before = len(self.asked)
        self.now += 1
        self.acts.tick()
        self.assertEqual(len(self.asked), before)

    def test_what_it_does_on_a_panel_is_said_as_a_hand_says_it(self):
        self.replies = {'control ask': 'error no such panel', 'control act 9 key': peeracts.NOT_SHARED}
        self.assertEqual(self.acts.act('ask', ['9']), 'error no such panel')
        for words in (['9', 'move', '0.25', '1'], ['9', 'button', '3', '1'], ['9', 'button', '1', '0'], ['0', 'scroll', '-2400', '2400'], ['9', 'scroll', '99', '0'],
                      ['31', 'key', '111', '1']):
            self.assertEqual(self.acts.act('act', words), 'ok', words)
        self.assertEqual(self.acts.act('act', ['9', 'key', '1', '0']), peeracts.NOT_SHARED, "its ft-together's answer is passed back")
        self.assertEqual(self.asked, ['control ask 9', 'gaze pointer hand', 'control act 9 move 16384 65535', 'control act 9 button 3 1', 'control act 9 button 1 0',
                                      'control act 0 scroll -2400 2400', 'control act 9 scroll 99 0', 'control act 31 key 111 1', 'control act 9 key 1 0'])
        self.now += 0.75
        self.acts.act('act', ['9', 'key', '30', '0'])
        self.assertEqual(self.asked[-2:], ['control act 9 key 1 0', 'control act 9 key 30 0'], 'its hand is said once a second, not with each act')
        self.now += 0.25
        self.acts.act('act', ['9', 'key', '30', '1'])
        self.assertEqual(self.asked[-2:], ['gaze pointer hand', 'control act 9 key 30 1'])
        self.assertEqual(self.sent, [], 'what it does moves no pointer of the room')

    def test_a_panel_is_put_where_it_says_turned_about_the_upright(self):
        self.assertEqual(self.acts.act('put', ['9', '1', '1.5', '-2']), 'ok')
        self.assertEqual(self.acts.act('put', ['9', '-100', '0', '100', '90']), 'ok')
        self.assertEqual(self.acts.act('put', ['9', '0', '0', '0', '-180']), 'ok')
        self.assertEqual(self.asked, ['move 9 1 1.5 -2 1.000000 0 0.000000 0', 'move 9 -100 0 100 0.707107 0 0.707107 0', 'move 9 0 0 0 0.000000 0 -1.000000 0'])

    def test_what_isnt_one_of_its_questions_says_nothing(self):
        for what, rest in (('point', []), ('point', ['3', '0.5']), ('point', ['32', '0.5', '0.5']), ('point', ['-1', '0.5', '0.5']), ('point', ['3', '1.01', '0']),
                           ('point', ['3', '0', '-0.01']), ('point', ['3', 'nan', '0']), ('point', ['3', '0.5', '0.5', 'up']), ('point', ['3.0', '0.5', '0.5']),
                           ('point', ['off', 'down']), ('point', ['3', '0.5', '0.5', 'down', 'down']), ('look', ['64', '0']), ('look', ['0', '-63.6']),
                           ('look', ['inf', '0']), ('look', ['1']), ('look', ['on']), ('look', ['1', '2', '3']), ('ask', []), ('ask', ['32']), ('ask', ['x']),
                           ('ask', ['1', '2']), ('act', ['9', 'move', '0.5']), ('act', ['9', 'move', '2', '0']), ('act', ['9', 'move', '0', 'nan']), ('act', ['9', 'move', '0', '-0.5']),
                           ('act', ['9', 'button', '0', '1']), ('act', ['9', 'button', '4', '1']), ('act', ['9', 'button', '1', '2']), ('act', ['9', 'button', '1', '-1']),
                           ('act', ['9', 'scroll', '2401', '0']), ('act', ['9', 'scroll', '0', '-2401']), ('act', ['9', 'scroll', '1.5', '0']),
                           ('act', ['9', 'key', '0', '1']), ('act', ['9', 'key', '112', '1']), ('act', ['9', 'key', '99', '1']), ('act', ['9', 'key', '30', '2']),
                           ('act', ['9', 'type', '30', '1']), ('act', ['32', 'key', '30', '1']), ('act', ['9', 'key', '30', '1', '1']), ('put', ['9', '0', '0']),
                           ('put', ['9', '100.5', '0', '0']), ('put', ['9', '0', '-101', '0']), ('put', ['9', '0', '0', '0', '361']), ('put', ['9', '0', 'nan', '0']),
                           ('put', ['32', '0', '0', '0']), ('put', ['9', '0', '0', '0', '0', '0']), ('wave', []), ('', [])):
            with self.subTest(what=what, rest=rest):
                try:
                    self.assertIsNone(self.acts.act(what, rest))
                except ValueError:
                    pass
        self.acts.tick()
        self.assertEqual((self.asked, self.sent, self.acts.points, self.acts.looks), ([], [], None, None))

    def test_resting_takes_the_hand_off_and_the_eyes_ahead(self):
        self.acts.rest()
        self.assertEqual((self.asked, self.sent), ([], []), 'a peer that did neither says nothing')
        self.acts.act('point', ['3', '0.5', '0.5', 'down'])
        self.acts.act('look', ['1', '2'])
        self.acts.rest()
        self.assertEqual((self.sent[1:], self.asked[-1], self.acts.points, self.acts.looks), ([self.pointer(255, 0, 0, 0)], 'gaze off', None, None))

    def test_its_state_has_what_its_session_says_of_control_and_gaze(self):
        self.replies = {'control': 'ok {"held": [{"panel": 9, "left": 4.5}]}', 'gaze': 'error gaze | gaze on'}
        self.assertEqual(self.acts.state(), {'points': None, 'looks': None, 'control': {'held': [{'panel': 9, 'left': 4.5}]}, 'gaze': {'error': 'error gaze | gaze on'}})
        self.assertEqual(self.asked, ['control', 'gaze'])


@unittest.skipUnless(peerkit.BUILT, peerkit.NOT_BUILT)
class RealActsTest(peerkit.RealRoom):
    @staticmethod
    def pointed(heard):
        """The peer's pointers among what the owner's program was told: [(panel, across, down, flags)]."""
        return [struct.unpack(POINT, data)[2:] for person, data in heard if person == 1]

    def pointers(self, asker, least, seconds=6.0):
        """...as it asks for them, until there are that many."""
        got, deadline = [], time.monotonic() + seconds
        while len(got) < least and time.monotonic() < deadline:
            got += self.pointed(asker.got(b'\x84', 'points', 0.5))
        return got

    def gazes(self, seconds=0.5):
        """The gaze in each pose of the peer the owner's program gets in that time: half degrees, or None."""
        poses = [pose for person, pose in self.owner.got(b'\x81', 'watch', seconds) if person == 1]
        return [struct.unpack_from('<bb', pose, 31) if pose[25] & LOOKS else None for pose in poses]

    def until(self, what, seconds=6.0):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if what():
                return True
            time.sleep(0.05)
        return False

    def control(self, asker):
        return peeracts.told(asker.ask('control'))

    def test_a_fake_peer_points_looks_asks_for_a_panel_and_uses_it_once_given(self):
        self.join()
        self.assertEqual(self.owner.ask('share 0 ' + SCREEN), 'ok', self.said())
        self.assertTrue(self.sim.until(lambda s: [panel['id'] for panel in s['together']['panels']] == [0]), self.said())
        points, desk = peerkit.Asker(self.owner_socket), peerkit.Asker(self.owner_socket)
        self.addCleanup(points.sock.close)
        self.addCleanup(desk.sock.close)

        # its pointer, for everyone in the room
        self.assertEqual(self.sim.ask('point 0 0.25 0.75'), 'ok')
        got = self.pointers(points, 2)
        self.assertEqual((len(got) >= 2, set(got)), (True, {(0, 16384, 49151, 0)}), 'told where it points, and again while it rests there\n' + self.said())
        self.assertEqual(self.sim.state()['points'], {'panel': 0, 'x': 0.25, 'y': 0.75, 'down': False})
        self.assertEqual(self.sim.ask('point 0 1 0 down'), 'ok')
        self.assertTrue(self.until(lambda: (0, 65535, 0, 1) in self.pointers(points, 1, 0.5)), 'a button held shows')
        self.assertEqual(points.ask('points'), 'ok')   # asked before, so that the one word of it isn't missed
        self.assertEqual(self.sim.ask('point off'), 'ok')
        self.assertEqual(self.pointed(points.heard(b'\x84', 1.0))[-1:], [(255, 0, 0, 0)], 'taken off, it points at nothing')
        self.assertEqual(self.pointed(points.heard(b'\x84', 2.5)), [], '...and says no more')

        # where it looks, in its poses
        self.assertEqual(set(self.gazes()), {None}, 'nothing of where it looks until it shares that')
        self.assertEqual(self.sim.ask('look 20 -10'), 'ok')
        self.assertTrue(self.until(lambda: set(self.gazes(0.3)) == {(40, -20)}), 'its poses say where it looks\n' + self.said())
        state = self.sim.state()
        self.assertEqual((state['looks'], state['gaze']), ([20.0, -10.0], {'shared': True, 'fresh': True, 'quiet': False}))
        time.sleep(1.5)
        self.assertEqual(set(self.gazes()), {(40, -20)}, 'kept up for as long as it looks')
        self.assertEqual(self.sim.ask('look off'), 'ok')
        self.assertTrue(self.until(lambda: set(self.gazes(0.3)) == {None}), 'and no longer once it stops')
        self.assertFalse(self.sim.state()['gaze']['shared'])

        # the owner's panel: asked for, given by the owner, used and put elsewhere
        self.assertEqual(desk.ask('inputs'), 'ok')
        self.assertEqual(self.sim.ask('act 0 move 0.5 0.5'), NOT_GIVEN)
        self.assertEqual(self.sim.ask('put 0 1 1.5 -2 90'), 'error you may not move that panel')
        self.assertEqual(self.sim.ask('ask 0'), 'ok')
        self.assertTrue(self.until(lambda: self.control(self.owner).get('asks') == [{'number': 0, 'person': 1}]), self.said())
        self.assertEqual(self.sim.state()['control']['asked'], [0])
        self.assertEqual((self.owner.ask('control here'), self.owner.ask('control give 0 1')), ('ok', 'ok'))
        self.assertTrue(self.sim.until(lambda s: [held['panel'] for held in s['control'].get('held', [])] == [0]), self.said())
        self.assertEqual(desk.ask('inputs'), 'ok')
        acts = [('move 0.25 1', (1, 16384, 65535)), ('button 1 1', (2, 1, 1)), ('button 1 0', (2, 1, 0)), ('key 30 1', (4, 30, 1)), ('key 30 0', (4, 30, 0)),
                ('scroll 0 -120', (3, 0, -120))]
        for words, _ in acts:
            self.assertEqual(self.sim.ask('act 0 ' + words), 'ok', words)
        self.assertEqual(desk.heard(b'\x85', 0.6), [(1, struct.pack('<BBii', 0, *act)) for _, act in acts],
                         "what it does reaches the program at the owner's desktop, in order, as its own\n" + self.said())
        self.assertEqual(self.sim.ask('put 0 1 1.5 -2 90'), 'ok')
        moved = []
        self.assertTrue(self.until(lambda: moved.extend(self.owner.ask('moves').split()[2:]) or moved), "the owner's Frame hears where it put the panel")
        number, person, *place = (float(part) for part in moved[0].split(':'))
        self.assertEqual((number, person), (0, 1))
        for was, should in zip(place, (1, 1.5, -2, 0.70711, 0, 0.70711, 0)):
            self.assertAlmostEqual(was, should, places=3)

        # taken back: nothing more of it reaches the desktop
        self.assertEqual(self.owner.ask('control take 0'), 'ok')
        self.assertTrue(self.sim.until(lambda s: s['control'].get('held') == []), self.said())
        self.assertEqual((self.sim.ask('act 0 button 1 1'), desk.heard(b'\x85', 0.4)), (NOT_GIVEN, []))


if __name__ == '__main__':
    unittest.main()
