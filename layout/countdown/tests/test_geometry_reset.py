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
        self.on_helper = self.on_sleep = self.on_head = self.on_place = self.on_get = None
        self.head = [1.0, 2.0, 3.0, 0.0]
        self.failures = {}
        self.pins = {}
        self.present = {1, 101}
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
        self.m.backend = lambda: "screens"
        self.m.screens_socket = self.socket
        self.m.log = lambda *args: self.events.append(("log", " ".join(map(str, args))))
        self.m.time = types.SimpleNamespace(time=lambda: self.now, monotonic=lambda: self.now,
                                           sleep=self.sleep)
        self.m.subprocess = types.SimpleNamespace(run=self.run_helper,
                                                 TimeoutExpired=subprocess.TimeoutExpired)
        for name in ("save_layout", "use_hidden", "open_apps", "open_remotes", "start_remotes",
                     "start_remote", "place_remote", "place_remotes", "send_hidden", "send_visibility",
                     "kwin_follow", "apply_scales", "nested_env", "write_launchers", "apply"):
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
        return types.SimpleNamespace(ask=self.ask, sock=types.SimpleNamespace(
            close=lambda: self.events.append(("socket_close",))))

    def ask(self, command):
        self.events.append(("ask", command))
        if command in self.failures:
            raise self.failures[command]
        if command == "head":
            if self.on_head:
                self.on_head()
            return "ok " + " ".join(map(str, self.head))
        if command.startswith("get "):
            n = int(command.split()[1])
            if self.on_get:
                self.on_get(n)
            if n not in self.present:
                raise RuntimeError("screen not present")
            return "ok 0 0 -1 1 0 0 0 1 0 0 0 1 4 2 .25 " + self.pins.get(n, "none")
        if command.startswith("place "):
            if self.on_place:
                self.on_place()
            return "ok"
        raise AssertionError("Reset sent a non-geometric command: " + command)

    def run(self):
        return self.m.main(["ft-layout", "reset"])

    def places(self):
        return [e[1] for e in self.events if e[0] == "ask" and e[1].startswith("place ")]


class GeometryResetReview(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="geometry-review-")
        self.addCleanup(self.temp.cleanup)
        self.lock = pathlib.Path(self.temp.name) / "layout.lock"

    def test_profile_restores_only_screen_poses_and_preserves_configuration(self):
        h = Harness(self.lock)
        original = copy.deepcopy(h.layout)
        h.head = [10, 20, 30, 90]
        self.assertEqual(h.run(), 0)
        self.assertEqual(h.layout, original)
        self.assertEqual(h.places(), [
            "place 1 8.0000 20.0000 30.0000 90.000 0.000 5.000",
            "place 101 7.0000 21.0000 28.0000 100.000 20.000 3.000"])
        self.assertEqual(sum(e == ("ask", "head") for e in h.events), 1)
        actions = [e[0] for e in h.events]
        self.assertLess(actions.index("helper_end"), actions.index("load"))
        self.assertEqual(actions[-1], "socket_close")

    def test_preset_reset_places_screen_without_restoring_windows(self):
        h = Harness(self.lock, profile=False)
        h.layout["hosts"] = []
        h.head = [10, 20, 30, 90]
        self.assertEqual(h.run(), 0)
        self.assertEqual(h.places(), ["place 1 8.0000 20.0000 30.0000 90.000 0.000 0.000"])

    def test_head_is_sampled_after_countdown_and_all_panel_inspection(self):
        h = Harness(self.lock)
        h.on_helper = lambda: setattr(h, "head", [5, 6, 7, 0])
        h.on_get = lambda n: setattr(h, "head", [10, 20, 30, 90])
        self.assertEqual(h.run(), 0)
        asks = [e[1] for e in h.events if e[0] == "ask"]
        self.assertEqual(asks[:3], ["get 1", "get 101", "head"])
        self.assertTrue(h.places()[0].startswith("place 1 8.0000 20.0000 30.0000 90.000"))

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

    def test_duplicate_reset_suppressed_through_helper_fallback_head_and_placement(self):
        h = Harness(self.lock)
        h.helper_elapsed = .25
        h.helper_code = 1
        duplicates = []
        h.on_helper = lambda: duplicates.append(h.run())
        h.on_sleep = lambda _: duplicates.append(h.run())
        h.on_head = lambda: duplicates.append(h.run())
        h.on_place = lambda: duplicates.append(h.run())
        self.assertEqual(h.run(), 0)
        self.assertEqual(duplicates, [1, 1, 1, 1, 1])
        self.assertEqual(sum(e[0] == "helper_start" for e in h.events), 1)
        self.assertEqual(len(h.places()), 2)
        h.on_helper = h.on_sleep = h.on_head = h.on_place = None
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
            self.assertFalse(h.places())
            self.assertFalse(any(e[0] == "socket" for e in h.events))
        self.assertEqual(h.run(), 0)

    def test_pinned_local_and_remote_panels_are_left_untouched(self):
        h = Harness(self.lock)
        h.pins = {1: "left", 101: "right"}
        self.assertEqual(h.run(), 0)
        self.assertEqual(h.places(), [])
        self.assertNotIn(("ask", "head"), h.events)
        self.assertEqual(h.events[-1], ("socket_close",))

    def test_disconnected_and_absent_remote_panels_are_not_restarted(self):
        for off in (False, True):
            with self.subTest(off=off):
                h = Harness(self.lock)
                h.layout["hosts"][0]["displays"][0]["off"] = off
                h.present = {1}
                self.assertEqual(h.run(), 0)
                self.assertEqual(len(h.places()), 1)
                self.assertTrue(h.places()[0].startswith("place 1 "))
                self.assertEqual(("ask", "get 101") in h.events, not off)

    def test_ipc_timeout_aborts_without_any_panel_motion_and_releases_locks(self):
        h = Harness(self.lock)
        h.failures["get 101"] = h.m.NoAnswer("mock compositor timeout")
        self.assertEqual(h.run(), 1)
        self.assertEqual(h.places(), [])
        self.assertEqual(h.events[-2], ("socket_close",))
        h.failures.clear()
        self.assertEqual(h.run(), 0)

    def test_missing_head_does_not_move_panels_and_releases_socket_and_locks(self):
        h = Harness(self.lock)
        h.failures["head"] = RuntimeError("head pose unavailable")
        self.assertEqual(h.run(), 1)
        self.assertEqual(h.places(), [])
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
