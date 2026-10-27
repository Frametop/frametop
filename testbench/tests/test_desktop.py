"""Frametop's control sockets (frame_testbench.desktop), against stand-ins for the input relay,
the pointer helper and ft-powerd on private abstract names. No Frametop needed."""
import socket
import threading
import unittest
import uuid
from unittest import mock

from frame_testbench import cli, desktop


class Service:
    """An abstract datagram socket that keeps what it's sent and answers from a table."""

    def __init__(self, test, answers=None):
        self.name = 'tb-test-' + uuid.uuid4().hex[:12]
        self.answers = answers or {}
        self.got = []
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.sock.bind('\0' + self.name)
        self.sock.settimeout(0.2)
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.run)
        self.thread.start()
        test.addCleanup(self.close)

    def run(self):
        while not self.stop.is_set():
            try:
                data, addr = self.sock.recvfrom(65536)
            except socket.timeout:
                continue
            text = data.decode()
            self.got.append(text)
            answer = self.answers.get(text)
            if answer is not None and addr:
                self.sock.sendto(answer.encode(), addr)

    def close(self):
        self.stop.set()
        self.thread.join()
        self.sock.close()


def events(message):
    """'hostev testbench KIND t c v ... 0 0 0' -> [(t, c, v), ...] without the report's end."""
    words = message.split()[3:]
    triples = [tuple(map(int, words[i:i + 3])) for i in range(0, len(words), 3)]
    assert triples[-1] == (0, 0, 0), message
    return triples[:-1]


class KeyNameTests(unittest.TestCase):
    def test_names_aliases_and_codes(self):
        for name, code in (('enter', 28), ('Return', 28), ('a', 30), ('z', 44), ('1', 2), ('0', 11),
                           ('f1', 59), ('f10', 68), ('f12', 88), ('ctrl', 29), ('altgr', 100), ('super', 125),
                           ('KEY_SPACE', 57), ('up', 103), ('pgdn', 109), ('183', 183)):
            with self.subTest(name=name):
                self.assertEqual(desktop.key_code(name), code)
        self.assertEqual(desktop.combo('ctrl+shift+t'), [29, 42, 20])
        for bad in ('enterr', '0x1c', '768', '', 'ctrl+ctrl', 'a+b+c+d+e+f+g', 'ctrl+'):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                desktop.combo(bad)


class DeviceTests(unittest.TestCase):
    def setUp(self):
        self.relay = Service(self)
        patch = mock.patch.object(desktop, 'RELAY', self.relay.name)
        patch.start()
        self.addCleanup(patch.stop)

    def wait_for(self, last):
        for _ in range(100):
            if self.relay.got and self.relay.got[-1].startswith(last):
                return self.relay.got
            threading.Event().wait(0.01)
        self.fail(f'no {last!r}: {self.relay.got}')

    def test_keys_press_in_order_and_let_go_in_reverse(self):
        desktop.press_keys(['ctrl+l', 'enter'], gap=0)
        got = self.wait_for('hostbye')
        self.assertEqual(got[0], 'hostdev testbench keyboard Frame testbench')
        self.assertEqual(got[-1], 'hostbye testbench keyboard')
        keys = [e for m in got if m.startswith('hostev testbench keyboard ') for e in events(m)]
        self.assertEqual(keys, [(1, 29, 1), (1, 38, 1), (1, 38, 0), (1, 29, 0), (1, 28, 1), (1, 28, 0)])
        with self.assertRaises(ValueError):
            desktop.press_keys([])

    def test_text_goes_to_the_relay_as_typing(self):
        self.assertEqual(desktop.type_text('héllo wörld'), {'ok': True, 'typed': 11})
        self.assertEqual(self.wait_for('type'), ['type héllo wörld'])
        for bad in ('', 'two\nlines', 'x' * 2001, 'tab\there'):
            with self.subTest(bad=bad[:10]), self.assertRaises(ValueError):
                desktop.type_text(bad)

    def test_a_move_comes_in_mouse_sized_pieces_and_a_drag_holds_its_button(self):
        desktop.mouse_move(-100, 30, button='left')
        got = self.wait_for('hostbye')
        self.assertEqual(got[0], 'hostdev testbench mouse Frame testbench')
        moves = [events(m) for m in got if m.startswith('hostev')]
        self.assertEqual(moves[0], [(1, 0x110, 1)])
        self.assertEqual(moves[-1], [(1, 0x110, 0)])
        rel = [e for m in moves[1:-1] for e in m]
        self.assertTrue(all(t == 2 and abs(v) <= 40 for t, _, v in rel), rel)
        self.assertEqual(sum(v for _, c, v in rel if c == 0), -100)
        self.assertEqual(sum(v for _, c, v in rel if c == 1), 30)
        for bad in ((20001, 0, None), (0, 0, 'thumb')):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                desktop.mouse_move(*bad)

    def test_clicks_and_scrolls(self):
        desktop.mouse_click('right', 2)
        presses = [e for m in self.wait_for('hostbye') if m.startswith('hostev') for e in events(m)]
        self.assertEqual(presses, [(1, 0x111, 1), (1, 0x111, 0)] * 2)
        self.relay.got.clear()
        desktop.mouse_scroll(-3, horizontal=True)
        wheel = [e for m in self.wait_for('hostbye') if m.startswith('hostev') for e in events(m)]
        self.assertEqual(wheel, [(2, 6, -1)] * 3)
        for bad in (lambda: desktop.mouse_click('left', 4), lambda: desktop.mouse_scroll(0),
                    lambda: desktop.mouse_scroll(51)):
            with self.assertRaises(ValueError):
                bad()


