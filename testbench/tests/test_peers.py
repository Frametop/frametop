"""The fake peers' client (frame_testbench/peers.py) and its CLI commands (test_mcp_peers.py has
the MCP tools): against stand-ins on the sockets, and once for real, a peer started, let into the room of an
`ft-together --loopback`, told to wave and stopped."""
import os
import socket
import subprocess
import threading
import unittest
from unittest import mock

from frame_testbench import cli, peer_cli, peers, peersim

TAG = '_t%d' % os.getpid()


class StandIn:
    """Something that answers on a Frametop datagram socket."""

    def __init__(self, name, answer):
        self.asked, self.answer = [], answer
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM | socket.SOCK_CLOEXEC)
        self.sock.bind('\0' + name)
        self.sock.settimeout(0.05)
        self.stopped = False
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        while not self.stopped:
            try:
                data, who = self.sock.recvfrom(2048)
            except OSError:
                continue
            self.asked.append(data.decode())
            reply = self.answer(data.decode())
            if reply is not None:
                self.sock.sendto(reply.encode(), who)

    def close(self):
        self.stopped = True
        self.thread.join()
        self.sock.close()


class ClientTest(unittest.TestCase):
    def stand_in(self, name, answer):
        made = StandIn(name, answer)
        self.addCleanup(made.close)
        return made

    def test_nobody_there_is_told_apart_from_no_answer_and_from_no(self):
        self.assertIsNone(peers.exchange('ft_peer_sim_nobody' + TAG, 'state'))
        with self.assertRaisesRegex(RuntimeError, "peer 1 isn't running"):
            peers.ask(1, 'state', TAG)
        self.stand_in(peersim.sim_socket(1, TAG), lambda text: None if text == 'state' else 'error no such pose')
        with self.assertRaisesRegex(RuntimeError, "did not answer 'state'"):
            peers.exchange(peersim.sim_socket(1, TAG), 'state', timeout=0.2)
        with self.assertRaisesRegex(RuntimeError, 'peer 1: error no such pose'):
            peers.tell(1, 'pose fly', TAG)

    def test_what_cant_start_is_said_before_anything_runs(self):
        run = mock.Mock()
        for name, models, why in (('', None, 'a name is'), ('a/b', None, 'a name is'), ('x' * 33, None, 'a name is'),
                                  ('Bo', '/nowhere/at/all', 'not a folder')):
            with self.assertRaisesRegex(ValueError, why):
                peers.start(2, name, models=models, tag=TAG, run=run, boxed=False)
        self.stand_in(peersim.sim_socket(2, TAG), lambda text: 'ok {}')
        with self.assertRaisesRegex(RuntimeError, 'peer 2 is running already'):
            peers.start(2, 'Bo', tag=TAG, run=run, boxed=False)
        run.assert_not_called()

    def test_on_the_frame_a_peer_is_a_user_unit_in_the_programs_container(self):
        def run(argv, **how):
            self.argv, self.how = argv, how
            self.stand_in(peersim.sim_socket(3, TAG), lambda text: 'ok {"peer": 3}')

        self.assertEqual(peers.start(3, 'Cy One', loopback=True, tag=TAG, run=run, boxed=False), {'peer': 3, 'ok': True})
        root = str(peers.ROOT)
        self.assertEqual(self.argv, ['systemd-run', '--user', '--quiet', '--collect', '--unit=frame-testbench-peer-3',
                                     '--working-directory=' + root, str(peers.ROOT.parent / 'scripts/in-box'), 'python3',
                                     root + '/ft-peer-sim', '3', '--name', 'Cy One', '--loopback', '--tag', TAG])
        self.assertEqual(self.how, {'check': True, 'timeout': 20})

    def test_a_peer_that_wont_end_is_told_with_a_signal_and_reported(self):
        self.assertFalse(peers.stop(1, TAG))
        sim = self.stand_in(peersim.sim_socket(1, TAG), lambda text: 'ok')
        run = mock.Mock()
        with mock.patch.object(peers, 'STOP_S', 0.3), self.assertRaisesRegex(RuntimeError, 'did not end when asked'):
            peers.stop(1, TAG, run=run)
        self.assertEqual(sim.asked[0], 'quit')
        run.assert_called_once_with(['pkill', '-TERM', '-f', 'ft-peer-sim 1( |$)'], timeout=10)
        with mock.patch.object(peers, 'STOP_S', 0.3):
            self.assertEqual(peers.stop_all(TAG, run), {'stopped': [], 'stuck': {'1': 'peer 1 did not end when asked, and was told to with SIGTERM'}})

    def test_a_peer_joins_only_a_room_that_is_open_here(self):
        with self.assertRaisesRegex(RuntimeError, "Frametop Together isn't on"):
            peers.join_here(1, 'ft_peer_test_owner' + TAG, TAG)
        with self.assertRaisesRegex(ValueError, 'not a socket name'):
            peers.join_here(1, 'a b', TAG)
        owner = self.stand_in('ft_peer_test_owner' + TAG, lambda text: 'ok {"mode": "idle", "asks": []}')
        with self.assertRaisesRegex(RuntimeError, 'no room of its own open'):
            peers.join_here(1, 'ft_peer_test_owner' + TAG, TAG)
        self.assertEqual(owner.asked, ['state'])

    def test_a_peer_is_invited_and_its_own_ask_and_no_other_is_accepted(self):
        mode = {'now': 'idle'}

        def owner_says(text):
            if text == 'state':
                return 'ok {"mode": "hosting", "asks": [{"id": 4, "name": "Eve", "address": "100.64.0.7"}, ' \
                       '{"id": 5, "name": "Bo", "address": "100.64.0.9"}, {"id": 6, "name": "Bo", "address": "100.64.0.7"}]}'
            if text == 'accept 6':
                mode['now'] = 'joined'
            return {'here': 'ok 100.64.0.7 47193 Frame', 'invite': 'ok ABCD2345', 'accept 6': 'ok'}.get(text, 'error no')

        owner = self.stand_in('ft_peer_test_owner' + TAG, owner_says)
        sim = self.stand_in(peersim.sim_socket(2, TAG), lambda text: 'ok' if text != 'state' else
                            'ok {"name": "Bo", "together": {"mode": "%s", "me": 2, "people": [{}, {}, {}]}}' % mode['now'])
        self.assertEqual(peers.join_here(2, 'ft_peer_test_owner' + TAG, TAG), {'ok': True, 'peer': 2, 'place': 2, 'people': 3})
        self.assertIn('join 100.64.0.7 47193 ABCD2345', sim.asked)
        self.assertEqual([text for text in owner.asked if text.startswith(('accept', 'decline'))], ['accept 6'])

    def test_a_peer_that_is_refused_says_why(self):
        self.stand_in('ft_peer_test_owner' + TAG, lambda text: {'state': 'ok {"mode": "hosting", "asks": []}', 'here': 'ok 100.64.0.7 47193 Frame',
                                                                'invite': 'ok ABCD2345'}.get(text, 'error no'))
        states = iter(['idle', 'calling', 'idle'])
        self.stand_in(peersim.sim_socket(2, TAG), lambda text: 'ok' if text != 'state' else
                      'ok {"name": "Bo", "together": {"mode": "%s", "note": "The room is full."}}' % next(states, 'idle'))
        with self.assertRaisesRegex(RuntimeError, 'peer 2 was not let in: The room is full.'):
            peers.join_here(2, 'ft_peer_test_owner' + TAG, TAG)


