"""A fake peer's own panel (frame_testbench/peershare.py): what each question says to the peer's
ft-together and starts, with stand-ins for both, and one fake peer for real in the room of an
`ft-together --loopback`: its test panel is in the owner's room, the owner asks for it and is
given it, what the owner does on it reaches the peer, the owner puts it elsewhere and the room
hears that it stands there, and it is gone once taken back. Nobody watches the panel here, so
the hardware encoder is never opened."""
import struct
import unittest

import peerkit
from frame_testbench import peershare, peerwords

FLAT = '1.000000 0.000000 0.000000 0.000000'
SHARED = 'share 0 1280 720 2.4 1.5 -2.5 ' + FLAT + ' 2 0 Test panel'
NOT_GIVEN = 'error you have no control of that panel'


class Child:
    def __init__(self, *words):
        self.words, self.ended = words, False

    def poll(self):
        return 0 if self.ended else None


class RulesTest(unittest.TestCase):
    def setUp(self):
        self.now, self.asked, self.started, self.replies, self.there, self.unasked = 100.0, [], [], {}, True, []
        self.share = peershare.Share(self.ask, self.came, self.start, self.stop, lambda: self.there, (2.4, 1.5, -2.5), lambda: self.now)

    def came(self):
        came, self.unasked = self.unasked, []
        return came

    def ask(self, text):
        self.asked.append(text)
        return next((reply for start, reply in self.replies.items() if text.startswith(start)), 'ok')

    def start(self, panel, size):
        self.started.append(Child(panel, size))
        return self.started[-1]

    @staticmethod
    def stop(child):
        if child is not None:
            child.ended = True

    def shares(self):
        return self.share.state()['shares']

    def act(self, input_, person=2, panel=17):
        return b'\x85' + bytes([person]) + struct.pack('<BBii', panel, *input_)

    def test_a_panel_is_told_to_its_session_and_then_its_picture_is_started(self):
        self.assertIsNone(self.shares())
        self.assertEqual(self.share.act('share', []), 'ok')
        self.assertEqual((self.asked, [child.words for child in self.started]), ([SHARED], [(0, (1280, 720))]))
        self.assertEqual(self.shares(), {'size': [1280, 720], 'at': [2.4, 1.5, -2.5], 'turn': [1.0, 0.0, 0.0, 0.0], 'running': True, 'inputs': 0, 'last': []})

    def test_said_again_it_moves_the_panel_and_keeps_its_picture(self):
        self.share.act('share', ['1920', '1080'])
        self.assertEqual(self.share.act('share', ['1920', '1080', '-100', '0', '100', '90']), 'ok')
        self.assertEqual(self.share.act('share', []), 'ok')
        moved = 'share 0 1920 1080 -100 0 100 0.707107 0.000000 0.707107 0.000000 2 0 Test panel'
        self.assertEqual(self.asked, ['share 0 1920 1080 2.4 1.5 -2.5 ' + FLAT + ' 2 0 Test panel', moved, moved])
        self.assertEqual(len(self.started), 1)
        self.assertEqual(self.share.act('share', ['1920', '1080', '1', '2', '3']), 'ok')
        self.assertEqual(self.asked[-1], 'share 0 1920 1080 1 2 3 ' + FLAT + ' 2 0 Test panel', 'no turn is none')
        self.assertEqual((self.shares()['at'], self.shares()['size']), ([1.0, 2.0, 3.0], [1920, 1080]))

    def test_another_size_is_another_panel(self):
        self.share.act('share', [])
        self.assertEqual(self.share.act('share', ['640', '360']), 'ok')
        self.assertEqual(self.asked[1:], ['unshare 0', 'share 0 640 360 2.4 1.5 -2.5 ' + FLAT + ' 2 0 Test panel'])
        first, second = self.started
        self.assertEqual((first.ended, second.ended, second.words), (True, False, (0, (640, 360))))

    def test_a_panel_its_session_refuses_has_no_picture(self):
        self.replies['share'] = "error you aren't in a room"
        self.assertEqual(self.share.act('share', []), "error you aren't in a room")
        self.assertEqual((self.started, self.shares()), ([], None))
        self.share.tick()
        self.assertEqual(self.asked, [SHARED], 'and nothing is kept up for it')

    def test_a_picture_that_ended_is_started_again_with_the_next_word(self):
        self.share.act('share', [])
        self.started[0].ended = True
        self.assertFalse(self.shares()['running'])
        self.share.act('share', [])
        self.assertEqual((len(self.started), self.shares()['running']), (2, True))

    def test_sizes_the_encoder_is_not_known_to_take_and_other_words_are_refused(self):
        for words in ('1366 768', '1344 720', '1280 721', '1280 724', '1280 352', '1280 1208', '512 360', '2048 1080', '1280', '1280 720 0', '1280 720 0 0',
                      '1280 720 0 0 0 0 0', '1280 720 100.5 0 0', '1280 720 0 -100.5 0', '1280 720 0 0 nan', '1280 720 0 0 0 361', '1280 720 0 0 0 -361',
                      'x 720', '1280.0 720'):
            with self.assertRaises((ValueError, TypeError), msg=words):
                if self.share.act('share', words.split()) is None:
                    raise TypeError   # not one of its questions with that many words
        for what, words in (('unshare', '0'), ('give', ''), ('give', '4'), ('give', '-1'), ('give', '1 2'), ('give', 'x'), ('take', '4'), ('take', '-1'),
                            ('take', '1 2')):
            with self.assertRaises((ValueError, TypeError), msg=(what, words)):
                if self.share.act(what, words.split()) is None:
                    raise TypeError
        self.assertEqual((self.asked, self.started), ([], []))
        for words in ('640 360', '896 1200', '1408 728', '1664 936', '1920 1080 100 -100 0 360', '1920 1080 0 0 0 -360'):
            self.assertEqual(self.share.act('share', words.split()), 'ok', words)
        self.assertIsNone(self.share.act('point', ['off']), "and what isn't a question about its panel is none of its")

    def test_taken_back_its_picture_ends(self):
        self.share.act('share', [])
        self.assertEqual(self.share.act('unshare', []), 'ok')
        self.assertEqual((self.asked[1:], self.started[0].ended, self.shares()), (['unshare 0'], True, None))
        self.share.rest()
        self.assertEqual(len(self.asked), 2, 'a peer with no panel has nothing to take out of a room')
        self.share.act('share', [])
        self.share.rest()
        self.assertEqual((self.asked[2:], self.started[1].ended, self.shares()), ([SHARED, 'unshare 0'], True, None))

    def test_it_gives_its_panel_as_someone_who_is_there_and_takes_it_back(self):
        self.share.act('share', [])
        del self.asked[:]
        self.assertEqual(self.share.act('give', ['3']), 'ok')
        self.assertEqual(self.share.act('take', ['0']), 'ok')
        self.assertEqual(self.share.act('take', []), 'ok')
        self.assertEqual(self.asked, ['control here', 'control give 0 3', 'control take 0 0', 'control take 0'])
        self.there = False
        self.replies['control give'] = 'error you are away'
        self.assertEqual(self.share.act('give', ['0']), 'error you are away')
        self.assertEqual(self.asked[4:], ['control give 0 0'], "away, it doesn't say it is there")

    def test_while_it_shares_it_says_it_is_there_and_asks_for_the_input_each_second(self):
        self.share.tick()
        self.assertEqual(self.asked, [], 'nothing while it shares nothing')
        self.share.act('share', [])
        del self.asked[:]
        for step in (0, 0.5, 0.25, 0.25):
            self.now += step
            self.share.tick()
        self.assertEqual(self.asked, ['control here', 'inputs', 'moves', 'moves', 'moves', 'control here', 'inputs', 'moves'], 'each said again a second after')
        self.now += 0.5
        self.share.tick()
        self.assertEqual(self.asked[8:], ['moves'])
        self.there = False
        self.now += 5
        self.share.tick()
        self.assertEqual(self.asked[9:], ['inputs', 'moves'], "away, it isn't there, and its panel stays")
        self.share.act('give', ['1'])
        self.there = True
        self.now += 0.75
        self.share.tick()
        self.assertEqual(self.asked[11:], ['control give 0 1', 'control here', 'moves'])
        self.share.act('give', ['1'])
        self.now += 0.75
        self.share.tick()
        self.assertEqual(self.asked[14:], ['control here', 'control give 0 1', 'inputs', 'moves'], 'said with a gift, it is said again a second after')

    def test_where_someone_who_may_put_its_panel_is_where_it_stands(self):
        self.share.act('share', ['1920', '1080'])
        self.replies['moves'] = 'ok 2 0:3:1.0000:1.5000:-2.0000:0.70711:0.00000:0.70711:0.00000 1:3:9.0000:9.0000:9.0000:1.00000:0.00000:0.00000:0.00000'
        self.share.tick()
        self.assertEqual(self.asked[-1], 'share 0 1920 1080 1 1.5 -2 0.707110 0.000000 0.707110 0.000000 2 0 Test panel',
                         'the room is told it stands there now; a place for a panel it has not is not its')
        self.assertEqual((self.shares()['at'], self.shares()['turn']), ([1.0, 1.5, -2.0], [0.70711, 0.0, 0.70711, 0.0]))
        self.assertEqual(len(self.started), 1)
        told = len(self.asked)
        for reply in ('ok 0', 'error not now', 'ok', '', 'ok 1 0:3:1:2', 'ok 1 0:3:1:2:3:1:0:0:0:0', 'ok 1 x:3:1:2:3:1:0:0:0', 'ok 1 0:3:101:2:3:1:0:0:0',
                      'ok 1 0:3:1:2:3:1.5:0:0:0', 'ok 1 0:3:1:nan:3:1:0:0:0'):
            self.replies['moves'] = reply
            self.share.tick()
            self.assertEqual((self.asked[told:], self.shares()['at']), (['moves'], [1.0, 1.5, -2.0]), reply)
            told = len(self.asked)
        self.replies.update({'moves': 'ok 1 0:3:5.0000:5.0000:5.0000:1.00000:0.00000:0.00000:0.00000', 'share': 'error not now'})
        self.share.tick()
        self.assertEqual(self.shares()['at'], [1.0, 1.5, -2.0], "a place its session wouldn't take isn't where it stands")

    def test_the_input_it_is_given_is_counted_and_the_last_of_it_kept(self):
        self.share.act('share', [])
        self.unasked = [self.act((1, 16384, 65535)), b'\x84' + self.act((2, 1, 1))[1:], self.act((2, 1, 1))[:-1], self.act((2, 1, 1)) + b'\0', b'',
                         self.act((2, 1, 1)), self.act((3, 0, -120), person=0), self.act((4, 30, 0)), self.act((0, 0, 0)), self.act((9, 1, 2))]
        self.share.tick()
        self.assertEqual(self.share.inputs, 6, 'taken at each look, so that none waits where it may be dropped')
        shares = self.shares()
        self.assertEqual(shares['last'], [{'person': 2, 'what': 'move', 'a': 16384, 'b': 65535}, {'person': 2, 'what': 'button', 'a': 1, 'b': 1},
                                          {'person': 0, 'what': 'scroll', 'a': 0, 'b': -120}, {'person': 2, 'what': 'key', 'a': 30, 'b': 0},
                                          {'person': 2, 'what': 'let go', 'a': 0, 'b': 0}, {'person': 2, 'what': '9', 'a': 1, 'b': 2}])
        self.unasked = [self.act((1, n, 0)) for n in range(20)]
        shares = self.shares()   # asked between two looks, it is counted too
        self.assertEqual((shares['inputs'], len(shares['last']), shares['last'][0]['a'], shares['last'][-1]['a']), (26, 16, 4, 19))


