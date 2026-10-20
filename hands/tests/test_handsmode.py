#!/usr/bin/env python3
"""Tests for hands/handsmode.py: HANDS_MODE and HANDS_MATTE_MARGIN in a temporary frametop.conf, the
Hands page's three input switches (methods_of, method_writes), and which units each mode starts,
stops and makes SteamVR want, with a made-up systemd (nothing here runs systemctl, getcap or pgrep).

  python3 hands/tests/test_handsmode.py
"""
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import handsmode as hm  # noqa: E402

CAMD, HANDS, VIEW = hm.TRACKING + (hm.VIEW,)
CUT_CAMD, CUT_HANDS = hm.CUTOUTS
BIN = "/home/u/frametop/hands/build/ft-camd"


class FakeHost:
    """systemd as the tests set it up: loaded units, active ones, SteamVR's wants."""

    def __init__(self, loaded=(CAMD, HANDS), active=(), steamvr=True, desktop=True, caps=True, built=True,
                 wanted=()):
        self.loaded, self.active, self.wanted_units = set(loaded), set(active), set(wanted)
        self.steamvr, self.desktop, self.has_caps, self.built = steamvr, desktop, caps, built
        self.calls = []

    def units(self, names):
        out = {}
        for u in names:
            on = u in self.active or (u == hm.STEAMVR and self.steamvr)
            out[u] = {"load": "loaded" if u in self.loaded or u == hm.STEAMVR or (u in hm.CUTOUTS and on)
                      else "not-found", "active": "active" if on else "inactive"}
        return out

    def exec_path(self, unit):
        return BIN if unit == CAMD and CAMD in self.loaded else ""

    def wanted(self, unit):
        return unit in self.wanted_units

    def exists(self, path):
        return self.built

    def caps(self, path):
        return self.has_caps

    def desktop_up(self):
        return self.desktop

    def cameras(self):
        return "ok"

    def systemctl(self, *args):
        self.calls.append(list(args))


def plan(mode, host, desktop=None, strict=False):
    return hm.plan(mode, hm.status(host, {"HANDS_MODE": mode}, cameras=False), desktop, strict)


class ConfTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="handsmode-test-")
        self.conf = os.path.join(self.dir, "frametop.conf")

    def tearDown(self):
        shutil.rmtree(self.dir)

    def test_mode_default_and_bad_values(self):
        self.assertEqual(hm.mode_of({}), "off")
        self.assertEqual(hm.mode_of({"HANDS_MODE": "sometimes"}), "off")
        for m in hm.MODES:
            self.assertEqual(hm.mode_of({"HANDS_MODE": m}), m)

    def test_margin(self):
        self.assertEqual(hm.margin_of({}), hm.MARGIN_DEFAULT)
        self.assertEqual(hm.margin_of({"HANDS_MATTE_MARGIN": "0.75"}), 0.75)
        self.assertEqual(hm.margin_of({"HANDS_MATTE_MARGIN": "big"}), hm.MARGIN_DEFAULT)
        self.assertEqual(hm.margin_of({"HANDS_MATTE_MARGIN": "12"}), hm.MARGIN_DEFAULT)  # ft-screens ignores it too

    def test_write_keeps_comment_and_others(self):
        with open(self.conf, "w") as f:
            f.write("SCREENS=2\nHANDS_MODE=off        # when hand tracking runs\n# HANDS_MODE=always\n")
        hm.write_value("HANDS_MODE", "frametop", self.conf)
        with open(self.conf) as f:
            text = f.read()
        self.assertIn("HANDS_MODE=frametop   # when hand tracking runs\n", text)
        self.assertIn("SCREENS=2\n", text)
        self.assertIn("# HANDS_MODE=always\n", text)  # a commented line isn't the setting
        self.assertEqual(hm.read_conf(self.conf), {"SCREENS": "2", "HANDS_MODE": "frametop"})

    def test_write_adds_missing_key_and_file(self):
        hm.write_value("HANDS_MODE", "always", self.conf)
        hm.write_value("HANDS_MATTE_MARGIN", "0.75", self.conf)
        conf = hm.read_conf(self.conf)
        self.assertEqual((hm.mode_of(conf), hm.margin_of(conf)), ("always", 0.75))

    def test_quoted_values(self):
        with open(self.conf, "w") as f:
            f.write('HANDS_MODE="always"\n')
        self.assertEqual(hm.mode_of(hm.read_conf(self.conf)), "always")