class CommandsTest(unittest.TestCase):
    def test_each_command_is_one_call(self):
        calls = []
        patches = {name: mock.patch.object(peers, name, side_effect=lambda *a, _name=name: calls.append((_name,) + a) or {'ok': True})
                   for name in ('start', 'stop', 'state', 'join_here', 'tell')}
        for patch in patches.values():
            patch.start()
            self.addCleanup(patch.stop)
        cases = [(['peer', '1', 'start'], ('start', 1, None, False, None)),
                 (['peer', '2', 'start', '--name', 'Bo', '--loopback', '--models', '/m'], ('start', 2, 'Bo', True, '/m')),
                 (['peer', '3', 'stop'], ('stop', 3)), (['peer', '1', 'state'], ('state', 1)),
                 (['peer', '1', 'join'], ('join_here', 1, 'ft_together')),
                 (['peer', '1', 'join', '--together', 'ft_other'], ('join_here', 1, 'ft_other')),
                 (['peer', '1', 'join', 'frame', '47193', 'ABCD2345'], ('tell', 1, 'join frame 47193 ABCD2345')),
                 (['peer', '1', 'leave'], ('tell', 1, 'leave')), (['peer', '1', 'pose', 'wave'], ('tell', 1, 'pose wave')),
                 (['peer', '1', 'walk', '1', '0.5', '90'], ('tell', 1, 'walk 1.0 0.5 90.0')),
                 (['peer', '1', 'walk', '--', '-1.5', '0', '-45'], ('tell', 1, 'walk -1.5 0.0 -45.0')),
                 (['peer', '1', 'wear', 'Robot'], ('tell', 1, 'wear Robot')), (['peer', '1', 'wear'], ('tell', 1, 'wear')),
                 (['peer', '1', 'speak', '220'], ('tell', 1, 'speak 220')), (['peer', '1', 'speak', 'off'], ('tell', 1, 'speak off')),
                 (['peer', '2', 'away', 'game'], ('tell', 2, 'away game')),
                 (['peer', '1', 'point', '8', '0.25', '0.75'], ('tell', 1, 'point 8 0.25 0.75')),
                 (['peer', '1', 'point', '8', '0.25', '0.75', '--down'], ('tell', 1, 'point 8 0.25 0.75 down')),
                 (['peer', '1', 'point', 'off'], ('tell', 1, 'point off')),
                 (['peer', '1', 'look', '20', '5'], ('tell', 1, 'look 20 5')), (['peer', '1', 'look', '--', '-20', '-5.5'], ('tell', 1, 'look -20 -5.5')),
                 (['peer', '1', 'look', 'off'], ('tell', 1, 'look off')), (['peer', '3', 'ask', '31'], ('tell', 3, 'ask 31')), (['peer', '3', 'ask', '0'], ('tell', 3, 'ask 0')),
                 (['peer', '1', 'act', '8', 'move', '0.5', '1'], ('tell', 1, 'act 8 move 0.5 1')),
                 (['peer', '1', 'act', '8', 'button', '1', '0'], ('tell', 1, 'act 8 button 1 0')),
                 (['peer', '1', 'act', '8', 'scroll', '--', '0', '-120'], ('tell', 1, 'act 8 scroll 0 -120')),
                 (['peer', '1', 'act', '8', 'key', '30', '1'], ('tell', 1, 'act 8 key 30 1')),
                 (['peer', '1', 'put', '8', '1', '1.5', '2'], ('tell', 1, 'put 8 1.0 1.5 2.0 0.0')),
                 (['peer', '1', 'put', '8', '--', '-1', '1.5', '-2', '-90'], ('tell', 1, 'put 8 -1.0 1.5 -2.0 -90.0')),
                 (['peer', '1', 'share'], ('tell', 1, 'share 1280 720')), (['peer', '2', 'share', '--size', '1920x1080'], ('tell', 2, 'share 1920 1080')),
                 (['peer', '1', 'share', '--size', '640x360', '--at', '-1', '1.5', '-2.5'], ('tell', 1, 'share 640 360 -1.0 1.5 -2.5 0.0')),
                 (['peer', '1', 'share', '--at', '1', '0', '2', '--turn', '-90'], ('tell', 1, 'share 1280 720 1.0 0.0 2.0 -90.0')),
                 (['peer', '1', 'unshare'], ('tell', 1, 'unshare')), (['peer', '1', 'give', '0'], ('tell', 1, 'give 0')), (['peer', '1', 'give', '3'], ('tell', 1, 'give 3')),
                 (['peer', '1', 'take'], ('tell', 1, 'take')), (['peer', '1', 'take', '0'], ('tell', 1, 'take 0')), (['peer', '3', 'take', '3'], ('tell', 3, 'take 3')),
                 (['peer', '1', 'pc'], ('tell', 1, 'pc')), (['peer', '2', 'pc', '4'], ('tell', 2, 'pc 4')), (['peer', '1', 'unpc'], ('tell', 1, 'unpc'))]
        for argv, expected in cases:
            with self.subTest(argv=argv):
                self.assertTrue(peer_cli.dispatch(cli.parser().parse_args(argv))['ok'])
                self.assertEqual(calls[-1], expected)
        with self.assertRaisesRegex(ValueError, 'join takes HOST PORT CODE'):
            peer_cli.dispatch(cli.parser().parse_args(['peer', '1', 'join', 'frame', '47193']))
        with self.assertRaisesRegex(ValueError, '--turn goes with --at'):
            peer_cli.dispatch(cli.parser().parse_args(['peer', '1', 'share', '--turn', '90']))
        self.assertEqual(len(calls), len(cases))

    def test_what_isnt_a_command_is_refused_by_the_parser(self):
        for argv in (['peer', '0', 'state'], ['peer', '4', 'state'], ['peer', '1'], ['peer', '1', 'pose', 'fly'], ['peer', '1', 'speak', '10'],
                     ['peer', '1', 'speak', 'nan'], ['peer', '1', 'away', 'gone'], ['peer', '1', 'walk', '1', '2'], ['peer', '1', 'point'],
                     ['peer', '1', 'look'], ['peer', '1', 'ask'], ['peer', '1', 'ask', '32'], ['peer', '1', 'ask', '-1'], ['peer', '1', 'ask', 'screen'],
                     ['peer', '1', 'act', '32', 'key', '30', '1'], ['peer', '1', 'act', '8', 'type', '30', '1'], ['peer', '1', 'act', '8', 'key', '30'],
                     ['peer', '1', 'put', '8', '1', '2'], ['peer', '1', 'put', '32', '1', '2', '3'], ['peer', '1', 'put', '8', '1', '2', 'far'],
                     ['peer', '1', 'share', '--size', '1366x768'], ['peer', '1', 'share', '--size', '1280x721'], ['peer', '1', 'share', '--size', '1280x352'],
                     ['peer', '1', 'share', '--size', '1280x1208'], ['peer', '1', 'share', '--size', '1280'], ['peer', '1', 'share', '--size', '1280x720x2'],
                     ['peer', '1', 'share', '--at', '1', '2'], ['peer', '1', 'share', '1280'], ['peer', '1', 'unshare', '0'], ['peer', '1', 'give'],
                     ['peer', '1', 'give', '4'], ['peer', '1', 'give', '-1'], ['peer', '1', 'take', '4'], ['peer', '1', 'take', 'all'],
                     ['peer', '1', 'pc', '0'], ['peer', '1', 'pc', '5'], ['peer', '1', 'pc', '1', '2'], ['peer', '1', 'unpc', '1']):
            with self.subTest(argv=argv), self.assertRaises(SystemExit), mock.patch('sys.stderr'):
                cli.parser().parse_args(argv)

    def test_release_stops_the_peers_before_it_needs_steamvr(self):
        order = []

        class Bench:
            def fakes(self, command, start=False):
                order.append('fakes')

            def control(self, command):
                order.append('control')
                return {'ok': True}

        with mock.patch.object(peers, 'stop_all', side_effect=lambda: order.append('peers') or {'stopped': [2]}), \
                mock.patch.object(cli.hostinput, 'settle_and_clear_flag', side_effect=lambda: order.append('flag')):
            result = cli.dispatch(cli.parser().parse_args(['release']), Bench())
        self.assertEqual((order, result['peers']), (['peers', 'fakes', 'control', 'flag'], {'stopped': [2]}))


