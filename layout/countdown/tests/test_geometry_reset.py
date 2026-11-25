"""Isolated regression checks; no headset, network, or user config is accessed."""
import copy
import fcntl
import pathlib
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

from test_reset import Harness as IntegrationHarness, load_layout_module


class Harness:
    """Real planning/math with strict fake IPC and forbidden external side effects."""
    def __init__(self, lock, profile=True):
        self.m = load_layout_module()
        self.m.RESET_TIMER = "/test/repo/layout/countdown/build/ft-reset-timer"
        real_access = self.m.os.access
        self.m.os = types.SimpleNamespace(**vars(self.m.os))
        self.m.os.access = lambda path, mode: path == self.m.RESET_TIMER or real_access(path, mode)
        self.m.in_container = lambda: True
        self.m.LOCK_PATH = str(lock)
        self.events = []
        self.now = 100.0
        self.helper_elapsed = 3.05
        self.helper_error = None
        self.helper_code = 0
        self.on_helper = self.on_sleep = self.on_head = self.on_get = None
        self.head = [1.0, 2.0, 3.0, 0.0]
        self.failures = {}
        self.pins = {}
        self.present = {1, 101}
        self.concealed = ""
        self.allowed = set()
        self.moved = 2
        self.layout = copy.deepcopy(self.m.DEFAULTS)
        self.layout["screens"] = [{"pos": [9, 9, 9], "face": [0, 0], "metres": 5,
                                    "curve": .5, "hidden": True, "scale": 1.25}]
        self.layout["hosts"] = [{"name": "remote", "address": "test.invalid", "displays": [
            {"id": "remote-one", "screen": 101, "pos": [8, 8, 8], "face": [0, 0],
             "metres": 7, "hidden": True, "curve": .4}]}]
        if profile:
            self.layout.update(mode="custom", active="Cockpit")
            self.layout["layouts"] = {"Cockpit": [{"pos": [0, 0, -2], "face": [0, 0],
                                                     "roll": 5, "metres": 2.4}]}
            self.layout["profiles"] = {"Cockpit": {
                "hidden": [], "windows": [{"class": "google-chrome", "fullScreen": False}],
                "remote": {"remote-one": {"pos": [2, 1, -3], "face": [10, 20], "roll": 3}}}}
        self.m.load_layout = self.load_layout
        self.m.kwin_follow = lambda: self.events.append(("kwin_follow",))
        self.m.save_anchor = lambda eye, heading: self.events.append(("anchor", tuple(eye), heading))
        self.m.backend = lambda: "screens"
        self.m.screens_socket = self.socket
        self.m.log = lambda *args: self.events.append(("log", " ".join(map(str, args))))
        self.m.time = types.SimpleNamespace(time=lambda: self.now, monotonic=lambda: self.now,
                                           sleep=self.sleep)
        self.m.subprocess = types.SimpleNamespace(run=self.run_helper,
                                                 TimeoutExpired=subprocess.TimeoutExpired)
        for name in ("save_layout", "use_hidden", "open_apps", "open_remotes", "start_remotes",
                     "start_remote", "place_remote", "place_remotes", "send_hidden", "send_visibility",
                     "apply_scales", "nested_env", "write_launchers", "apply"):
            setattr(self.m, name, self.forbid(name))

    def forbid(self, name):
        def call(*args, **kwargs):
            raise AssertionError("Reset unexpectedly called " + name)
        return call

    def load_layout(self):
        self.events.append(("load",))
        return copy.deepcopy(self.layout)

    def sleep(self, seconds):
        self.events.append(("sleep", seconds))
        self.now += seconds
        if self.on_sleep:
            self.on_sleep(seconds)

    def run_helper(self, command, **kwargs):
        expected = [self.m.RESET_TIMER, "--deadline", f"{self.now + 6:.9f}"]
        if command != expected or kwargs != {"timeout": 6, "check": False}:
            raise AssertionError("Unexpected external process: " + repr((command, kwargs)))
        self.events.append(("helper_start",))
        self.now += self.helper_elapsed
        if self.on_helper:
            self.on_helper()
        self.events.append(("helper_end",))
        if self.helper_error:
            raise self.helper_error
        return types.SimpleNamespace(returncode=self.helper_code)

    def socket(self):
        self.events.append(("socket",))
        return types.SimpleNamespace(ask=self.ask, close=lambda: self.events.append(("socket_close",)))

    def ask(self, command):
        self.events.append(("ask", command))
        if command in self.failures:
            raise self.failures[command]
        if command.startswith("recentre"):
            if self.on_head:
                self.on_head()
            return f"ok {self.moved} " + " ".join(map(str, self.head))
        if command == "concealed":
            return "ok" + self.concealed
        if command.startswith("get "):
            n = int(command.split()[1])
            if self.on_get:
                self.on_get(n)
            if n not in self.present:
                raise RuntimeError("screen not present")
            return "ok 0 0 -1 1 0 0 0 1 0 0 0 1 4 2 .25 " + self.pins.get(n, "none")
        if command in ("vrkeyboard close", "tiles recentre") or command in self.allowed:
            return "ok"
        raise AssertionError("Reset sent something other than the one move: " + command)  # (a place, a pin)

    def run(self):
        return self.m.main(["ft-layout", "reset"])

    def moves(self):
        return [e[1] for e in self.events if e[0] == "ask" and e[1].startswith("recentre")]


