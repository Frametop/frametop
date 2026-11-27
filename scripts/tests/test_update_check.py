#!/usr/bin/env python3
"""scripts/update-check.py: what it says about the KWin effects the session turns off, SteamVR's saved
universes, KWin's EIS, the eye cameras' rate, the palm menu's readouts, and the tracking cameras'
rate under the 60 fps hand tracking switch.

KWin 6.2 has a blur and a contrast effect, each with a factory named in kwin_wayland. KWin 6.6
(SteamOS 0.5.8) has no contrast effect of its own, which is not something to warn about; a
KWin without the blur effect is.

  python3 scripts/tests/test_update_check.py
"""
import importlib.util
import json
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


class SavedUniverses(unittest.TestCase):
    """SteamVR's saved universes, as screens/universefix.h reads them."""

    def setUp(self):
        self.m = load()
        self.said = []
        self.m.report = lambda *a: self.said.append(a)

    def check(self, data):
        path = Path(self.enterContext(tempfile.TemporaryDirectory())) / "chaperone_info.vrchap"
        path.write_text(json.dumps(data))
        self.m.CHAPERONE = str(path)
        self.m.check_chaperone()
        (state, _, detail), = self.said
        return state, detail

    def test_rooms_are_counted(self):
        room = {"universeID": "5", "standing": {"translation": [0.1, 1.2, 0.3], "yaw": 0.4}}
        lost = {"universeID": "6", "standing": {"translation": [0, 1.6, 0], "yaw": 0}}
        state, detail = self.check({"jsonid": "chaperone_info", "version": 5, "universes": [room, lost]})
        self.assertEqual(state, "ok")
        self.assertIn("2, 1 with the guessed floor", detail)

    def test_no_list_once_the_last_universe_is_gone(self):
        self.assertEqual(self.check({"jsonid": "chaperone_info", "version": 5})[0], "skip")

    def test_an_empty_list(self):
        self.assertEqual(self.check({"jsonid": "chaperone_info", "version": 5, "universes": []})[0], "skip")

    def test_the_list_under_another_name_warns(self):
        self.assertEqual(self.check({"jsonid": "chaperone_info", "version": 6, "rooms": []})[0], "warn")

    def test_an_empty_file_or_another_shape_warns(self):
        for data in ({}, [], {"version": 5}, {"jsonid": "chaperone_info", "version": 6}, {"jsonid": "chaperone_info", "universes": [{"universeID": "5"}]}):
            with self.subTest(data=data):
                self.said.clear()
                self.assertEqual(self.check(data)[0], "warn")


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


class Readouts(unittest.TestCase):
    """The palm menu's temperature and battery files, in a made-up /sys."""

    def verdict(self, skip=(), core="cpu0-thermal"):
        module = load()
        said = []
        module.report = lambda state, name, detail: said.append((state, detail))
        with tempfile.TemporaryDirectory() as root:
            files = {"power/cell/type": "Battery", "power/cell/capacity": "87", "power/cell/voltage_now": "8",
                     "power/cell/current_avg": "1", "power/charger/type": "USB", "power/charger/online": "0",
                     "power/pad/type": "Battery", "power/pad/scope": "Device", "power/pad/capacity": "5",
                     "power/pad/voltage_now": "8", "power/pad/current_avg": "1", "power/pad/online": "1",
                     "zones/thermal_zone0/type": core}
            for name, value in files.items():
                if name not in skip:
                    path = Path(root, name)
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text(value + "\n")
            module.check_readouts(root + "/power", root + "/zones")
        return said[0]

    def test_all_there_is_ok(self):
        self.assertEqual(self.verdict()[0], "ok")

    def test_each_one_missing_warns_and_a_pad_battery_does_not_stand_in(self):
        for name, word in (("capacity", "battery capacity"), ("voltage_now", "battery voltage_now"),
                           ("current_avg", "battery current_avg")):
            state, detail = self.verdict(skip=("power/cell/" + name,))
            self.assertEqual(state, "warn")
            self.assertIn(word, detail)
        self.assertIn("charger online", self.verdict(skip=("power/charger/online",))[1])

    def test_the_cluster_zone_is_not_a_core(self):
        state, detail = self.verdict(core="cpuss0-thermal")
        self.assertEqual(state, "warn")
        self.assertIn("CPU core", detail)