@unittest.skipUnless(peers.in_box() and all(path.is_file() for path in peersim.PROGRAMS.values()),
                     'Frametop Together is not built here (together/build.sh, screens/build.sh, in their container)')
class RealPeerTest(unittest.TestCase):
    def test_a_peer_is_started_let_in_told_to_wave_and_stopped(self):
        owner_socket = 'ft_peer_test_real' + TAG
        port = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        port.bind(('127.0.0.1', 0))
        owner = subprocess.Popen([str(peersim.PROGRAMS['ft-together']), '--loopback', '--name', 'Owner', '--port', str(port.getsockname()[1]),
                                  '--socket', owner_socket], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        port.close()
        self.addCleanup(owner.wait)
        self.addCleanup(owner.terminate)
        self.addCleanup(peers.stop_all, TAG)
        started = peers.start(2, 'Bo', loopback=True, tag=TAG)
        self.assertEqual((started['ok'], started['name'], started['pose'], started['together']['mode']), (True, 'Bo', 'stand', 'idle'))
        with self.assertRaisesRegex(RuntimeError, 'no room of its own open'):
            peers.join_here(2, owner_socket, TAG)
        self.assertEqual(peers.exchange(owner_socket, 'open'), 'ok')
        self.assertEqual(peers.join_here(2, owner_socket, TAG), {'ok': True, 'peer': 2, 'place': 1, 'people': 2})
        with self.assertRaisesRegex(RuntimeError, "can't join now: its session is joined"):
            peers.join_here(2, owner_socket, TAG)
        self.assertEqual(peers.tell(2, 'pose wave', TAG), {'ok': True, 'peer': 2, 'reply': 'ok'})
        self.assertEqual(peers.state(2, TAG)['pose'], 'wave')
        self.assertEqual(peers.stop_all(TAG), {'stopped': [2]})
        self.assertFalse(peers.stop(2, TAG))
        self.assertEqual(len(peers.owner_state(owner_socket)['people']), 1)


if __name__ == '__main__':
    unittest.main()
