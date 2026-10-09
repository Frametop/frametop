#!/usr/bin/env python3
"""Tests for hands/handsflag.py (`ft-handsctl flag`): the request ft-hands reads, the folder, the
flash and head poses asked of a made-up ft-handview, and a request nobody takes. Nothing here talks
to the real ft-hands or ft-handview, or opens /dev/video99.

  python3 hands/tests/test_handsflag.py
"""
import json
import os
import shutil
import socket
import sys
import tempfile
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import handsflag as hf  # noqa: E402


class FakeView(threading.Thread):
    """ft-handview's socket under another name: answers flash and poses."""

    def __init__(self, name):
        super().__init__(daemon=True)
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.sock.bind("\0" + name)
        self.sock.settimeout(0.2)
        self.got, self.stop = [], False

    def run(self):
        while not self.stop:
            try:
                data, addr = self.sock.recvfrom(256)
            except socket.timeout:
                continue
            cmd = data.decode()
            self.got.append(cmd)
            if cmd == "flash":
                reply = "ok 2"
            elif cmd.startswith("poses"):
                m = " ".join("%.5f" % v for v in (1, 0, 0, 0.1, 0, 1, 0, 1.6, 0, 0, 1, -0.2))
                reply = "ok" + "".join("\n%d %s" % (1000 + 33 * i, m) for i in range(3))
            else:
                reply = "error"
            self.sock.sendto(reply.encode(), addr)


class FakeHands(threading.Thread):
    """ft-hands' side: takes run_dir/flag and keeps what it said."""

    def __init__(self, run_dir):
        super().__init__(daemon=True)
        self.path, self.seen, self.stop = os.path.join(run_dir, "flag"), None, False

    def run(self):
        while not self.stop and self.seen is None:
            if os.path.exists(self.path):
                with open(self.path) as f:
                    self.seen = dict(line.rstrip("\n").split("=", 1) for line in f if "=" in line)
                os.unlink(self.path)
            time.sleep(0.05)


class FlagTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="handsflag-test-")
        self.run_dir = os.path.join(self.tmp, "run")
        self.env = {k: os.environ.get(k) for k in ("FT_HANDS_FLAG_DIR", "FT_HANDS_FLAG_VIEW", "FT_HANDS_FLAG_TONE",
                                                   "FT_HANDS_FLAG_FLASH", "FRAME_VOICE_WAKE_UNIX")}
        os.environ.update(FT_HANDS_FLAG_DIR=os.path.join(self.tmp, "flags"), FT_HANDS_FLAG_VIEW="0", FT_HANDS_FLAG_TONE="0",
                          FT_HANDS_FLAG_FLASH="1")
        self.view_name = "ft_handview_test_%d" % os.getpid()

    def tearDown(self):
        for k, v in self.env.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_flag_taken(self):
        view, hands = FakeView(self.view_name), FakeHands(self.run_dir)
        view.start(), hands.start()
        os.environ["FRAME_VOICE_WAKE_UNIX"] = "1760000000.25"
        try:
            rc = hf.main(["fingers", "crossed,\nnext", "line"], run_dir=self.run_dir, sock_name=self.view_name, detach=False)
        finally:
            view.stop = hands.stop = True
            view.join(1)
            view.sock.close()
        self.assertEqual(rc, 0)
        self.assertIsNotNone(hands.seen)
        path = hands.seen["dir"]
        self.assertTrue(os.path.isdir(path) and os.path.basename(path).startswith("flag-"))
        self.assertEqual(os.stat(path).st_mode & 0o777, 0o700)
        self.assertEqual(hands.seen["note"], "fingers crossed, next line")   # one line
        self.assertEqual(float(hands.seen["said_unix"]), 1760000000.25)
        self.assertIn("flash", view.got)
        self.assertTrue(any(c.startswith("poses ") for c in view.got))
        with open(os.path.join(path, "hmd.jsonl")) as f:
            poses = [json.loads(line) for line in f]
        self.assertEqual(len(poses), 3)
        self.assertEqual(len(poses[0]["m"]), 12)
        with open(os.path.join(path, "ctl.json")) as f:
            ctl = json.load(f)
        self.assertTrue(ctl["taken"])
        self.assertEqual(ctl["flash"], "ok 2")
        self.assertEqual(ctl["hmd_poses"], 3)
        self.assertNotIn("view", ctl)   # HANDS_FLAG_VIEW=0
        self.assertFalse(os.path.exists(os.path.join(self.run_dir, "flag")))

    def test_not_taken(self):
        start = time.monotonic()
        rc = hf.main([], run_dir=self.run_dir, sock_name=self.view_name, detach=False)
        self.assertEqual(rc, 1)
        self.assertLess(time.monotonic() - start, hf.TAKE_WAIT_S + hf.AFTER_S + 2)
        # the request is gone, so a later ft-hands doesn't take a stale one
        self.assertFalse(os.path.exists(os.path.join(self.run_dir, "flag")))
        with open(os.path.join(os.listdir(os.environ["FT_HANDS_FLAG_DIR"]) and os.path.join(
                os.environ["FT_HANDS_FLAG_DIR"], os.listdir(os.environ["FT_HANDS_FLAG_DIR"])[0]), "ctl.json")) as f:
            ctl = json.load(f)
        self.assertFalse(ctl["taken"])
        self.assertNotIn("flash", ctl)
        self.assertEqual(ctl["hmd_poses"], 0)   # no ft-handview answering

    def test_folders_dont_collide(self):
        base = os.path.join(self.tmp, "b")
        a, b = hf.flag_dir(base, 1760000000), hf.flag_dir(base, 1760000000)
        self.assertNotEqual(a, b)
        self.assertTrue(b.endswith("-2"))

    def test_poses_lines(self):
        self.assertEqual(hf.poses_lines(None), [])
        self.assertEqual(hf.poses_lines("error x"), [])
        lines = hf.poses_lines("ok\n5 " + " ".join(["0.5"] * 12) + "\nbad line")
        self.assertEqual(len(lines), 1)
        self.assertEqual(json.loads(lines[0])["t_ns"], 5)


if __name__ == "__main__":
    unittest.main()