class PlanTest(unittest.TestCase):
    def test_always_wants_and_starts(self):
        h = FakeHost(loaded=(CAMD, HANDS, VIEW))
        self.assertEqual(plan("always", h), [["add-wants", hm.STEAMVR, CAMD, HANDS, VIEW],
                                             ["start", "--no-block", CAMD, HANDS, VIEW]])

    def test_always_without_steamvr_only_wants(self):
        h = FakeHost(steamvr=False, wanted=(CAMD,))
        self.assertEqual(plan("always", h, strict=True), [["add-wants", hm.STEAMVR, HANDS]])

    def test_hand_view_not_installed_is_left_out(self):
        h = FakeHost(loaded=(CAMD, HANDS))
        self.assertFalse(any(VIEW in call for call in plan("always", h)))

    def test_frametop_with_desktop(self):
        h = FakeHost(wanted=(CAMD, HANDS))
        self.assertEqual(plan("frametop", h), [["disable", CAMD, HANDS], ["start", "--no-block", CAMD, HANDS]])

    def test_frametop_without_desktop(self):
        h = FakeHost(desktop=False, active=(CAMD, HANDS))
        self.assertEqual(plan("frametop", h), [])  # an install or caps run leaves ft-handsctl's alone
        self.assertEqual(plan("frametop", h, strict=True), [["stop", "--no-block", CAMD, HANDS]])

    def test_desktop_hooks(self):
        h = FakeHost(desktop=False)  # the hook's word wins over pgrep (ft-screens still starting)
        self.assertEqual(plan("frametop", h, desktop=True), [["start", "--no-block", CAMD, HANDS]])
        h = FakeHost(desktop=True, active=(CAMD, HANDS))  # ft-screens still ending
        self.assertEqual(plan("frametop", h, desktop=False), [["stop", "--no-block", CAMD, HANDS]])

    def test_off_stops_everything_when_chosen(self):
        h = FakeHost(loaded=(CAMD, HANDS, VIEW), active=(CAMD, HANDS, VIEW, CUT_CAMD), wanted=(CAMD, HANDS, VIEW))
        self.assertEqual(plan("off", h, strict=True), [["disable", CAMD, HANDS, VIEW],
                                                       ["stop", "--no-block", CAMD, HANDS, VIEW, CUT_CAMD]])
        self.assertEqual(plan("off", h), [["disable", CAMD, HANDS, VIEW]])

    def test_off_with_nothing_running(self):
        self.assertEqual(plan("off", FakeHost(), strict=True), [])

    def test_no_caps_starts_nothing(self):
        h = FakeHost(caps=False)
        self.assertEqual(plan("always", h), [["add-wants", hm.STEAMVR, CAMD, HANDS]])
        self.assertEqual(plan("frametop", h), [])

    def test_not_installed(self):
        h = FakeHost(loaded=())
        self.assertEqual(plan("always", h), [])
        self.assertEqual(plan("frametop", h), [])

    def test_apply_runs_the_plan(self):
        h = FakeHost()
        calls, _ = hm.apply(h, {"HANDS_MODE": "frametop"})
        self.assertEqual(h.calls, calls)
        self.assertEqual(calls, [["start", "--no-block", CAMD, HANDS]])


