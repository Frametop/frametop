#!/usr/bin/env python3
"""Dictation's notice (notice.py): what ft-voiced tells ft-screens, and how often.
Run: python3 voice/test/test_notice.py"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import notice  # noqa: E402


class Notice(unittest.TestCase):
    def setUp(self):
        self.now = 100.0
        self.sent = []
        self.n = notice.Notice(send=self.sent.append, clock=lambda: self.now)

    def test_flow(self):
        self.n.listening("hold")
        self.n.level(0.06)  # too soon after the start: nothing
        self.now += notice.NOTICE_EVERY
        self.n.level(0.06)
        self.now += 0.01
        self.n.level(0.0)  # throttled
        self.n.typing()
        self.n.level(0.06)  # no longer listening: nothing
        self.n.off()
        self.assertEqual(self.sent, ["voice listen hold 0", "voice listen hold 1.00", "voice typing", "voice off"])

    def test_level(self):
        self.assertEqual(notice.level_of(0), 0)
        self.assertEqual(notice.level_of(1), 1)
        self.assertAlmostEqual(notice.level_of(notice.FULL_RMS / 4), 0.5)
        self.assertEqual(notice.level_of(-1), 0)

    def test_no_screens(self):
        n = notice.Notice()
        notice.SCREENS = "\0ft_notice_test_nobody"
        n.listening("toggle")  # nobody listening: no error
        n.off()


if __name__ == "__main__":
    unittest.main()