@unittest.skipUnless(peerkit.BUILT, peerkit.NOT_BUILT)
class RealShareTest(peerkit.RealRoom):
    def panels(self, state):
        return {panel['id']: panel for panel in state.get('panels', [])}

    def shares(self):
        return self.sim.state()['shares']

    def held(self, asker):
        return [held['panel'] for held in peerwords.told(asker.ask('control')).get('held', [])]

    def test_a_fake_peer_shares_a_test_panel_that_the_owner_is_given_uses_and_moves(self):
        self.join()
        self.assertIsNone(self.shares())
        self.assertEqual(self.sim.ask('share'), 'ok', self.said())
        self.assertTrue(self.owner.until(lambda s: list(self.panels(s)) == [8]), 'the room has its panel: number 0 of the person in place 1\n' + self.said())
        panel = self.panels(self.owner.state())[8]
        self.assertEqual((panel['owner'], panel['number'], panel['name'], panel['size'], panel['metres'], panel['curve']), (1, 0, 'Test panel', [1280, 720], 2, 0), panel)
        shares = self.shares()
        self.assertEqual((shares['size'], shares['at'], shares['running'], shares['inputs']), ([1280, 720], [-2.4, 1.5, -2.5], True, 0))
        self.assertTrue(self.sim.state()['running']['ft-sharepanel'])
        self.assertTrue(self.owner.until(lambda s: 'waiting for someone to watch' in self.said(), 5), 'its picture waits for a watcher\n' + self.said())

        # asked for by the owner, and given by the peer
        self.assertEqual(self.owner.ask('gaze pointer hand'), 'ok')   # as ft-roomgaze says of a hand's pointer: a move is kept from nobody
        self.assertEqual(self.owner.ask('control act 8 move 100 100'), NOT_GIVEN)
        self.assertEqual(self.owner.ask('control ask 8'), 'ok')
        self.assertTrue(self.sim.until(lambda s: s['control'].get('asks') == [{'number': 0, 'person': 0}]), self.said())
        self.assertEqual(self.sim.ask('give 0'), 'ok')
        self.assertTrue(self.owner.until(lambda s: self.held(self.owner) == [8]), self.said())
        acts = [('move 16384 65535', 'move', 16384, 65535), ('button 1 1', 'button', 1, 1), ('button 1 0', 'button', 1, 0), ('key 30 1', 'key', 30, 1),
                ('key 30 0', 'key', 30, 0), ('scroll 0 -120', 'scroll', 0, -120)]
        self.assertEqual(self.owner.ask('gaze pointer hand'), 'ok')
        for words, *_ in acts:
            self.assertEqual(self.owner.ask('control act 8 ' + words), 'ok', words)
        self.assertTrue(self.sim.until(lambda s: s['shares']['inputs'] >= len(acts)), 'what the owner does on it reaches the peer\n' + self.said())
        shares = self.shares()
        self.assertEqual((shares['inputs'], shares['last']), (len(acts), [{'person': 0, 'what': what, 'a': a, 'b': b} for _, what, a, b in acts]))
        self.assertTrue(self.sim.until(lambda s: self.held(self.owner) == [8], 3), 'it stands while the peer says it is there')

        # put elsewhere by the owner: the peer takes the place, and the room hears it stands there
        self.assertEqual(self.owner.ask('move 8 1 1.25 -3 0.707107 0 0.707107 0'), 'ok')
        self.assertTrue(self.sim.until(lambda s: s['shares']['at'] == [1.0, 1.25, -3.0]), 'the peer takes where its panel was put\n' + self.said())
        self.assertTrue(self.owner.until(lambda s: [round(part, 3) for part in self.panels(s)[8]['at']] == [1.0, 1.25, -3.0]), self.owner.state())

        # taken back, and away
        self.assertEqual(self.sim.ask('take 0'), 'ok')
        self.assertTrue(self.owner.until(lambda s: self.held(self.owner) == []), self.said())
        self.assertEqual(self.owner.ask('control act 8 button 1 1'), NOT_GIVEN)
        self.assertEqual(self.sim.ask('give 0'), 'ok')
        self.assertTrue(self.owner.until(lambda s: self.held(self.owner) == [8]), self.said())
        self.assertEqual(self.sim.ask('away off'), 'ok')
        self.assertTrue(self.owner.until(lambda s: self.held(self.owner) == []), 'what it gave ends when it goes away\n' + self.said())
        self.assertEqual(list(self.panels(self.owner.state())), [8], 'and its panel stays')
        self.assertEqual(self.sim.ask('away back'), 'ok')
        self.assertEqual(self.shares()['inputs'], len(acts))

        # another size, and out of the room
        self.assertEqual(self.sim.ask('share 640 360'), 'ok')
        self.assertTrue(self.owner.until(lambda s: [panel['size'] for panel in s['panels']] == [[640, 360]]), self.owner.state())
        self.assertEqual(self.sim.ask('unshare'), 'ok')
        self.assertTrue(self.owner.until(lambda s: s['panels'] == []), self.said())
        state = self.sim.state()
        self.assertEqual((state['shares'], state['running']['ft-sharepanel']), (None, False))
        self.assertEqual(self.sim.ask('share'), 'ok')
        self.assertTrue(self.owner.until(lambda s: list(self.panels(s)) == [8]), self.said())
        self.assertEqual(self.sim.ask('leave'), 'ok')
        self.assertTrue(self.sim.until(lambda s: s['shares'] is None and not s['running']['ft-sharepanel']), 'it takes its panel with it when it leaves')


if __name__ == '__main__':
    unittest.main()
