#!/usr/bin/python3
"""recorder/window.py's _stay_awake: the awake unit runs while any export or upload does, even
when its workers run out of order (2026-10-17: a queued export's end and its upload's start come
together, and an export's stop could land after the upload's start). Needs PySide6 (the
standalone recorder's venv, or the dev container).

  python3 recorder/tests/test_stay_awake.py
"""
import os
import sys
import tempfile
import types
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", ".."))
try:
    from recorder import window
except ImportError:   # no PySide6
    window = None


@unittest.skipIf(window is None, "no PySide6")
class StayAwakeTest(unittest.TestCase):
    def setUp(self):
        class Backend(window.RecorderBackend):
            AWAKE_UNIT = "ft-test-awake"
        self.base = tempfile.mkdtemp(prefix="recorder-awake-test-")
        self.backend = Backend(types.SimpleNamespace(base=self.base), types.SimpleNamespace())
        self.workers = []   # held back, to run in any order
        self.backend._thread = self.workers.append
        self.calls = []
        for name in ("start_unit", "stop_unit"):
            p = mock.patch.object(window.units, name, side_effect=lambda *a, n=name, **k: self.calls.append(n))
            p.start()
            self.addCleanup(p.stop)

    def tearDown(self):
        os.rmdir(self.base)

    def run_workers(self, order):
        for i in order:
            self.workers[i]()
        self.workers.clear()

    def test_on_then_off(self):
        self.backend._stay_awake("export", True)
        self.run_workers([0])
        self.backend._stay_awake("export", False)
        self.run_workers([0])
        self.assertEqual(self.calls, ["start_unit", "stop_unit"])

    def test_export_ends_as_its_upload_starts(self):
        self.backend._stay_awake("export", True)
        self.run_workers([0])
        self.backend._stay_awake("upload", True)
        self.backend._stay_awake("export", False)
        self.run_workers([1, 0])   # the export's stop runs first
        self.assertEqual(self.calls, ["start_unit"])   # never stopped: the upload holds it

    def test_out_of_order_off(self):
        self.backend._stay_awake("export", True)
        self.backend._stay_awake("export", False)
        self.run_workers([1, 0])   # the stop's worker runs before the start's
        self.assertEqual(self.calls, [])   # nothing to keep awake by then: never started


if __name__ == "__main__":
    unittest.main()
