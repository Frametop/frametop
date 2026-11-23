#!/usr/bin/env python3
"""scripts/update-check.py: what it says about the KWin effects the session turns off, KWin's EIS,
the eye cameras' rate, and the tracking cameras' rate under the 60 fps hand tracking switch.

KWin 6.2 has a blur and a contrast effect, each with a factory named in kwin_wayland. KWin 6.6
(SteamOS 0.5.8) has no contrast effect of its own, which is not something to warn about; a
KWin without the blur effect is.

  python3 scripts/tests/test_update_check.py
"""
import importlib.util
import os
import tempfile
import time
import types
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


class KWinEis(unittest.TestCase):
    """The wheel's steps go through KWin's EIS plugin (input/kwin_ei.py, issue 108)."""

    @classmethod
    def setUpClass(cls):
        cls.m = load()

    def plugin(self, data):
        import tempfile
        path = Path(self.enterContext(tempfile.TemporaryDirectory())) / "eis.so"
        path.write_bytes(data)
        return str(path)

    def test_plugin_with_connect_to_eis(self):
        state, label, _ = self.m.kwin_eis(self.plugin(b"\0org.kde.KWin.EIS.RemoteDesktop\0connectToEIS\0"))
        self.assertEqual((state, label), ("ok", "KWin EIS"))

    def test_no_plugin_or_no_call_warns(self):
        for path in ("/nonexistent/eis.so", self.plugin(b"org.kde.KWin.EIS.InputCapture")):
            with self.subTest(path=path):
                state, _, detail = self.m.kwin_eis(path)
                self.assertEqual(state, "warn")
                self.assertIn("Zed", detail)


class HandsRate(unittest.TestCase):
    """The 60 fps hand tracking check: what it asks the host, and that it reports
    hands/handsrate.py's verdict, or nothing while the switch is off (the verdicts themselves:
    hands/tests/test_handsrate.py). The switch's state and the settings file are made up."""
    LINE = "[{} s]  og01a1b_4-0036_video9 {} fps (dark 0 at 0.0 stale 0 torn 0 remapped 0 idled 0, sync 0.08 ms)"

    def check(self, on, rate="", loaded="loaded", pids="", written_ago=3600):
        m, real, said, asked = load(), load().repo_module("handsrate", "hands", "handsrate.py"), [], []
        settings = tempfile.NamedTemporaryFile()
        self.addCleanup(settings.close)
        os.utime(settings.name, (time.time() - written_ago,) * 2)
        hr = types.SimpleNamespace(state=lambda: {"on": on, "note": ""}, XRSERVICE=real.XRSERVICE,
                                   XR_PATH=settings.name, bright_rate=real.bright_rate, verdict=real.verdict)
        journal = "\n".join(self.LINE.format(t, rate) for t in (60, 120, 180)) if rate else ""
        # Two XRServices, 500 s and 20 s old: the settings count from the newer one's start.
        answers = {"pgrep": (0 if pids else 1, pids), "ps": (0, "500\n 20\n"), "journalctl": (0, journal)}

        def run(*cmd, **kw):
            asked.append(cmd)
            return answers[cmd[0]]
        m.run, m.systemctl = run, lambda *args: loaded
        m.report = lambda state, label, detail="": said.append((state, label, detail))
        m.check_hands_rate(hr)
        return said, asked

    def test_off_reports_nothing_and_asks_nothing(self):
        self.assertEqual(self.check(False, "30.0", pids="41\n"), ([], []))

    def test_sixty_since_xrservice_started_is_ok(self):
        said, asked = self.check(True, "59.9", pids="41\n52\n")
        (state, label, detail), = said
        self.assertEqual((state, label), ("ok", "60 fps hand tracking"))
        self.assertIn("59.9", detail)
        self.assertEqual(asked[1], ("ps", "-o", "etimes=", "-p", "41,52"))
        since = int(asked[2][asked[2].index("--since") + 1].lstrip("@"))
        self.assertAlmostEqual(since, time.time() - 20, delta=5)

    def test_thirty_fails(self):
        self.assertEqual(self.check(True, "30.0", pids="41\n")[0][0][0], "FAIL")

    def test_written_after_xrservice_started_says_restart(self):
        (state, _, detail), = self.check(True, "30.0", pids="41\n", written_ago=5)[0]
        self.assertEqual(state, "warn")
        self.assertIn("restart SteamVR", detail)

    def test_no_xrservice_is_a_skip(self):
        self.assertEqual(self.check(True)[0][0][0], "skip")

    def test_hand_tracking_not_installed_warns(self):
        self.assertEqual(self.check(True, loaded="not-found")[0][0][0], "warn")


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
