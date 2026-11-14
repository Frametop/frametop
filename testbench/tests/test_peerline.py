"""A fake peer's line to its ft-together (frame_testbench/peerline.py), against a stand-in on
the socket: an answer is told apart from what comes unasked, which is kept for whoever takes
it."""
import os
import socket
import threading
import time
import unittest
from unittest import mock

from frame_testbench import peerline

NAME = 'ft_peerline_test_%d' % os.getpid()


class Session:
    """Answers each question with the datagrams its script has for it, in order."""

    def __init__(self, script):
        self.script, self.asked, self.who = script, [], None
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM | socket.SOCK_CLOEXEC)
        self.sock.bind('\0' + NAME)
        self.sock.settimeout(0.05)
        self.stopped = False
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        while not self.stopped:
            try:
                data, self.who = self.sock.recvfrom(2048)
            except OSError:
                continue
            self.asked.append(data)
            for reply in self.script.get(data, []):
                self.sock.sendto(reply, self.who)

    def close(self):
        self.stopped = True
        self.thread.join()
        self.sock.close()


class LineTest(unittest.TestCase):
    def setUp(self):
        self.line = peerline.Line(NAME)
        self.addCleanup(self.line.close)

    def session(self, script):
        made = Session(script)
        self.addCleanup(made.close)
        return made

    def settled(self):
        time.sleep(0.1)   # what the stand-in sent is on the socket

    def test_an_answer_is_text_and_what_came_unasked_before_it_is_kept(self):
        session = self.session({b'inputs': [b'\x85\x01first', b'\x81pose', b'ok', b'\x85\x02late'], b'state': [b'ok {}']})
        self.assertEqual(self.line.ask('inputs'), 'ok')
        self.settled()
        self.assertEqual(self.line.heard(), [b'\x85\x01first', b'\x81pose', b'\x85\x02late'], 'the oldest first')
        self.assertEqual(self.line.heard(), [], 'and taken once')
        session.sock.sendto(b'\x85\x03unasked', session.who)
        session.sock.sendto(b'ok too late', session.who)
        self.settled()
        self.assertEqual(self.line.ask('state'), 'ok {}', "an answer that came too late for its question isn't the next one's")
        self.assertEqual(self.line.heard(), [b'\x85\x03unasked'], '...and what came unasked meanwhile is kept')
        self.assertEqual(session.asked, [b'inputs', b'state'])

    def test_a_datagram_that_has_no_answer_is_only_sent(self):
        session = self.session({})
        self.line.send(b'\x03pointer')
        self.settled()
        self.assertEqual(session.asked, [b'\x03pointer'])

    def test_no_session_and_no_answer_are_said_and_nothing_is_raised(self):
        self.assertEqual(self.line.ask('state'), 'error its ft-together gave no answer (Connection refused)')
        self.line.send(b'\x03pointer')
        self.assertEqual(self.line.heard(), [])
        self.session({b'state': [b'\x81pose']})
        with mock.patch.object(peerline, 'ANSWER_S', 0.3):
            started = time.monotonic()
            self.assertEqual(self.line.ask('state'), 'error its ft-together gave no answer (timeout)')
            self.assertTrue(0.25 <= time.monotonic() - started < 1.5)
        self.assertEqual(self.line.heard(), [b'\x81pose'], 'what came instead of an answer was unasked')

    def test_what_nobody_takes_is_dropped_the_oldest_first(self):
        session = self.session({b'go': [b'ok']})
        self.line.ask('go')
        for n in range(260):
            session.sock.sendto(b'\x85' + n.to_bytes(2, 'little'), session.who)
            if n % 8 == 7:
                self.line.ask('go')   # taken off the socket, as each question does
        self.settled()
        heard = self.line.heard()
        self.assertEqual((len(heard), heard[0], heard[-1]), (256, b'\x85\x04\x00', b'\x85\x03\x01'))

if __name__ == '__main__':
    unittest.main()