class GeometryResetReview(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="geometry-review-")
        self.addCleanup(self.temp.cleanup)
        self.lock = pathlib.Path(self.temp.name) / "layout.lock"

    def test_one_move_and_the_layout_and_configuration_stay(self):
        h = Harness(self.lock)
        original = copy.deepcopy(h.layout)
        h.head = [10, 20, 30, 90]
        self.assertEqual(h.run(), 0)
        self.assertEqual(h.layout, original)
        self.assertEqual(h.moves(), ["recentre 1 0.00"])
        self.assertIn(("anchor", (10.0, 20.0, 30.0), 90.0), h.events)
        actions = [e[0] for e in h.events]
        self.assertLess(actions.index("helper_end"), actions.index("load"))
        self.assertEqual(actions[-2:], ["socket_close", "kwin_follow"])

    def test_the_lead_is_where_the_layout_has_it_round_you(self):
        h = Harness(self.lock)
        h.layout["layouts"]["Cockpit"][0]["pos"] = [-1, 0.4, -1]  # 45 degrees to the left
        self.assertEqual(h.run(), 0)
        self.assertEqual(h.moves(), ["recentre 1 45.00"])

    def test_preset_lead(self):
        h = Harness(self.lock, profile=False)
        h.layout["hosts"] = []
        self.assertEqual(h.run(), 0)
        self.assertEqual(h.moves(), ["recentre 1 0.00"])

    def test_the_main_screen_leads(self):
        h = Harness(self.lock)
        h.layout["screen_count"] = 2
        h.layout["primary"] = 2
        h.layout["screens"].append({})
        h.layout["layouts"]["Cockpit"].append({"pos": [1, 0, -1], "face": [0, 0], "roll": 0, "metres": 1})
        h.present = {1, 2, 101}
        h.m.native_count = h.m.screen_count = lambda layout=None: 2
        self.assertEqual(h.run(), 0)
        self.assertEqual(h.moves(), ["recentre 2 -45.00"])

    def test_head_is_sampled_after_countdown_and_all_panel_inspection(self):
        h = Harness(self.lock)
        h.on_helper = lambda: setattr(h, "head", [5, 6, 7, 0])
        h.on_get = lambda n: setattr(h, "head", [10, 20, 30, 90])
        self.assertEqual(h.run(), 0)
        asks = [e[1] for e in h.events if e[0] == "ask"]
        self.assertEqual(asks[:4], ["concealed", "get 1", "recentre 1 0.00", "tiles recentre"])
        self.assertIn(("anchor", (10.0, 20.0, 30.0), 90.0), h.events)

    def test_countdown_failures_and_fast_success_preserve_three_second_minimum(self):
        scenarios = [(.25, None, 0, 2.75), (0, FileNotFoundError("missing"), 0, 3),
                     (.5, None, 1, 2.5), (6, subprocess.TimeoutExpired("helper", 6), 0, None)]
        for elapsed, error, code, sleep in scenarios:
            with self.subTest(elapsed=elapsed, error=type(error).__name__, code=code):
                h = Harness(self.lock)
                h.helper_elapsed, h.helper_error, h.helper_code = elapsed, error, code
                h.on_head = lambda: self.assertGreaterEqual(h.now, 103)
                self.assertEqual(h.run(), 0)
                self.assertEqual([e[1] for e in h.events if e[0] == "sleep"], [] if sleep is None else [sleep])

    def test_duplicate_reset_suppressed_through_helper_fallback_and_the_move(self):
        h = Harness(self.lock)
        h.helper_elapsed = .25
        h.helper_code = 1
        duplicates = []
        h.on_helper = lambda: duplicates.append(h.run())
        h.on_sleep = lambda _: duplicates.append(h.run())
        h.on_head = lambda: duplicates.append(h.run())
        self.assertEqual(h.run(), 0)
        self.assertEqual(duplicates, [1, 1, 1])
        self.assertEqual(sum(e[0] == "helper_start" for e in h.events), 1)
        self.assertEqual(len(h.moves()), 1)
        h.on_helper = h.on_sleep = h.on_head = None
        self.assertEqual(h.run(), 0)
        self.assertEqual(sum(e[0] == "helper_start" for e in h.events), 2)

    def test_other_process_pending_reset_rejected_before_countdown(self):
        code = "import fcntl,sys; f=open(sys.argv[1],'w'); fcntl.flock(f,fcntl.LOCK_EX); print('locked',flush=True); sys.stdin.read()"
        proc = subprocess.Popen([sys.executable, "-c", code, str(self.lock) + ".reset-delay"],
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        try:
            self.assertEqual(proc.stdout.readline().strip(), "locked")
            h = Harness(self.lock)
            self.assertEqual(h.run(), 1)
            self.assertEqual([e[0] for e in h.events], ["log"])
        finally:
            proc.communicate(timeout=5)

    def test_layout_lock_prevents_interleaving_with_other_layout_commands(self):
        with self.lock.open("w") as held:
            fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
            h = Harness(self.lock)
            self.assertEqual(h.run(), 1)
            self.assertFalse(h.moves())
            self.assertFalse(any(e[0] == "socket" for e in h.events))
        self.assertEqual(h.run(), 0)

    def test_a_pinned_hidden_or_absent_main_screen_leads_nothing(self):
        for what, setup in (("wrist", lambda h: h.pins.update({1: "left"})), ("head", lambda h: h.pins.update({1: "head"})),
                            ("dashboard", lambda h: h.pins.update({1: "dashboard"})),
                            ("hidden", lambda h: setattr(h, "concealed", " 1")), ("absent", lambda h: h.present.remove(1))):
            with self.subTest(what):
                h = Harness(self.lock)
                setup(h)
                self.assertEqual(h.run(), 0)
                self.assertEqual(h.moves(), ["recentre 101 -33.69"])  # (the remote display's place, 2 m right, 3 m ahead)

    def test_with_none_of_the_layout_in_the_room_what_is_there_still_comes(self):
        h = Harness(self.lock)  # (desktopless: floating windows and no screen)
        h.pins = {1: "head", 101: "right"}
        self.assertEqual(h.run(), 0)
        self.assertEqual(h.moves(), ["recentre"])
        self.assertEqual(h.events[-1], ("kwin_follow",))

    def test_keep_in_view_ends_but_a_pin_the_layout_saved_is_back(self):
        rel = [1, 0, 0, 0, 0, 1, 0, .5, 0, 0, 1, -1]
        back = " head " + " ".join(f"{v:.5f}" for v in rel)
        saved = {"hand": "head", "rel": rel}
        remote = lambda h: h.layout["profiles"]["Cockpit"]["remote"]["remote-one"]
        for what, pins, setup, want in (
                ("Keep in View", {1: "head", 101: "head"}, lambda h: None, []),
                ("a screen saved on the head", {1: "head"}, lambda h: h.layout["layouts"]["Cockpit"][0].update(pin=saved), ["pin 1" + back]),
                ("...and off it now", {}, lambda h: h.layout["layouts"]["Cockpit"][0].update(pin=saved), []),
                ("...and on a wrist now", {1: "left"}, lambda h: h.layout["layouts"]["Cockpit"][0].update(pin=saved), []),
                ("saved on a wrist", {1: "head"}, lambda h: h.layout["layouts"]["Cockpit"][0].update(pin={"hand": "left", "rel": rel}), []),
                ("a remote display saved on the head", {101: "head"}, lambda h: remote(h).update(pin=saved), ["pin 101" + back]),
                ("...and disconnected", {101: "head"}, lambda h: (remote(h).update(pin=saved), h.layout["hosts"][0]["displays"][0].update(off=True)), [])):
            with self.subTest(what):
                h = Harness(self.lock)
                h.pins = pins
                setup(h)
                h.allowed = {"pin 1" + back, "pin 101" + back}
                self.assertEqual(h.run(), 0)
                asks = [e[1] for e in h.events if e[0] == "ask"]
                self.assertEqual([a for a in asks if a.startswith("pin ")], want)
                if want:  # (the move takes it off the head: pinned again after)
                    self.assertLess(asks.index(h.moves()[0]), asks.index(want[0]))

    def test_a_pc_windows_panel_leads_nothing(self):
        # (its place in the layout is where it last opened, not where you put it: desktopless)
        h = Harness(self.lock)
        h.layout["hosts"][0]["displays"][0]["window"] = True
        h.present = {101}
        self.assertEqual(h.run(), 0)
        self.assertEqual(h.moves(), ["recentre"])

    def test_screens_never_arranged_are_arranged(self):
        # (the desktop came up with the headset off)
        h = Harness(self.lock)
        h.failures["recentre 1 0.00"] = RuntimeError("error not arranged yet")
        h.m.apply_screens = lambda: h.events.append(("arranged",)) or ["ok"]
        self.assertEqual(h.run(), 0)
        self.assertIn(("arranged",), h.events)
        self.assertEqual(h.events[-1], ("kwin_follow",))

    def test_nothing_in_the_room_leaves_plasma_alone(self):
        h = Harness(self.lock)
        h.pins = {1: "dashboard", 101: "dashboard"}
        h.moved = 0
        self.assertEqual(h.run(), 0)
        self.assertIn(("ask", "tiles recentre"), h.events)  # (the tiles come round you)
        self.assertNotIn("anchor", [e[0] for e in h.events])  # (nothing is arranged from here)
        self.assertNotIn(("kwin_follow",), h.events)

    def test_moved_panels_close_the_keyboard_and_rearrange_plasma(self):
        h = Harness(self.lock)
        self.assertEqual(h.run(), 0)
        self.assertIn(("ask", "vrkeyboard close"), h.events)
        self.assertEqual(h.events[-1], ("kwin_follow",))

    def test_disconnected_and_absent_remote_panels_are_not_restarted(self):
        for off in (False, True):
            with self.subTest(off=off):
                h = Harness(self.lock)
                h.layout["hosts"][0]["displays"][0]["off"] = off
                h.present = set()
                self.assertEqual(h.run(), 0)
                self.assertEqual(h.moves(), ["recentre"])
                self.assertEqual(("ask", "get 101") in h.events, not off)

    def test_ipc_timeout_aborts_without_any_panel_motion_and_releases_locks(self):
        h = Harness(self.lock)
        h.failures["get 1"] = h.m.NoAnswer("mock compositor timeout")
        self.assertEqual(h.run(), 1)
        self.assertEqual(h.moves(), [])
        self.assertEqual(h.events[-2], ("socket_close",))
        h.failures.clear()
        self.assertEqual(h.run(), 0)

    def test_missing_head_does_not_move_panels_and_releases_socket_and_locks(self):
        h = Harness(self.lock)
        h.failures["recentre 1 0.00"] = RuntimeError("no head pose (headset off?)")
        self.assertEqual(h.run(), 1)
        self.assertNotIn(("ask", "tiles recentre"), h.events)
        self.assertFalse(any(e[0] in ("anchor", "kwin_follow") for e in h.events))
        self.assertEqual(h.events[-2], ("socket_close",))
        h.failures.clear()
        self.assertEqual(h.run(), 0)

    def test_direct_apply_and_use_still_use_full_layout_path_without_countdown(self):
        for command in (("apply",), ("use", "Cockpit")):
            with self.subTest(command=command):
                h = IntegrationHarness(self.lock, profile=True)
                h.m.recenter_screens = mock.Mock(side_effect=AssertionError("direct command used recenter"))
                self.assertEqual(h.run(*command), 0)
                self.assertIn(("ask", "head"), h.events)
                self.assertIn(("visibility",), h.events)
                self.assertFalse(any(e[0] in ("sleep", "helper_start") for e in h.events))
                if command[0] == "use":
                    self.assertIn(("open_apps", "Cockpit"), h.events)

    def test_legacy_gamescope_reset_preserves_explicit_profile_use_actions(self):
        direct = IntegrationHarness(self.lock, profile=True)
        reset = IntegrationHarness(self.lock, profile=True)
        for h in (direct, reset):
            h.m.backend = lambda: "gamescope"
            h.m.apply_gamescope = lambda *args, h=h: h.events.append(("apply_gamescope",))
        self.assertEqual(direct.run("use", "Cockpit"), 0)
        self.assertEqual(reset.run("reset"), 0)
        end = next(i for i, e in enumerate(reset.events) if e[0] == "helper_end")
        self.assertEqual(reset.events[end + 1], ("load",))
        self.assertEqual(reset.events[end + 2:], direct.events)


if __name__ == "__main__":
    unittest.main(verbosity=2)