class GamePad(unittest.TestCase):
    """Gamepad mode's pad for a Steam game goes by Steam's virtual gamepad's ids (input/vrpad.py STEAM)."""
    m = load()
    IGNORE = {"SDL_GAMECONTROLLER_IGNORE_DEVICES": "0x28de/0x11ff,0x045e/0x028e"}

    def test_no_game_running_is_skipped(self):
        self.assertEqual(self.m.game_pad_verdict([])[0], "skip")

    def test_steams_virtual_gamepad_allowed_is_ok(self):
        envs = [{**self.IGNORE, "SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD": "1"}] * 2
        self.assertEqual(self.m.game_pad_verdict(envs)[0], "ok")

    def test_a_game_without_the_allowance_warns(self):
        envs = [{**self.IGNORE, "SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD": "1"}, dict(self.IGNORE)]
        state, detail = self.m.game_pad_verdict(envs)
        self.assertEqual(state, "warn")
        self.assertIn("28de:11ff", detail)

    def test_only_this_users_processes_with_steams_ignore_list_count(self):
        with tempfile.TemporaryDirectory(prefix="ft-proc-test-") as proc:
            for pid, env in (("10", b"A=1\0SDL_GAMECONTROLLER_IGNORE_DEVICES=0x1/0x2\0B=x=y\0"), ("11", b"A=1\0"),
                             ("self", b"SDL_GAMECONTROLLER_IGNORE_DEVICES=1\0")):
                os.mkdir(os.path.join(proc, pid))
                Path(proc, pid, "environ").write_bytes(env)
            os.mkdir(os.path.join(proc, "12"))  # gone before its environment was read
            self.assertEqual(self.m.steam_game_envs(proc),
                             [{"A": "1", "SDL_GAMECONTROLLER_IGNORE_DEVICES": "0x1/0x2", "B": "x=y"}])


