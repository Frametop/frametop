"""hosted.py, offline: the launch, handshake, lease and framing, against a stand-in keyboard
program (a Python script here that speaks protocol 1 on the descriptor it's given)."""
import json
import os
import select
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "host"))
import hosted  # noqa: E402

# The stand-in: says ready (with PROTOCOLS), writes each request it reads to LOG, answers
# "fail" ops with ok:false, sends an event split over two writes, and exits 0 when the
# connection closes.
STAND_IN = r'''
import json, os, socket, sys, time
args = sys.argv[1:]
fd = int(args[args.index("--hosted") + 1])
sock = socket.socket(fileno=fd)
log = open(os.environ["LOG"], "a")
log.write(json.dumps({"argv": args}) + "\n"); log.flush()
sock.sendall(json.dumps({"event": "ready", "protocol": json.loads(os.environ["PROTOCOLS"]),
                         "version": "9.9.9", "session": 1}).encode() + b"\n")
buf = b""
def recv():
    try:
        return sock.recv(65536)
    except ConnectionResetError:   # closed with our answers unread: closed all the same
        return b""
def sendall(data):
    try:
        sock.sendall(data)
    except BrokenPipeError:
        sys.exit(0)
while True:
    data = recv()
    if not data:
        sys.exit(0)
    buf += data
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        msg = json.loads(line)
        log.write(json.dumps(msg) + "\n"); log.flush()
        if msg["op"] == "fail":
            sendall(json.dumps({"id": msg["id"], "ok": False, "error": "nope"}).encode() + b"\n")
        elif msg["op"] == "split":
            whole = json.dumps({"event": "geometry", "width": 0.6}).encode() + b"\n"
            sendall(whole[:7]); time.sleep(0.05); sendall(whole[7:])
        elif "id" in msg:
            sendall(json.dumps({"id": msg["id"], "ok": True}).encode() + b"\n")
'''


class HostedTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.log = Path(self.dir.name) / "log"
        self.program = Path(self.dir.name) / "framekeyboard"
        self.program.write_text(f"#!{sys.executable}\n{STAND_IN}")
        self.program.chmod(0o755)
        self.events = []
        self.keyboards = []

    def tearDown(self):
        for k in self.keyboards:
            k.close()
            k.wait(timeout=5)
        self.dir.cleanup()

    def start(self, protocols=(1,)):
        env = dict(os.environ, LOG=str(self.log), PROTOCOLS=json.dumps(list(protocols)))

        def popen(command, **kw):
            return subprocess.Popen(command, env=env, **kw)
        keyboard = hosted.Hosted(self.program, "/data", "/config", self.events.append, "1.2.3", popen=popen)
        self.keyboards.append(keyboard)
        return keyboard

    def pump(self, keyboard, seconds=0.5, until=None):
        end = time.monotonic() + seconds
        alive = True
        while time.monotonic() < end and alive:
            if select.select([keyboard], [], [], 0.05)[0]:
                alive = keyboard.receive()
            if until and until():
                break
        return alive

    def logged(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []

    def requests(self):
        return [m for m in self.logged() if "op" in m]

    def test_launch_and_handshake(self):
        keyboard = self.start()
        self.pump(keyboard, until=lambda: len(self.requests()) >= 2)
        argv = self.logged()[0]["argv"]
        self.assertEqual(argv[:2], ["--vr", "--hosted"])
        self.assertEqual(argv[3:], ["--input", "ei", "--start-enabled", "--data-dir", "/data", "--config-dir", "/config"])
        hello, subscribe = self.requests()[:2]
        self.assertEqual(hello, {"op": "hello", "protocol": 1, "client": "Frametop", "client_version": "1.2.3",
                                 "lease_ms": hosted.LEASE_MS, "id": 1})
        self.assertEqual(subscribe, {"op": "subscribe", "events": hosted.EVENTS, "id": 2})
        self.assertEqual(keyboard.version, "9.9.9")
        self.assertEqual([e["event"] for e in self.events], ["ready"])

    def test_requests_answers_and_split_lines(self):
        keyboard = self.start()
        self.pump(keyboard, until=lambda: keyboard.greeted)
        self.assertIsNone(keyboard.send("pointer", hand="left", phase="move", u=0.5, v=0.5))   # no id
        self.assertIsNotNone(keyboard.send("fail"))
        keyboard.send("split")
        self.pump(keyboard, until=lambda: len(self.events) >= 3)
        self.assertEqual([e.get("error") or e.get("event") for e in self.events], ["ready", "nope", "geometry"])
        self.assertEqual(self.events[1]["op"], "fail")   # a refusal says what it refused
        pointer = [m for m in self.requests() if m["op"] == "pointer"][0]
        self.assertNotIn("id", pointer)

    def test_the_lease_is_kept_with_pings(self):
        keyboard = self.start()
        self.pump(keyboard, until=lambda: keyboard.greeted)
        keyboard.tick(time.monotonic())
        keyboard.tick(time.monotonic() + hosted.PING_EVERY + 0.01)
        self.pump(keyboard, 0.3, until=lambda: any(m["op"] == "ping" for m in self.requests()))
        self.assertEqual([m["op"] for m in self.requests()].count("ping"), 1)

    def test_an_old_keyboard_is_closed(self):
        keyboard = self.start(protocols=(2,))
        alive = self.pump(keyboard, 2.0)
        self.assertFalse(alive)
        self.assertEqual(keyboard.wait(timeout=5), 0)   # its end closed: it exits
        self.assertEqual(self.requests(), [])           # and was never greeted
        self.assertIsNone(keyboard.send("show"))
        self.assertEqual((keyboard.why, keyboard.version), ("protocol", "9.9.9"))

    def test_a_line_over_the_limit_closes_it(self):
        keyboard = self.start()
        self.pump(keyboard, 0.3)   # hello's and subscribe's answers in
        keyboard.buffer = b"x" * hosted.MAX_LINE
        keyboard.send("split")   # one more piece without a newline goes over
        self.assertFalse(self.pump(keyboard, 2.0, until=lambda: keyboard.closed))
        self.assertEqual(keyboard.why, "oversize")

    def test_closing_our_end_is_not_a_reason(self):
        keyboard = self.start()
        self.pump(keyboard, until=lambda: keyboard.greeted)
        keyboard.close()
        self.assertIsNone(keyboard.why)

    def test_closing_our_end_makes_it_quit(self):
        keyboard = self.start()
        self.pump(keyboard, until=lambda: keyboard.greeted)
        keyboard.close()
        self.assertEqual(keyboard.wait(timeout=5), 0)


if __name__ == "__main__":
    unittest.main()