class ToggleTest(unittest.TestCase):
    """hands_toggle (the relay's Meta+H) and ft-handsctl toggle: off if anything runs, else on."""

    def test_running_stops_everything(self):
        h = FakeHost(loaded=(CAMD, HANDS, VIEW), active=(CAMD, HANDS, VIEW, CUT_CAMD))
        calls, now, problem = hm.toggle(h, {"HANDS_MODE": "frametop"})
        self.assertEqual((calls, now, problem), ([["stop", "--no-block", CAMD, HANDS, VIEW, CUT_CAMD]], "off", ""))
        self.assertEqual(h.calls, calls)

    def test_stopped_starts_with_the_hand_view(self):
        h = FakeHost(loaded=(CAMD, HANDS, VIEW))
        calls, now, _ = hm.toggle(h, {"HANDS_MODE": "off"})
        self.assertEqual((calls, now), ([["start", "--no-block", CAMD, HANDS, VIEW]], "on"))
        self.assertEqual(hm.toggle(FakeHost(), {})[0], [["start", "--no-block", CAMD, HANDS]],
                         "no hand view installed: tracking alone")

    def test_half_running_counts_as_on(self):
        self.assertEqual(hm.toggle(FakeHost(active=(CAMD,)), {})[:2], ([["stop", "--no-block", CAMD]], "off"))

    def test_cant_start(self):
        for host, why in ((FakeHost(steamvr=False), "SteamVR isn't running"), (FakeHost(caps=False), "permissions"),
                          (FakeHost(loaded=()), "isn't installed")):
            calls, now, problem = hm.toggle(host, {})
            self.assertEqual((calls, now, host.calls), ([], "", []))
            self.assertIn(why, problem)

    def test_mode_is_left_alone(self):
        conf = {"HANDS_MODE": "always"}
        hm.toggle(FakeHost(active=(CAMD, HANDS)), conf)
        self.assertEqual(conf, {"HANDS_MODE": "always"})


class StatusTest(unittest.TestCase):
    def test_problems(self):
        st = hm.status(FakeHost(loaded=()), {})
        self.assertIn("hands/run.sh install", st["problem"])
        st = hm.status(FakeHost(built=False), {})
        self.assertIn("isn't built", st["problem"])
        st = hm.status(FakeHost(caps=False), {"HANDS_MODE": "always"})
        self.assertIn("/home/u/frametop/hands/run.sh caps", st["problem"])
        self.assertEqual(st["summary"], "Hand tracking can't start with SteamVR")
        self.assertEqual(hm.status(FakeHost(), {})["problem"], "")

    def test_cameras(self):
        h = FakeHost()
        h.cameras = lambda: "degraded: upper cameras and IR light off (VCINT FPGA failed to load)"
        self.assertIn("upper cameras", hm.status(h, {})["problem"])
        h.steamvr = False  # nothing to check without SteamVR
        self.assertEqual(hm.status(h, {})["problem"], "")

    def test_summary(self):
        self.assertEqual(hm.status(FakeHost(), {})["summary"], "Hand tracking is off")
        self.assertEqual(hm.status(FakeHost(active=(CAMD, HANDS)), {})["summary"],
                         "Hand tracking is running (started by hand: ft-handsctl)")
        self.assertEqual(hm.status(FakeHost(active=(CAMD, HANDS)), {"HANDS_MODE": "frametop"})["summary"],
                         "Hand tracking is running")
        self.assertEqual(hm.status(FakeHost(steamvr=False), {"HANDS_MODE": "frametop"})["summary"],
                         "Hand tracking starts with the Frametop desktop")
        self.assertEqual(hm.status(FakeHost(active=(CUT_CAMD, CUT_HANDS)), {})["summary"],
                         "The hand cutouts are running (started by hand: ft-cutouts)")


