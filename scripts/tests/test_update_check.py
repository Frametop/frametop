#!/usr/bin/env python3
"""scripts/update-check.py: what it says about the KWin effects the session turns off, and about
the eye cameras' rate.

KWin 6.2 has a blur and a contrast effect, each with a factory named in kwin_wayland. KWin 6.6
(SteamOS 0.5.8) has no contrast effect of its own, which is not something to warn about; a
KWin without the blur effect is.

  python3 scripts/tests/test_update_check.py
"""
import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def load():
    spec = importlib.util.spec_from_file_location("update_check_kwin", ROOT / "scripts/update-check.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


BLUR, CONTRAST = b"\0KWin::blur_factory\0", b"\0KWin::contrast_factory\0"


class KWinEffects(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.m = load()

    def test_kwin_6_2_has_both(self):
        state, label, detail = self.m.kwin_effects(b"x" + BLUR + CONTRAST)
        self.assertEqual((state, label), ("ok", "KWin effects"))
        self.assertIn("blur and contrast", detail)

    def test_kwin_6_6_without_a_contrast_effect_is_fine(self):
        # 6.6.2 still names the protocol (ContrastManagerInterface), not an effect.
        state, _, detail = self.m.kwin_effects(b"KWin::ContrastManagerInterface" + BLUR)
        self.assertEqual(state, "ok")
        self.assertIn("no contrast effect", detail)

    def test_no_blur_effect_warns(self):
        for blob, named in ((CONTRAST, "no built-in blur effect"), (b"", "no built-in blur, contrast effect")):
            with self.subTest(named=named):
                state, _, detail = self.m.kwin_effects(blob)
                self.assertEqual(state, "warn")
                self.assertIn(named, detail)


class EyeRate(unittest.TestCase):
    """The eye check: restarting helps only when SteamVR started before Frametop set it."""
    SETTING = "steamvr.gazeDepEyeToHeadEnable2"

    @classmethod
    def setUpClass(cls):
        cls.m = load()

    def verdict(self, on=True, rate=15.0, started=200.0, marked_at=100.0, ours=True):
        return self.m.eye_rate_verdict(self.SETTING, on, rate, started, marked_at, ours)

    def test_at_90_is_ok(self):
        state, detail = self.verdict(rate=90.0)
        self.assertEqual(state, "ok")
        self.assertIn("set by Frametop", detail)

    def test_set_before_steamvr_started_and_still_15_is_not_a_restart(self):
        # The 2026-11-16 report: on SteamOS 0.5 the old name was on, SteamVR started after, still 15.
        state, detail = self.verdict()
        self.assertEqual(state, "FAIL")
        self.assertNotIn("restart SteamVR", detail)
        self.assertIn("doesn't go by it", detail)
        self.assertEqual(self.verdict(ours=False)[0], "warn")

    def test_set_after_steamvr_started_says_restart(self):
        state, detail = self.verdict(started=50.0)
        self.assertEqual(state, "warn")
        self.assertIn("restart SteamVR", detail)

    def test_set_by_someone_else_says_restart(self):
        self.assertIn("restart SteamVR", self.verdict(marked_at=None)[1])

    def test_off_at_15(self):
        state, detail = self.verdict(on=False, marked_at=None)
        self.assertEqual(state, "FAIL")
        self.assertIn("gaze/eye-rate.py on", detail)

    def test_failed_start_is_not_ok(self):
        # 2026-11-17: the eye tracker's start failed, and the old check said "ok 90 fps".
        state, detail = self.m.eye_rate_verdict(self.SETTING, True, None, 200.0, 100.0, True,
                                                "Eye tracking system init failed")
        self.assertEqual(state, "FAIL")
        self.assertIn("didn't start", detail)

    def test_off_at_90_warns(self):
        state, detail = self.verdict(on=False, rate=90.0, marked_at=None)
        self.assertEqual(state, "warn")
        self.assertIn(self.SETTING + " is off", detail)


if __name__ == "__main__":
    unittest.main()