class ServiceTests(unittest.TestCase):
    def serve(self, attr, answers):
        service = Service(self, answers)
        patch = mock.patch.object(desktop, attr, service.name)
        patch.start()
        self.addCleanup(patch.stop)
        return service

    def test_pointer_state_and_commands(self):
        helper = self.serve('POINTER', {'gaze ? headset': 'ok on worn', 'head': 'ok 0.1000 1.6000 -0.2000 12.50 -3.00'})
        self.assertEqual(desktop.pointer_state(), {'ok': True, 'gaze_mode': True, 'worn': True,
                                                   'head': {'position': [0.1, 1.6, -0.2], 'yaw': 12.5, 'pitch': -3.0}})
        helper.answers.update({'gaze ? headset': 'ok off away', 'head': 'error no head pose (headset off?)'})
        state = desktop.pointer_state()
        self.assertEqual((state['gaze_mode'], state['worn']), (False, False))
        self.assertIn('no head pose', state['head']['error'])
        desktop.pointer('recenter')
        with self.assertRaises(ValueError):
            desktop.pointer('reload')

    def test_overlays_and_measure(self):
        helper = self.serve('POINTER', {
            'overlays': '{"t": "overlays", "list": [{"key": "a.b", "name": "A", "visible": true}]}',
            'measure a.b': 'ok 0.1 1.5 -1.0 1.2 0.7 1 0 0 0 1 0 0 0 1',
            'measure gone': 'error no overlay gone'})
        self.assertEqual(desktop.overlays()['overlays']['list'][0]['key'], 'a.b')
        self.assertEqual(desktop.measure('a.b'), {'ok': True, 'overlay': 'a.b', 'center': [0.1, 1.5, -1.0],
                                                  'width': 1.2, 'height': 0.7, 'right': [1.0, 0.0, 0.0],
                                                  'up': [0.0, 1.0, 0.0], 'front': [0.0, 0.0, 1.0]})
        with self.assertRaisesRegex(RuntimeError, 'no overlay gone'):
            desktop.measure('gone')
        for bad in ('', 'a b', 'x' * 128):
            with self.subTest(bad=bad[:10]), self.assertRaises(ValueError):
                desktop.measure(bad)
        self.assertEqual(helper.got, ['overlays', 'measure a.b', 'measure gone'])

    def test_power(self):
        self.serve('POWERD', {'status': 'ok off 640 600', 'on': 'ok',
                              'off': "error the displays are already off, or the backlight can't be written"})
        self.assertEqual(desktop.power('status'), {'ok': True, 'displays': 'off', 'idle_seconds': 640.0,
                                                   'off_after_seconds': 600.0})
        self.assertEqual(desktop.power('on'), {'ok': True, 'power': 'on'})
        with self.assertRaisesRegex(RuntimeError, 'already off'):
            desktop.power('off')

    def test_a_service_that_isnt_running_or_doesnt_answer(self):
        with mock.patch.object(desktop, 'POWERD', 'tb-test-nobody-' + uuid.uuid4().hex):
            with self.assertRaisesRegex(RuntimeError, "isn't listening"):
                desktop.power('status')
        self.serve('POWERD', {})
        link = desktop.Datagrams(timeout=0.2)
        self.addCleanup(link.close)
        with self.assertRaisesRegex(RuntimeError, 'did not answer'):
            link.ask(desktop.POWERD, 'status')


class CliTests(unittest.TestCase):
    def run_cli(self, *argv):
        return cli.dispatch(cli.parser().parse_args(list(argv)), cli.Bench())

    def test_commands_map_to_the_sockets(self):
        cases = [
            (['keyboard', 'keys', 'ctrl+l', 'enter'], 'press_keys', (['ctrl+l', 'enter'],)),
            (['keyboard', 'type', '--', '-a b'], 'type_text', ('-a b',)),
            (['mouse', 'move', '-100', '20'], 'mouse_move', (-100, 20, None)),
            (['mouse', 'move', '5', '0', '--drag', 'left'], 'mouse_move', (5, 0, 'left')),
            (['mouse', 'click'], 'mouse_click', ('left', 1)),
            (['mouse', 'click', 'right', '--count', '2'], 'mouse_click', ('right', 2)),
            (['mouse', 'scroll', '-2', '--horizontal'], 'mouse_scroll', (-2, True)),
            (['pointer', 'state'], 'pointer_state', ()),
            (['pointer', 'overlays'], 'overlays', ()),
            (['pointer', 'measure', 'a.b'], 'measure', ('a.b',)),
            (['pointer', 'recenter'], 'pointer', ('recenter',)),
            (['power', 'off'], 'power', ('off',)),
        ]
        for argv, function, args in cases:
            with self.subTest(argv=argv), mock.patch.object(desktop, function, return_value={'ok': True}) as called:
                self.assertEqual(self.run_cli(*argv), {'ok': True})
                called.assert_called_once_with(*args)
        for bad in (['keyboard', 'keys', 'ctrl+nope'], ['mouse', 'click', 'thumb'], ['mouse', 'click', '--count', '4'],
                    ['pointer', 'reload'], ['power', 'toggle'], ['mouse', 'move', '1.5', '0']):
            with self.subTest(bad=bad), self.assertRaises(SystemExit):
                with mock.patch('sys.stderr'):
                    cli.parser().parse_args(bad)


if __name__ == '__main__':
    unittest.main()
