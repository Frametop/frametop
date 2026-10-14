#!/usr/bin/env python3
"""The eye recorder's frame writer (gaze/rec/eyerec.py) against a made-up share, laid out as
ft-eyegrab's (gaze/tracker/eyes_share.py). No eye images: each frame is one byte value
repeated, from its camera and number, so the round trip can be checked.

  python3 gaze/rec/tests/test_eyerec.py
"""
import mmap
import os
import shutil
import struct
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import eyerec  # noqa: E402

W, H, SLOTS = eyerec.W, eyerec.H, 8
ESIZE = 64 + W * H


def frame_bytes(cam, n):
    return bytes([(cam * 128 + n) % 256]) * (W * H)


class FakeShare:
    """Writes frames into a share file at fps per camera, on a thread, as ft-eyegrab --share does."""

    def __init__(self, path, fps=90.0):
        self.path, self.fps = path, fps
        self.count = [0, 0]
        self._stop = threading.Event()
        self._make()
        self._thread = threading.Thread(target=self._run, daemon=True)

    def _make(self):
        size = 64 + 2 * SLOTS * ESIZE
        tmp = self.path + ".new"
        fd = os.open(tmp, os.O_RDWR | os.O_CREAT | os.O_TRUNC, 0o600)
        os.ftruncate(fd, size)
        self.mm = mmap.mmap(fd, size)
        os.close(fd)
        struct.pack_into("<6I2QI", self.mm, 0, 0x31434546, 1, W, H, SLOTS, ESIZE, 0, 0, os.getpid())
        os.replace(tmp, self.path)
        self.count = [0, 0]

    def replace(self):
        """As ft-eyegrab restarting: a new file, the counts from 0."""
        self._make()

    def start(self):
        self._thread.start()
        return self

    def stop(self):
        self._stop.set()
        self._thread.join(2)

    def put(self, cam):
        n = self.count[cam]
        off = 64 + (cam * SLOTS + n % SLOTS) * ESIZE
        seq = struct.unpack_from("<Q", self.mm, off)[0]
        struct.pack_into("<Q", self.mm, off, seq + 1)
        self.mm[off + 64:off + 64 + W * H] = frame_bytes(cam, n)
        struct.pack_into("<dQII", self.mm, off + 8, time.clock_gettime(time.CLOCK_MONOTONIC_RAW), n, cam, n % 4 + 4 * cam)
        struct.pack_into("<Q", self.mm, off, seq + 2)
        self.count[cam] = n + 1
        struct.pack_into("<Q", self.mm, 24 + 8 * cam, n + 1)

    def _run(self):
        period = 1.0 / self.fps
        nxt = time.monotonic()
        while not self._stop.is_set():
            for c in (0, 1):
                self.put(c)
            nxt += period
            time.sleep(max(0.0, nxt - time.monotonic()))


class WriterTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="eyerec-test-")
        self.share_path = os.path.join(self.tmp, "cams")
        self.want = os.path.join(self.tmp, "want")
        self.take = os.path.join(self.tmp, "take")
        self.lines = []

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def writer(self, **kw):
        kw.setdefault("min_free", 0)
        return eyerec.Writer(self.take, share_path=self.share_path, want_path=self.want, log=self.lines.append, **kw)

    def wait_frames(self, w, n, timeout=10):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            with w._lock:
                if all(len(w._times[c]) and w._last[c] is not None for c in (0, 1)) and \
                        sum(len(w._open[c]) for c in (0, 1)) + sum(w._frames.values()) >= n:
                    return
            time.sleep(0.02)
        self.fail("not enough frames")

    def test_round_trip_with_a_pause(self):
        share = FakeShare(self.share_path).start()
        try:
            w = self.writer(chunk_frames=20).start()
            time.sleep(0.2)                 # frames before recording aren't kept
            w.record(True)
            time.sleep(0.6)
            w.record(False)
            time.sleep(0.3)                 # nor while paused
            w.record(True)
            time.sleep(0.4)
            stats = w.stop()
        finally:
            share.stop()
        self.assertIsNone(stats["error"])
        self.assertEqual(stats["dropped"], [0, 0])
        self.assertGreater(min(stats["frames"]), 50)
        self.assertGreater(min(stats["fps"]), 60)
        rows = eyerec.read_index(self.take)
        self.assertEqual(len(rows), sum(stats["frames"]))
        got = list(eyerec.iter_frames(self.take))
        for c in (0, 1):
            ts = [t for cam, t, _s, _d in got if cam == c]
            gaps = [b - a for a, b in zip(ts, ts[1:])]
            self.assertGreater(max(gaps), 0.25, "the pause leaves a gap")
            self.assertEqual(sorted(ts), ts)
            # Each frame is what the share had: a byte value from its camera and number, so
            # consecutive frames of a camera go up by one (mod 256) outside the gap.
            vals = [d[0] for cam, _t, _s, d in got if cam == c]
            steps = {(b - a) % 256 for a, b, g in zip(vals, vals[1:], gaps) if g < 0.05}
            self.assertEqual(steps, {1})
            self.assertTrue(all(d == bytes([d[0]]) * len(d) for cam, _t, _s, d in got if cam == c))
        # The pause ended the chunks: none spans it.
        for c in (0, 1):
            chunks = {}
            for cam, chunk, i, _slot, t in rows:
                if cam == c:
                    chunks.setdefault(chunk, []).append(t)
            self.assertTrue(all(max(ts) - min(ts) < 0.25 for ts in chunks.values()))
        self.assertTrue(os.path.exists(self.want), "ft-eyegrab's want file is touched")
        # It compresses: a repeated byte is tiny.
        self.assertLess(stats["bytes"], sum(stats["frames"]) * W * H / 100)

    def test_a_slow_writer_drops_chunks_not_frames_in_the_share(self):
        share = FakeShare(self.share_path).start()
        real = eyerec.compress

        def slow(b):
            time.sleep(0.3)
            return real(b)
        try:
            with mock.patch.object(eyerec, "compress", slow), mock.patch.object(eyerec, "MAX_QUEUED", 1):
                w = self.writer(chunk_frames=5).start()
                w.record(True)
                time.sleep(1.0)
                stats = w.stop()
        finally:
            share.stop()
        self.assertGreater(sum(stats["dropped"]), 0)
        self.assertTrue(any("behind" in line for line in self.lines))
        rows = eyerec.read_index(self.take)
        self.assertEqual(len(rows), sum(stats["frames"]))
        for c, chunk, _i, _s, _t in rows:   # every chunk the index names is there
            self.assertTrue(os.path.exists(os.path.join(self.take, "cam%d" % c, "%06d.zst" % chunk)))

    def test_follows_a_replaced_share(self):
        share = FakeShare(self.share_path).start()
        try:
            w = self.writer(chunk_frames=10).start()
            w.record(True)
            time.sleep(0.4)
            share.replace()                 # ft-eyegrab restarted with SteamVR
            time.sleep(2.8)                 # the reader looks every 2 s
            before = w.stats()["frames"]
            time.sleep(0.5)
            stats = w.stop()
        finally:
            share.stop()
        self.assertIsNone(stats["error"])
        self.assertTrue(any("replaced" in line for line in self.lines))
        self.assertGreater(min(stats["frames"]), min(before))

    def test_no_share(self):
        with mock.patch.object(eyerec, "SHARE_WAIT", 0.2):
            w = self.writer().start()
            w.record(True)
            time.sleep(0.6)
            stats = w.stop()
        self.assertIn("frametop-eyegrab", stats["error"])
        self.assertFalse(w.recording)

    def test_stops_before_the_disk_fills(self):
        share = FakeShare(self.share_path).start()
        try:
            w = self.writer(min_free=1e18).start()
            w.record(True)
            time.sleep(2.5)
            self.assertFalse(w.recording)
            stats = w.stop()
        finally:
            share.stop()
        self.assertIn("free on the disk", stats["error"])
        w.record(True)
        self.assertFalse(w.recording, "it doesn't start again after an error")

    def test_numbering_goes_on_in_a_take_already_there(self):
        share = FakeShare(self.share_path).start()
        try:
            for _ in range(2):
                w = self.writer(chunk_frames=10).start()
                w.record(True)
                time.sleep(0.3)
                w.stop()
        finally:
            share.stop()
        rows = eyerec.read_index(self.take)
        chunks = [chunk for c, chunk, _i, _s, _t in rows if c == 0]
        self.assertEqual(len(set(chunks)), len(os.listdir(os.path.join(self.take, "cam0"))))
        with open(os.path.join(self.take, "index.tsv")) as f:
            self.assertEqual(sum(1 for line in f if line.startswith("#")), 1)


@unittest.skipUnless(shutil.which("zstd") or os.path.exists("/usr/bin/zstd"), "no zstd command")
class CommandFallbackTest(unittest.TestCase):
    """The zstd command, as on the SteamOS host's Python 3.12."""

    def test_round_trip(self):
        real_import = __import__

        def no_modules(name, *a, **kw):
            if name in ("compression", "zstandard"):
                raise ImportError(name)
            return real_import(name, *a, **kw)
        with mock.patch("builtins.__import__", no_modules):
            c, d, name = eyerec._compressor()
        self.assertTrue(name.endswith("zstd"))
        data = frame_bytes(0, 3) + frame_bytes(1, 4)
        self.assertEqual(d(c(data)), data)
        if eyerec.decompress is not None:   # the module's and the command's are the same format
            self.assertEqual(eyerec.decompress(c(data)), data)


if __name__ == "__main__":
    unittest.main()
