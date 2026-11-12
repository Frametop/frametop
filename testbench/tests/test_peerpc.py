"""A fake peer's split compute PC (frame_testbench/peerpc.py): what each question says to the
peer's ft-together and starts, with stand-ins for both, and one fake peer for real in the room
of an `ft-together --loopback`: its PC calls it and is let in, its windows are panels of the
peer's in the owner's room, fewer and none when told, and they go with it. Nobody watches them
in that run. Where the Frame's video devices are, a second run has the owner's `ft-peerpanel`
watch one: the loop decodes, round after round, with no encoder opened."""
import os
import subprocess
import unittest

import peerkit
from frame_testbench import peerpc, peersim

FLAT = '1.000000 0.000000 0.000000 0.000000'
WINDOWS = ['share %d 640 360 %s 2.5 -2.5 %s 1 0 Window %d' % (3 + i, x, FLAT, i + 1) for i, x in enumerate(('0.75', '1.85', '2.95', '4.05'))]
WATCHER = peersim.REPO / 'together/build/ft-peerpanel'
VIDEO = WATCHER.is_file() and all(os.path.exists(device) for device in ('/dev/video-dec0', '/dev/dri/renderD128'))
LOOP_PICTURES = 30


class Child:
    def __init__(self, *words):
        self.words, self.ended = words, False

    def poll(self):
        return 0 if self.ended else None


class RulesTest(unittest.TestCase):
    def setUp(self):
        self.asked, self.started = [], []
        self.replies = {'here': 'ok 100.64.0.7 47194 Sim', 'sender': 'ok ABCD2345'}
        self.pc = peerpc.Pc(self.ask, self.start, self.stop, (2.4, 1.5, -2.5))

    def ask(self, text):
        self.asked.append(text)
        return next((reply for start, reply in self.replies.items() if text.startswith(start)), 'ok')

    def start(self, address, port, code):
        self.started.append(Child(address, port, code))
        return self.started[-1]

    @staticmethod
    def stop(child):
        if child is not None:
            child.ended = True

    def state(self):
        return self.pc.state()['pc']

    def test_its_pc_calls_the_peers_own_session_with_a_code_that_one_made_and_has_one_window(self):
        self.assertIsNone(self.state())
        self.assertEqual(self.pc.act('pc', []), 'ok')
        self.assertEqual((self.asked, [child.words for child in self.started]), (['here', 'sender', WINDOWS[0]], [('100.64.0.7', 47194, 'ABCD2345')]))
        self.assertEqual(self.state(), {'running': True, 'windows': 1})

    def test_said_again_it_has_that_many_windows_and_the_same_pc(self):
        self.assertEqual(self.pc.act('pc', ['4']), 'ok')
        self.assertEqual(self.asked[2:], WINDOWS, 'in a row above its own panel, each a metre wide')
        del self.asked[:]
        self.assertEqual(self.pc.act('pc', ['2']), 'ok')
        self.assertEqual(self.asked, WINDOWS[:2] + ['unshare 6', 'unshare 5'])
        self.assertEqual(self.pc.act('pc', []), 'ok')
        self.assertEqual(self.asked[4:], WINDOWS[:2], 'no number: as many as it had')
        self.assertEqual((len(self.started), self.state()), (1, {'running': True, 'windows': 2}))

    def test_a_pc_that_ended_calls_again_with_a_new_code(self):
        self.pc.act('pc', [])
        self.started[0].ended = True
        self.assertEqual(self.state(), {'running': False, 'windows': 1})
        self.replies['sender'] = 'ok WXYZ6789'
        self.assertEqual(self.pc.act('pc', []), 'ok')
        self.assertEqual(([child.words[2] for child in self.started], self.state()), (['ABCD2345', 'WXYZ6789'], {'running': True, 'windows': 1}))

    def test_a_session_that_does_not_say_where_it_is_or_makes_no_code_gets_no_pc(self):
        for what, reply, said in (('here', 'error not now', 'error not now'), ('sender', 'error what', 'error what'), ('here', '', 'error no answer'),
                                  ('here', 'ok 100.64.0.7', 'error no answer'), ('here', 'ok 100.64.0.7 port Sim', 'error no answer'),
                                  ('sender', 'ok', 'error no answer'), ('sender', 'ok A B', 'error no answer')):
            self.replies = {'here': 'ok 100.64.0.7 47194 Sim', 'sender': 'ok ABCD2345', what: reply}
            self.assertEqual(self.pc.act('pc', []), said, (what, reply))
        self.assertEqual((self.started, self.state()), ([], None))
        self.assertNotIn(WINDOWS[0], self.asked)

    def test_a_window_its_session_refuses_is_not_in_the_room_and_the_ones_before_it_are(self):
        self.replies['share 4'] = "error you aren't in a room"
        self.assertEqual(self.pc.act('pc', ['3']), "error you aren't in a room")
        self.assertEqual((self.asked[2:], self.state()), (WINDOWS[:2], {'running': True, 'windows': 1}))
        del self.replies['share 4']
        self.assertEqual(self.pc.act('pc', ['3']), 'ok')
        self.replies['share 3'] = 'error not now'
        del self.asked[:]
        self.assertEqual(self.pc.act('pc', ['1']), 'error not now')
        self.assertEqual((self.asked, self.state()['windows']), (WINDOWS[:1], 3), 'the windows it had stay')

    def test_its_windows_leave_the_room_before_the_peer_does_and_its_pc_stays(self):
        self.pc.rest()
        self.assertEqual(self.asked, [], 'a peer with no PC has nothing to take out of a room')
        self.pc.act('pc', ['3'])
        del self.asked[:]
        self.pc.rest()
        self.assertEqual((self.asked, self.state(), self.started[0].ended), (['unshare 5', 'unshare 4', 'unshare 3'], {'running': True, 'windows': 0}, False))
        self.assertEqual(self.pc.act('pc', []), 'ok')
        self.assertEqual((self.asked[3:], len(self.started)), (WINDOWS[:1], 1), 'in the next room the same PC has a window again')

    def test_its_pc_goes(self):
        self.assertEqual(self.pc.act('unpc', []), 'ok')
        self.assertEqual(self.asked, [])
        self.pc.act('pc', ['2'])
        del self.asked[:]
        self.assertEqual(self.pc.act('unpc', []), 'ok')
        self.assertEqual((self.asked, self.started[0].ended, self.state()), (['unshare 4', 'unshare 3'], True, None))

    def test_other_words_are_refused(self):
        for what, words in (('pc', '0'), ('pc', '5'), ('pc', '-1'), ('pc', 'two'), ('pc', '1.0'), ('pc', '1 2'), ('unpc', '1')):
            with self.assertRaises((ValueError, TypeError), msg=(what, words)):
                if self.pc.act(what, words.split()) is None:
                    raise TypeError   # not one of its questions with that many words
        self.assertEqual((self.asked, self.started), ([], []))
        self.assertIsNone(self.pc.act('share', []), "and what isn't a question about its PC is none of its")