class MethodsTest(unittest.TestCase):
    """The Hands page's three switches (gaze clicks, the ray, direct touch): read from old configs as
    they always worked, and each toggled without moving the others."""

    def apply(self, conf, method, on):
        new = dict(conf, **hm.method_writes(conf, method, on))
        return new, hm.methods_of(new)

    def test_old_configs_read_as_before(self):
        self.assertEqual(hm.methods_of({}), {"gaze_clicks": False, "ray": False, "touch": False})
        on = {"gaze_clicks": True, "ray": True, "touch": True}
        self.assertEqual(hm.methods_of({"POINTER_HANDS": "1"}), on)
        self.assertEqual(hm.methods_of({"POINTER_HANDS": "1", "HANDS_NEAR": "1"}), on)  # the example config's
        self.assertEqual(hm.methods_of({"POINTER_HANDS": "1", "POINTER_HAND_RAY": "0"})["ray"], False)
        self.assertEqual(hm.methods_of({"POINTER_HANDS": "1", "HANDS_NEAR": "0"})["touch"], False)
        self.assertTrue(hm.methods_of({"HANDS_NEAR": "force"})["touch"])
        self.assertFalse(hm.methods_of({"HANDS_NEAR": "1"})["touch"], "1 without POINTER_HANDS: off, as before")
        self.assertTrue(hm.methods_of({"HANDS_NEAR": "on"})["touch"])
        self.assertFalse(hm.methods_of({"POINTER_HANDS": "yes"})["ray"], "as the helper reads it: not a number, 0")

    def test_each_switch_alone(self):
        for method in hm.METHODS:
            conf, got = self.apply({}, method, True)
            self.assertEqual(got, {m: m == method for m in hm.METHODS}, f"{method} alone: {conf}")
            conf, got = self.apply(conf, method, False)
            self.assertEqual(got, {m: False for m in hm.METHODS}, f"{method} off again: {conf}")

    def test_touch_alone_needs_no_pointer_hands(self):
        conf, got = self.apply({}, "touch", True)
        self.assertEqual(conf, {"HANDS_NEAR": "on"})
        conf, got = self.apply({"POINTER_HANDS": "1"}, "gaze_clicks", False)
        conf, got = self.apply(conf, "ray", False)
        self.assertEqual(conf["POINTER_HANDS"], "0")
        self.assertEqual(conf["HANDS_NEAR"], "on", "touch stays on without POINTER_HANDS")
        self.assertTrue(got["touch"])

    def test_turning_one_on_pins_the_others(self):
        # all off, then the ray: POINTER_GAZE_PINCH's default (1) and HANDS_NEAR's (1) would turn the
        # others on with POINTER_HANDS, so they're written off
        conf, got = self.apply({}, "ray", True)
        self.assertEqual(got, {"gaze_clicks": False, "ray": True, "touch": False})
        self.assertEqual(conf, {"POINTER_HANDS": "1", "POINTER_GAZE_PINCH": "0", "HANDS_NEAR": "0"})
        conf, got = self.apply(conf, "gaze_clicks", True)
        self.assertEqual(got, {"gaze_clicks": True, "ray": True, "touch": False})

    def test_minimal_writes(self):
        conf = {"POINTER_HANDS": "1", "HANDS_NEAR": "1"}
        self.assertEqual(hm.method_writes(conf, "ray", True), {}, "already on: nothing")
        self.assertEqual(hm.method_writes(conf, "ray", False), {"POINTER_HAND_RAY": "0"})
        self.assertEqual(hm.method_writes(conf, "touch", False), {"HANDS_NEAR": "0"})
        self.assertEqual(hm.method_writes({"HANDS_NEAR": "force"}, "touch", True), {}, "force stays force")

    def test_every_combination(self):
        start = {"POINTER_HANDS": "1", "HANDS_NEAR": "1"}
        for bits in range(8):
            want = {m: bool(bits >> i & 1) for i, m in enumerate(hm.METHODS)}
            conf = dict(start)
            for m in hm.METHODS:
                conf, _ = self.apply(conf, m, want[m])
            self.assertEqual(hm.methods_of(conf), want, f"{want}: {conf}")


class HostTest(unittest.TestCase):
    def test_units_parses_systemctl_show(self):
        h = hm.Host()
        out = ("Id=steamvr.service\nLoadState=loaded\nActiveState=active\n\n"
               "Id=frametop-camd.service\nLoadState=not-found\nActiveState=inactive\n")
        h.run = lambda cmd, timeout=20: hm.subprocess.CompletedProcess(cmd, 0, out, "")
        got = h.units((hm.STEAMVR, CAMD, VIEW))
        self.assertEqual(got[hm.STEAMVR], {"load": "loaded", "active": "active"})
        self.assertEqual(got[CAMD]["load"], "not-found")
        self.assertEqual(got[VIEW], {"load": "not-found", "active": "inactive"})  # no answer for it

    def test_exec_path(self):
        h = hm.Host()
        out = "{ path=/x/hands/build/ft-camd ; argv[]=/x/hands/build/ft-camd --status 60 ; ignore_errors=no }\n"
        h.run = lambda cmd, timeout=20: hm.subprocess.CompletedProcess(cmd, 0, out, "")
        self.assertEqual(h.exec_path(CAMD), "/x/hands/build/ft-camd")


if __name__ == "__main__":
    unittest.main()
