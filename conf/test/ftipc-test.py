#!/usr/bin/env python3
"""conf/ftipc.py: answers, nobody listening, a timeout, and a late answer that must not answer
the next question (DEE-305). The servers are threads on names of this test's own."""
import os
import socket
import sys
import threading
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "conf"))
import ftipc  # noqa: E402


class Server:
    """A datagram server on @name: answer(text) -> (delay seconds, answer bytes or None)."""

    def __init__(self, name, answer):
        self.name, self.answer = name, answer
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.sock.bind("\0" + name)
        self.sock.settimeout(0.05)
        self.heard = []
        self.running = True
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        while self.running:
            try:
                data, peer = self.sock.recvfrom(1 << 16)
            except (TimeoutError, socket.timeout):
                continue
            except OSError:
                return
            text = data.decode()
            self.heard.append(text)
            delay, reply = self.answer(text)
            if reply is None:
                continue
            threading.Timer(delay, self.reply, (reply, peer)).start()

    def reply(self, data, peer):
        try:
            self.sock.sendto(data, peer)
        except OSError:   # (the asker's old socket is gone: that's the point)
            pass

    def close(self):
        self.running = False
        self.thread.join()
        self.sock.close()


class FtipcTest(unittest.TestCase):
    def setUp(self):
        self.name = f"ftipc_test_{os.getpid()}_{self._testMethodName}"
        self.servers = []

    def tearDown(self):
        for s in self.servers:
            s.close()

    def serve(self, answer):
        s = Server(self.name, answer)
        self.servers.append(s)
        return s

    def test_answer(self):
        self.serve(lambda t: (0, f"ok {t}".encode()))
        c = ftipc.Client()
        self.assertEqual(c.ask(self.name, "hello"), "ok hello")
        self.assertEqual(c.why, "")
        self.assertEqual(c.ask("\0" + self.name, "again"), "ok again")   # either spelling
        c.close()

    def test_nobody(self):
        c = ftipc.Client()
        self.assertIsNone(c.ask(self.name + "_nobody", "hello"))
        self.assertEqual(c.why, ftipc.NOBODY)
        self.assertEqual(ftipc.exchange(self.name + "_nobody", "x"), (None, ftipc.NOBODY))
        self.assertFalse(c.send(self.name + "_nobody", "x"))

    def test_late_answer_doesnt_answer_the_next_question(self):
        # "slow" is answered after 0.3 s; the client gives up after 0.1 s and asks "fast".
        self.serve(lambda t: (0.3, b"ok slow") if t == "slow" else (0, f"ok {t}".encode()))
        c = ftipc.Client(timeout=0.1)
        self.assertIsNone(c.ask(self.name, "slow"))
        self.assertEqual(c.why, ftipc.TIMEOUT)
        time.sleep(0.35)   # the late "ok slow" has been sent by now
        self.assertEqual(c.ask(self.name, "fast"), "ok fast")
        # ...and while it's still on its way
        self.assertIsNone(c.ask(self.name, "slow"))
        self.assertEqual(c.ask(self.name, "fast2", timeout=1.0), "ok fast2")
        time.sleep(0.35)
        self.assertEqual(c.ask(self.name, "fast3"), "ok fast3")
        c.close()

    def test_a_waiting_datagram_is_dropped_first(self):
        # A send() whose answer nobody read: the next question doesn't take it.
        self.serve(lambda t: (0, f"ok {t}".encode()))
        c = ftipc.Client()
        self.assertTrue(c.send(self.name, "fire"))
        time.sleep(0.1)
        self.assertEqual(c.ask(self.name, "question"), "ok question")
        c.close()

    def test_no_answer_at_all(self):
        self.serve(lambda t: (0, None))
        self.assertEqual(ftipc.exchange(self.name, "x", timeout=0.05), (None, ftipc.TIMEOUT))

    def test_large_answer_and_bad_bytes(self):
        big = b"ok " + b"x" * 150000
        self.serve(lambda t: (0, big) if t == "big" else (0, b"ok \xff"))
        self.assertEqual(ftipc.ask(self.name, "big"), big.decode())
        self.assertEqual(ftipc.ask(self.name, "bad"), "ok �")

    def test_bound_name(self):
        own = self.name + "_me"
        server = self.serve(lambda t: (0, b"ok"))
        self.assertEqual(ftipc.ask(self.name, "x", bind=own), "ok")
        self.assertEqual(server.heard, ["x"])


if __name__ == "__main__":
    unittest.main()