@unittest.skipUnless(peerkit.BUILT, peerkit.NOT_BUILT)
class RealPcTest(peerkit.RealRoom):
    def panels(self, state):
        return {panel['id']: panel for panel in state.get('panels', [])}

    def pc(self):
        return self.sim.state()['pc']

    def test_a_fake_peers_pc_calls_it_and_its_windows_are_the_peers_panels_in_the_room(self):
        self.join()
        self.assertIsNone(self.pc())
        self.assertEqual(self.sim.ask('pc 3'), 'ok', self.said())
        self.assertTrue(self.owner.until(lambda s: list(self.panels(s)) == [11, 12, 13]), 'numbers 3 to 5 of the person in place 1\n' + self.said())
        panel = self.panels(self.owner.state())[12]
        self.assertEqual((panel['owner'], panel['number'], panel['name'], panel['size'], panel['metres'], [round(part, 2) for part in panel['at']]),
                         (1, 4, 'Window 2', [640, 360], 1, [-2.95, 2.5, -2.5]), panel)
        self.assertTrue(self.sim.until(lambda s: 'panel 6 W 0' in self.said(), 5), 'its PC was let in, and heard what to encode\n' + self.said())
        self.assertEqual((self.pc(), self.sim.state()['running']['ft-pcsend']), ({'running': True, 'windows': 3}, True))
        self.assertNotIn('refused', self.said())

        # with a panel of its own beside them, fewer, and none
        self.assertEqual(self.sim.ask('share'), 'ok')
        self.assertTrue(self.owner.until(lambda s: list(self.panels(s)) == [8, 11, 12, 13]), self.said())
        self.assertEqual(self.sim.ask('pc 1'), 'ok')
        self.assertTrue(self.owner.until(lambda s: list(self.panels(s)) == [8, 11]), self.said())
        self.assertEqual(self.sim.ask('pc 5')[:6], 'error ', 'a PC has four panels at the most')
        self.assertEqual(self.sim.ask('unpc'), 'ok')
        self.assertTrue(self.owner.until(lambda s: list(self.panels(s)) == [8]), self.said())
        state = self.sim.state()
        self.assertEqual((state['pc'], state['running']['ft-pcsend']), (None, False))

        # again, and out of the room with the peer
        self.assertEqual(self.sim.ask('pc 2'), 'ok')
        self.assertTrue(self.owner.until(lambda s: list(self.panels(s)) == [8, 11, 12]), self.said())
        self.assertEqual(self.sim.ask('leave'), 'ok')
        self.assertTrue(self.owner.until(lambda s: s['panels'] == []), self.said())
        self.assertTrue(self.sim.until(lambda s: s['pc'] == {'running': True, 'windows': 0}), 'its PC stays with it')

    @unittest.skipUnless(VIDEO, "needs the Frame's video decoder and ft-peerpanel (together/build.sh)")
    def test_its_windows_pictures_decode_round_after_round(self):
        self.join()
        self.assertEqual(self.sim.ask('pc 4'), 'ok', self.said())
        self.assertTrue(self.owner.until(lambda s: list(self.panels(s)) == [11, 12, 13, 14]), self.said())
        count = 2 * LOOP_PICTURES + 10
        view = subprocess.run([str(WATCHER), '--panel', '14', '--socket', self.owner_socket, '--probe', '--frames', str(count)], capture_output=True, text=True,
                              timeout=60)
        pictures = [line.split() for line in view.stdout.splitlines() if line.startswith('picture ')]
        self.assertEqual((view.returncode, len(pictures), {words[2] for words in pictures}), (0, count, {'640x360'}), view.stdout[-1500:] + view.stderr[-1500:])
        numbers = [int(words[4]) for words in pictures]   # each picture says which one it is (together/media/testpattern.h)
        self.assertEqual(max(numbers), numbers[0] + LOOP_PICTURES - 1, numbers)
        self.assertTrue(all(next_ == one + 1 or next_ == numbers[0] for one, next_ in zip(numbers, numbers[1:])), numbers)
        self.assertIn('panel 6 W 1', self.said())
        self.assertNotIn('watched: encoding', self.said(), 'no encoder was opened for it')


if __name__ == '__main__':
    unittest.main()
