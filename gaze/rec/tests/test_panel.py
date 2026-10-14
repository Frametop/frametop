#!/usr/bin/env python3
"""The gaze panel's rec mode (gaze/panel/ft-gazepanel.cpp, "show rec") without SteamVR (--no-vr,
a made-up 90 Hz vsync): its commands, and targets.jsonl, the ground truth of where the dot was
when. Needs the built panel (gaze/build.sh); skipped without it.

  python3 gaze/rec/tests/test_panel.py
"""
import json
import math
import os
import shutil
import socket
import subprocess
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
PANEL = os.path.join(HERE, "..", "..", "build", "ft-gazepanel")
IN_BOX = os.path.expanduser("~/.local/bin/distrobox")


def command():
    """The panel's argv: in the dev container on the Frame's host, where it was built."""
    if os.path.exists("/run/.containerenv") or not os.path.exists(IN_BOX):
        return [PANEL]
    return [IN_BOX, "enter", "dev", "--", PANEL]


@unittest.skipUnless(os.path.exists(PANEL), "the panel isn't built (gaze/build.sh)")
class RecPanelTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="gazepanel-test-")
        self.name = f"ft_gazepanel_test_{os.getpid()}"
        self.proc = subprocess.Popen(command() + ["--no-vr", "--watch-stdin", "--socket", self.name, "--dump",
                                                  os.path.join(self.tmp, "pics")],
                                     stdin=subprocess.PIPE, stderr=subprocess.PIPE)
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.sock.bind("")
        self.sock.settimeout(1.0)
        end = time.monotonic() + 20
        while True:
            try:
                if self.ask("ping").startswith("ok"):
                    break
            except OSError:
                pass
            if time.monotonic() > end:
                self.fail("the panel didn't start")
            time.sleep(0.1)

    def tearDown(self):
        self.proc.stdin.close()
        try:
            self.proc.wait(5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        self.proc.stderr.close()
        self.sock.close()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def ask(self, cmd):
        self.sock.sendto(cmd.encode(), "\0" + self.name)
        return self.sock.recv(4096).decode()

    def rows(self):
        with open(os.path.join(self.tmp, "targets.jsonl")) as f:
            return [json.loads(line) for line in f]

    def test_rec_lines_and_errors(self):
        for cmd in ("show rec", "bg 0.3", "title Nine dots", "step Round 1 of 5", "rec on", "text Look at each dot|as it comes",
                    "big 3", "action Ready? Press the side button", "note Your left eye is hard to see",
                    "keys Space: go · P: pause", "countdown 0.4", "paused on", "paused off", "countdown off"):
            self.assertEqual(self.ask(cmd), "ok", cmd)
        self.assertTrue(self.ask("path spiral 0 0 1 2").startswith("error"))
        self.assertTrue(self.ask("path circle 0 0 10 0").startswith("error"), "seconds must be positive")
        self.assertTrue(self.ask("log start /nonexistent/dir").startswith("error"))
        time.sleep(0.2)
        self.assertTrue(os.listdir(os.path.join(self.tmp, "pics")), "pictures were drawn")
        self.ask("show quick")
        self.assertTrue(self.ask("path circle 0 0 10 2").startswith("error"), "only in rec")

    def test_fixed_dot_logs_its_changes(self):
        self.ask("show rec")
        self.assertEqual(self.ask(f"log start {self.tmp}"), "ok")
        self.ask("dot 10 -5 look")
        time.sleep(0.15)
        self.ask("dot 10 -5 capture 0.5")
        time.sleep(0.15)
        self.ask("bg 0.8")
        time.sleep(0.15)
        self.ask("dot 0 0 off")
        time.sleep(0.15)
        reply = self.ask("log stop").split()
        rows = self.rows()
        self.assertEqual(int(reply[1]), len(rows))
        on = [r for r in rows if r["dot"] == "fixed"]
        self.assertEqual([(r["yaw"], r["pitch"], r["state"]) for r in on],
                         [(10, -5, "look"), (10, -5, "capture"), (10, -5, "capture")])
        self.assertEqual(on[-1]["bg"], 0.8)
        self.assertEqual(rows[-1]["dot"], "off")
        for r in rows:
            self.assertGreater(r["t"], r["set"], "the photons come after it's set")
            self.assertLess(r["t"] - r["set"], 0.03)
        # The RAW clock, as the eye frames' (this process's CLOCK_MONOTONIC_RAW).
        self.assertLess(abs(rows[-1]["set"] - time.clock_gettime(time.CLOCK_MONOTONIC_RAW)), 5)

    def test_path_every_frame(self):
        self.ask("show rec")
        self.ask(f"log start {self.tmp}")
        self.assertEqual(self.ask("path circle 2 1 10 1.0"), "ok")
        time.sleep(1.2)
        self.ask("dot 0 0 off")
        self.ask("log stop")
        rows = [r for r in self.rows() if r["dot"] == "path"]
        ts = [r["t"] for r in rows]
        self.assertGreater(len(rows), 80, "about one a frame")
        gaps = [b - a for a, b in zip(ts, ts[1:])]
        self.assertTrue(all(g > 0 for g in gaps))
        self.assertAlmostEqual(sorted(gaps)[len(gaps) // 2], 1 / 90, delta=0.002)
        self.assertEqual(len({r["frame"] for r in rows}), len(rows), "one row a frame")
        for r in rows:
            self.assertAlmostEqual(math.hypot(r["yaw"] - 2, r["pitch"] - 1), 10, delta=1e-3)
        first = rows[0]
        self.assertAlmostEqual((first["yaw"], first["pitch"])[1], 11, delta=0.5, msg="it starts at the top")
        # A lap a second: back near the top a second later, having gone left (yaw up) first.
        later = min(rows, key=lambda r: abs(r["t"] - first["t"] - 1.0))
        self.assertAlmostEqual(later["pitch"], 11, delta=0.5)
        self.assertGreater(rows[5]["yaw"], 2)

    def test_line_and_eight(self):
        self.ask("show rec")
        self.ask(f"log start {self.tmp}")
        self.ask("path line -10 0 10 0 0.5")
        time.sleep(0.8)
        self.ask("path eight 0 0 15 8 1")
        time.sleep(0.6)
        self.ask("log stop")
        rows = self.rows()
        line = [r for r in rows if r["dot"] == "path" and abs(r["pitch"]) < 1e-6 and abs(r["yaw"]) <= 10 + 1e-6]
        self.assertGreater(len(line), 60)
        self.assertGreater(max(r["yaw"] for r in line), 9)
        self.assertLess(min(r["yaw"] for r in line), -9.5)
        eight = [r for r in rows if r["t"] > line[-1]["t"]]
        self.assertTrue(all(abs(r["yaw"]) <= 15 + 1e-6 and abs(r["pitch"]) <= 8 + 1e-6 for r in eight if r["dot"] == "path"))

    def test_world_dot_and_distance(self):
        self.ask("show rec")
        self.ask(f"log start {self.tmp}")
        reply = self.ask("world 0.5 0 -1")
        self.assertTrue(reply.startswith("ok"), reply)
        self.assertEqual([round(float(v), 3) for v in reply.split()[1:]], [0.5, 0.0, -1.0])
        time.sleep(0.2)
        again = self.ask("world 0.5 0 -1 capture 0.5")
        self.assertEqual(again, reply, "the same point again: only the state changes")
        time.sleep(0.1)
        self.ask("dot 0 0 look")
        self.assertEqual(self.ask("distance 0.5"), "ok")
        time.sleep(0.1)
        self.ask("log stop")
        rows = self.rows()
        world = [r for r in rows if r["dot"] == "world"]
        self.assertGreater(len(world), 10)
        # --no-vr's head is at the origin looking down -z: half a metre right of a metre ahead.
        self.assertAlmostEqual(world[0]["yaw"], -math.degrees(math.atan2(0.5, 1)), delta=0.01)
        self.assertAlmostEqual(world[0]["dist"], math.hypot(0.5, 1), delta=0.001)
        self.assertEqual(rows[-1]["dist"], 0.5)
        self.assertEqual(rows[-1]["dot"], "fixed")


if __name__ == "__main__":
    unittest.main()