class HandsRate(unittest.TestCase):
    """The 60 fps hand tracking check: what it says of the cameras (the numbers are ft-camd's,
    2026-11-22 and 23), what it asks the host, and that it says nothing with the switch off. The
    switch's state and the settings file are made up; reading the status lines is tested in
    hands/tests/test_handsrate.py."""
    LINE = "[{} s]  og01a1b_4-0036_video9 {} fps (dark {} at 3.6 stale 0 torn 0 remapped 0 idled 0, sync 0.08 ms)"

    @classmethod
    def setUpClass(cls):
        cls.m = load()

    def verdict(self, best=59.9, minutes=2, dark=0.003, hands="running", pending=False):
        return self.m.hands_rate_verdict(hands, pending, {"best": best, "minutes": minutes, "dark": dark})

    def test_sixty_and_no_dark_frames_is_ok(self):
        state, detail = self.verdict()
        self.assertEqual(state, "ok")
        self.assertIn("59.9", detail)
        self.assertIn("0.3% dark", detail)
        # The most the mode gave in a run of ft-camd (1.25%), and the least SteamVR's own did (30%).
        self.assertEqual(self.verdict(dark=0.0125)[0], "ok")
        self.assertEqual(self.verdict(dark=0.299)[0], "FAIL")

    def test_dark_frames_mean_the_mode_no_longer_holds(self):
        # SteamVR's own mode, as a SteamOS update that renumbers the modes would leave it.
        state, detail = self.verdict(best=40.4, minutes=5, dark=0.30)
        self.assertEqual(state, "FAIL")
        self.assertIn("handsrate.py off --now", detail)
        self.assertIn("slam-rate.py", detail)

    def test_the_headset_off_is_a_skip_not_a_failure(self):
        # 2026-11-23: the cameras idle at 0 to 5.1 a second with the headset off, and the first
        # check called that a broken mode.
        state, detail = self.verdict(best=5.1, minutes=0, dark=0.0)
        self.assertEqual(state, "skip")
        self.assertIn("check again", detail)
        self.assertEqual(self.verdict(best=0.0, minutes=0, dark=None)[0], "skip")

    def test_part_minutes_are_a_skip(self):
        # Short wears: handsrate.camera_rates counts no full minute in them.
        self.assertEqual(self.verdict(best=40.0, minutes=0, dark=0.0)[0], "skip")
        self.assertEqual(self.verdict(best=30.5, minutes=2, dark=0.0)[0], "skip")

    def test_minutes_at_a_lower_rate_warn(self):
        state, detail = self.verdict(best=45.0, minutes=4, dark=0.0)
        self.assertEqual(state, "warn")
        self.assertIn("45", detail)
        self.assertEqual(self.verdict(best=45.0, minutes=3, dark=0.0)[0], "warn")   # three is enough

    def test_a_faster_mode_warns(self):
        self.assertEqual(self.verdict(best=114.4)[0], "warn")

    def test_written_after_steamvr_started_says_restart(self):
        state, detail = self.verdict(best=40.4, minutes=5, dark=0.30, pending=True)
        self.assertEqual(state, "warn")
        self.assertIn("restart SteamVR", detail)

    def test_hand_tracking_missing_or_stopped(self):
        state, detail = self.verdict(hands="missing")
        self.assertEqual(state, "warn")
        self.assertIn("handsrate.py off", detail)
        state, detail = self.verdict(best=0.0, minutes=0, dark=None, hands="stopped")
        self.assertEqual(state, "skip")
        self.assertIn("isn't running", detail)

    def check(self, on, lines=(), unit="loaded\nactive", pids="", ages="500\n 20\n", written_ago=3600):
        m, real, said, asked = load(), self.m.repo_module("handsrate", "hands", "handsrate.py"), [], []
        settings = tempfile.NamedTemporaryFile()
        self.addCleanup(settings.close)
        os.utime(settings.name, (time.time() - written_ago,) * 2)
        hr = types.SimpleNamespace(state=lambda: {"on": on, "note": ""}, XRSERVICE=real.XRSERVICE,
                                   XR_PATH=settings.name, camera_rates=real.camera_rates)
        journal = "\n".join(self.LINE.format(*line) for line in lines)
        # Two XRServices, 500 s and 20 s old: the settings count from the newer one's start.
        answers = {"pgrep": (0 if pids else 1, pids), "ps": (0, ages), "journalctl": (0, journal)}

        def run(*cmd, **kw):
            asked.append(cmd)
            return answers[cmd[0]]

        def systemctl(*args):
            asked.append(("systemctl",) + args)
            return unit
        m.run, m.systemctl = run, systemctl
        m.report = lambda state, label, detail="": said.append((state, label, detail))
        m.check_hands_rate(hr)
        return said, asked

    WORN = ((60, "12.9", 0), (120, "59.9", 7), (180, "59.9", 7))

    def test_off_reports_nothing_and_asks_nothing(self):
        self.assertEqual(self.check(False, self.WORN, pids="41\n"), ([], []))

    def test_it_measures_since_the_newest_xrservice_started(self):
        said, asked = self.check(True, self.WORN, pids="41\n52\n")
        (state, label, detail), = said
        self.assertEqual((state, label), ("ok", "60 fps hand tracking"))
        self.assertIn("59.9", detail)
        self.assertEqual(asked[1], ("ps", "-o", "etimes=", "-p", "41,52"))
        journal = asked[2]
        self.assertAlmostEqual(int(journal[journal.index("--since") + 1].lstrip("@")), time.time() - 20, delta=5)
        self.assertNotIn("-n", journal)   # with -g, -n gives the lines newest first

    def test_steamvrs_own_mode_fails(self):
        own = ((9242, "2.1", 3952), (9302, "30.0", 5752), (9362, "29.6", 7535), (9422, "30.0", 9335))
        self.assertEqual(self.check(True, own, pids="41\n")[0][0][0], "FAIL")

    def test_the_headset_off_skips(self):
        idle = ((240, "5.1", 0), (300, "0.0", 0), (360, "5.1", 0), (420, "0.0", 0))
        self.assertEqual(self.check(True, idle, pids="41\n")[0][0][0], "skip")

    def test_written_after_xrservice_started_says_restart(self):
        (state, _, detail), = self.check(True, self.WORN, pids="41\n", written_ago=5)[0]
        self.assertEqual(state, "warn")
        self.assertIn("restart SteamVR", detail)

    def test_no_xrservice_is_a_skip(self):
        self.assertEqual(self.check(True)[0][0][0], "skip")
        # ...and one that went between pgrep and ps is no broken checkout
        (state, _, detail), = self.check(True, pids="41\n", ages="")[0]
        self.assertEqual(state, "skip")

    def test_hand_tracking_not_installed_or_not_running(self):
        self.assertEqual(self.check(True, unit="not-found\ninactive")[0][0][0], "warn")
        (state, _, detail), = self.check(True, pids="41\n", unit="loaded\ninactive")[0]
        self.assertEqual(state, "skip")
        self.assertIn("isn't running", detail)


class Npu(unittest.TestCase):
    """ft-npud: nothing when it isn't installed; a changed or missing DSP firmware, or a daemon that
    isn't running, says so."""
    @classmethod
    def setUpClass(cls):
        cls.m = load()

    def test_each_case(self):
        good = self.m.NPU_FIRMWARE_SHA
        self.assertIsNone(self.m.npu_verdict(False, "", None))
        self.assertEqual(self.m.npu_verdict(True, "active", good)[0], "ok")
        state, detail = self.m.npu_verdict(True, "active", "0" * 64)
        self.assertEqual(state, "warn")
        self.assertIn("firmware changed", detail)
        state, detail = self.m.npu_verdict(True, "activating", good)
        self.assertEqual(state, "warn")
        self.assertIn("uses the CPU", detail)
        self.assertNotIn("firmware", detail)
        self.assertEqual(self.m.npu_verdict(True, "failed", None)[0], "FAIL")

    def test_the_firmware_hash(self):
        with tempfile.NamedTemporaryFile() as f:
            f.write(b"abc")
            f.flush()
            self.assertEqual(self.m.file_sha256(f.name),
                             "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
        self.assertIsNone(self.m.file_sha256("/nonexistent/cdsp.mbn"))


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
