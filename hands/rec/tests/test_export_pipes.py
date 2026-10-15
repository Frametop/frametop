#!/usr/bin/env python3
"""Tests for export's pipe into zstd (takes.py Store._compress) when zstd ends early or talks a
lot, and for the disk-full messages: zstd's own words instead of "Broken pipe", no hang, nothing
left behind (2026-10-15).

  python3 hands/rec/tests/test_export_pipes.py     (anywhere with zstd)
"""
import os
import shutil
import stat
import sys
import tempfile
import threading
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import takes  # noqa: E402
from recorder import age  # noqa: E402
from test_validate import make_session, SESSION  # noqa: E402

RECIPIENTS = os.path.join(os.path.dirname(HERE), "test-recipients.txt")


class Index:
    """What _compress reads: n records of size bytes."""

    def __init__(self, n, size):
        self.n, self.size = n, size

    def read_raw(self, i):
        return bytes((i + k) % 251 for k in range(256)) * (self.size // 256)


def compress(zstd, dest, index, recipients=None, timeout=60):
    """Store._compress on a thread: (result, error), or fails the test if it hangs."""
    got = {}

    def run():
        try:
            got["raw"] = takes.Store._compress(index, range(index.n), zstd, dest, lambda n: None,
                                               lambda: False, recipients)
        except BaseException as e:
            got["error"] = e
    t = threading.Thread(target=run, daemon=True)
    t.start()
    t.join(timeout)
    if t.is_alive():
        raise AssertionError("export hung on zstd's pipes")
    return got.get("raw"), got.get("error")


class PipeTest(unittest.TestCase):
    def setUp(self):
        self.zstd = takes.find_zstd()
        if not self.zstd:
            self.skipTest("no zstd")
        self.tmp = tempfile.mkdtemp(prefix="handrec-pipes-test-")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def fake_zstd(self, body):
        path = os.path.join(self.tmp, "zstd")
        with open(path, "w") as f:
            f.write("#!/bin/sh\n" + body + "\n")
        os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR)
        return path

    def test_disk_full(self):
        """zstd can't write (a full disk): its message, not "Broken pipe"."""
        if not os.path.exists("/dev/full"):
            self.skipTest("no /dev/full")
        raw, e = compress(self.zstd, "/dev/full", Index(200, 256 * 1024))
        self.assertIsInstance(e, takes.ZstdStopped)
        self.assertIn("No space left", str(e))
        self.assertTrue(takes.disk_full(e))

    def test_disk_full_encrypted(self):
        """The same for a testing session's sets, which are encrypted as zstd writes."""
        if not os.path.exists("/dev/full"):
            self.skipTest("no /dev/full")
        raw, e = compress(self.zstd, "/dev/full", Index(200, 256 * 1024), age.read_recipients(RECIPIENTS))
        self.assertIsNotNone(e)
        self.assertTrue(takes.disk_full(e), repr(e))

    def test_zstd_dies_mid_stream(self):
        """zstd goes before it has read everything: the write into its pipe breaks."""
        zstd = self.fake_zstd("echo 'error 70 : Write error : cannot write block' >&2; exit 70")
        for recipients in (None, age.read_recipients(RECIPIENTS)):
            raw, e = compress(zstd, os.path.join(self.tmp, "out.zst"), Index(64, 64 * 1024), recipients)
            self.assertIsInstance(e, takes.ZstdStopped)
            self.assertEqual(str(e), "zstd stopped: error 70 : Write error : cannot write block")
            self.assertFalse(takes.disk_full(e))

    def test_zstd_fails_after_reading(self):
        """zstd reads it all, then fails: the same message, from its exit status."""
        zstd = self.fake_zstd("cat > /dev/null; exit 1")
        raw, e = compress(zstd, os.path.join(self.tmp, "out.zst"), Index(2, 1024))
        self.assertIsInstance(e, takes.ZstdStopped)
        self.assertEqual(str(e), "zstd stopped (exit status 1)")

    def test_zstd_talks_a_lot(self):
        """More on stderr than a pipe holds, before zstd reads its input: no hang."""
        zstd = self.fake_zstd("head -c 400000 /dev/zero | tr '\\0' x >&2; cat")
        for recipients in (None, age.read_recipients(RECIPIENTS)):
            raw, e = compress(zstd, os.path.join(self.tmp, "out.zst"), Index(16, 64 * 1024), recipients)
            self.assertIsNone(e)
            self.assertEqual(raw, 16 * 64 * 1024)


class DiskFullExportTest(unittest.TestCase):
    """Store.export says what's free and what it needs, and leaves nothing behind."""

    def setUp(self):
        if not takes.find_zstd():
            self.skipTest("no zstd")
        self.tmp = tempfile.mkdtemp(prefix="handrec-diskfull-test-")
        make_session(self.tmp)
        self.store = takes.Store(self.tmp)
        self.partial = self.store.export_dir(SESSION) + ".partial"

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_too_little_space_before(self):
        with mock.patch.object(takes, "free_bytes", return_value=1):
            with self.assertRaises(takes.DiskFull) as cm:
                self.store.export(SESSION, low_priority=False)
        self.assertTrue(str(cm.exception).startswith("Not enough free space to export this session: 1 B free"))
        self.assertFalse(os.path.exists(self.partial))

    def test_free_space_unknown(self):
        """free_bytes is 0 when it can't tell: export goes ahead."""
        with mock.patch.object(takes, "free_bytes", return_value=0):
            self.assertTrue(os.path.isdir(self.store.export(SESSION, low_priority=False)))

    def test_disk_fills_up(self):
        full = takes.ZstdStopped("zstd stopped: error 70 : Write error : cannot write block : No space left on device")
        with mock.patch.object(takes.Store, "_compress", side_effect=full):
            with self.assertRaises(takes.DiskFull) as cm:
                self.store.export(SESSION, low_priority=False)
        self.assertTrue(str(cm.exception).startswith("The disk filled up while exporting: "))
        self.assertIn("Free some space and export again.", str(cm.exception))
        self.assertNotIn("old export", str(cm.exception))
        self.assertFalse(os.path.exists(self.partial))

    def test_disk_fills_up_with_an_old_export(self):
        self.store.export(SESSION, low_priority=False)
        with mock.patch.object(takes.Store, "_compress", side_effect=OSError(28, "No space left on device")):
            with self.assertRaises(takes.DiskFull) as cm:
                self.store.export(SESSION, low_priority=False)
        self.assertIn("Its old export (", str(cm.exception))
        self.assertTrue(os.path.isfile(os.path.join(self.store.export_dir(SESSION), "manifest.json")))
        self.assertFalse(os.path.exists(self.partial))

    def test_other_errors_unchanged(self):
        with mock.patch.object(takes.Store, "_compress", side_effect=takes.ZstdStopped("zstd stopped: boom")):
            with self.assertRaises(takes.ZstdStopped):
                self.store.export(SESSION, low_priority=False)
        self.assertFalse(os.path.exists(self.partial))


if __name__ == "__main__":
    unittest.main()
