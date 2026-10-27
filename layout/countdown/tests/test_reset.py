"""Current ft-layout reset behavior, without a headset or real user configuration."""
import copy
import importlib.util
import pathlib
import subprocess
import sys
import tempfile
import types
import unittest


def load_layout_module():
    path = pathlib.Path(__file__).resolve().parents[2] / "ft_layout.py"
    spec = importlib.util.spec_from_file_location("countdown_review_layout", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Harness:
    def __init__(self, lock_path, profile=False):
        self.m = load_layout_module()
        self.events = []
        self.head = [1.0, 2.0, 3.0, 0.0]
        self.on_sleep = None
        self.on_head = None
        self.layout = {"mode": "preset", "screens": [{}], "layouts": {}}
        if profile:
            self.layout.update(mode="custom", active="Cockpit", layouts={"Cockpit": [{}]})
        self.m.LOCK_PATH = str(lock_path)
        self.m.log = lambda *args: self.events.append(("log", " ".join(map(str, args))))
        self.now = 100.0
        self.helper_elapsed = 3.05
        self.helper_error = None
        self.helper_code = 0
        self.on_helper = None
        self.m.time = types.SimpleNamespace(
            sleep=self.sleep, time=lambda: self.now, monotonic=lambda: self.now)
        self.m.subprocess = types.SimpleNamespace(
            run=self.run_helper, TimeoutExpired=subprocess.TimeoutExpired)
        self.m.RESET_TIMER = "/test/repo/layout/countdown/build/ft-reset-timer"
        self.helper_available = True
        real_access = self.m.os.access
        self.m.os = types.SimpleNamespace(**vars(self.m.os))
        self.m.os.access = lambda path, mode: (self.helper_available if path == self.m.RESET_TIMER
                                              else real_access(path, mode))
        self.m.in_container = lambda: True
        self.m.load_layout = self.load_layout
        self.m.save_layout = lambda layout: self.events.append(("save",))
        self.m.backend = lambda: "screens"
        self.m.screens_socket = self.socket
        self.m.screens_up = lambda sock: 1
        self.m.screen_count = lambda layout=None: 1
        self.m.start_remotes = lambda sock, layout: self.events.append(("start_remotes",))
        self.m.send_visibility = lambda *args: self.events.append(("visibility",))
        self.m.send_hidden = lambda *args: self.events.append(("hidden",))
        self.m.place_remotes = lambda *args: self.events.append(("place_remotes",))
        self.m.plan = lambda *args: [{"pos": [0, 0, -2], "face": [0, 0], "roll": 0}]
        self.m.screen_entry = lambda *args: {}
        self.m.screen_metres = lambda *args: 1.8
        self.m.kwin_follow = lambda: self.events.append(("kwin_follow",))
        for method in ("use_named", "use_hidden", "open_remotes", "profile_remotes"):
            setattr(self.m, method, lambda *args, method=method: self.events.append((method,)))
        self.m.open_apps = lambda name, **kw: self.events.append(("open_apps", name))

    def load_layout(self):
        self.events.append(("load",))
        return copy.deepcopy(self.layout)

    def sleep(self, seconds):
        self.events.append(("sleep", seconds))
        self.now += seconds
        if self.on_sleep:
            self.on_sleep(seconds)

    def socket(self):
        self.events.append(("socket",))
        return types.SimpleNamespace(ask=self.ask, sock=types.SimpleNamespace(
            close=lambda: self.events.append(("socket_close",))))

    def ask(self, command):
        self.events.append(("ask", command))
        if command == "head":
            if self.on_head:
                self.on_head()
            return "ok " + " ".join(map(str, self.head))
        if command.startswith("get "):
            return "ok 0 0 -1 1 0 0 0 1 0 0 0 1 4 2 .25 none"
        return "ok"

    def run(self, *args):
        return self.m.main(["ft-layout", *args])


    def run_helper(self, command, **kwargs):
        self.events.append(("helper_start", tuple(command), kwargs))
        self.now += self.helper_elapsed
        if self.on_helper:
            self.on_helper()
        self.events.append(("helper_end", self.now))
        if self.helper_error:
            raise self.helper_error
        return types.SimpleNamespace(returncode=self.helper_code)


class ResetCountdownReview(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="countdown-review-locks-")
        self.addCleanup(self.temp.cleanup)
        self.lock = pathlib.Path(self.temp.name) / "layout.lock"

    def assert_new_head_after_wait(self, h):
        def read_head():
            self.assertGreaterEqual(h.now, 103)
            self.assertEqual(any(e[0] == "helper_end" for e in h.events), h.helper_available)
        h.on_head = read_head
        self.assertEqual(h.run("reset"), 0)
        place = next(e[1] for e in h.events if e[0] == "ask" and e[1].startswith("place "))
        self.assertEqual(list(map(float, place.split()[2:5])), [8.0, 20.0, 30.0])
        self.assertEqual(float(place.split()[5]), 90.0)
        actions = [e[0] for e in h.events]
        waited = "helper_end" if h.helper_available else "sleep"
        self.assertLess(actions.index(waited), actions.index("load"))

    def test_success_helper_has_fixed_cli_bounded_timeout_and_no_extra_wait(self):
        h = Harness(self.lock)
        h.on_helper = lambda: setattr(h, "head", [10.0, 20.0, 30.0, 90.0])
        self.assert_new_head_after_wait(h)
        helper = next(e for e in h.events if e[0] == "helper_start")
        self.assertEqual(helper[1], (h.m.RESET_TIMER, "--deadline", "106.000000000"))
        self.assertEqual(helper[2], {"timeout": 6, "check": False})
        self.assertFalse(any(e[0] == "sleep" for e in h.events))

    def test_fast_helper_success_still_waits_three_seconds_before_sampling_head(self):
        h = Harness(self.lock)
        h.helper_elapsed = .25
        h.on_sleep = lambda seconds: setattr(h, "head", [10.0, 20.0, 30.0, 90.0])
        self.assert_new_head_after_wait(h)
        self.assertEqual([e for e in h.events if e[0] == "sleep"], [("sleep", 2.75)])
        self.assertEqual(h.now, 103)

    def test_missing_helper_fallback_waits_full_three_seconds(self):
        h = Harness(self.lock)
        h.helper_available = False
        h.m.in_container = lambda: self.fail("missing helper attempted container launch")
        h.on_sleep = lambda seconds: setattr(h, "head", [10.0, 20.0, 30.0, 90.0])
        self.assert_new_head_after_wait(h)
        self.assertEqual([e for e in h.events if e[0] == "sleep"], [("sleep", 3)])
        self.assertFalse(any(e[0] == "helper_start" for e in h.events))
        self.assertTrue(any(e[0] == "log" and "unavailable" in e[1] for e in h.events))

    def test_helper_execution_error_preserves_three_second_wait(self):
        h = Harness(self.lock)
        h.helper_elapsed = 0
        h.helper_error = OSError("mock loader failure")
        h.on_sleep = lambda seconds: setattr(h, "head", [10.0, 20.0, 30.0, 90.0])
        self.assert_new_head_after_wait(h)
        self.assertEqual([e for e in h.events if e[0] == "sleep"], [("sleep", 3)])

    def test_nonzero_helper_fallback_waits_remaining_time(self):
        h = Harness(self.lock)
        h.helper_elapsed = .5
        h.helper_code = 1
        h.on_sleep = lambda seconds: setattr(h, "head", [10.0, 20.0, 30.0, 90.0])
        self.assert_new_head_after_wait(h)
        self.assertEqual([e for e in h.events if e[0] == "sleep"], [("sleep", 2.5)])
        self.assertEqual(h.now, 103)

    def test_timed_out_helper_falls_back_without_another_three_second_wait(self):
        h = Harness(self.lock)
        h.helper_elapsed = 6
        h.helper_error = subprocess.TimeoutExpired(["mock helper"], 6)
        h.on_helper = lambda: setattr(h, "head", [10.0, 20.0, 30.0, 90.0])
        self.assert_new_head_after_wait(h)
        self.assertEqual(h.now, 106)
        self.assertFalse(any(e[0] == "sleep" for e in h.events))

    def test_duplicates_suppressed_during_helper_fallback_and_head_then_lock_released(self):
        h = Harness(self.lock)
        h.helper_elapsed = .25
        h.helper_code = 1
        duplicates = []
        h.on_helper = lambda: duplicates.append(h.run("reset"))
        h.on_sleep = lambda seconds: duplicates.append(h.run("reset"))
        h.on_head = lambda: duplicates.append(h.run("reset"))
        self.assertEqual(h.run("reset"), 0)
        self.assertEqual(duplicates, [1, 1, 1])
        self.assertEqual(sum(e[0] == "helper_start" for e in h.events), 1)
        self.assertEqual(sum(e[0] == "sleep" for e in h.events), 1)
        self.assertEqual(sum(e == ("ask", "head") for e in h.events), 1)
        h.on_helper = h.on_sleep = h.on_head = None
        self.assertEqual(h.run("reset"), 0)
        self.assertEqual(sum(e[0] == "helper_start" for e in h.events), 2)

    def test_pending_reset_from_another_process_rejected_before_helper(self):
        code = "import fcntl,sys; f=open(sys.argv[1],'w'); fcntl.flock(f,fcntl.LOCK_EX); print('locked',flush=True); sys.stdin.read()"
        proc = subprocess.Popen([sys.executable, "-c", code, str(self.lock) + ".reset-delay"],
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        try:
            self.assertEqual(proc.stdout.readline().strip(), "locked")
            h = Harness(self.lock)
            self.assertEqual(h.run("reset"), 1)
            self.assertEqual([e[0] for e in h.events], ["log"])
        finally:
            proc.communicate(timeout=5)

    def test_gamescope_reset_runs_helper_before_preserved_profile_actions(self):
        h = Harness(self.lock, profile=True)
        h.m.backend = lambda: "gamescope"
        h.m.apply_gamescope = lambda *args: h.events.append(("apply_gamescope",))
        self.assertEqual(h.run("reset"), 0)
        actions = [e[0] for e in h.events]
        self.assertLess(actions.index("helper_end"), actions.index("use_named"))
        self.assertLess(actions.index("helper_end"), actions.index("open_apps"))
        self.assertIn(("open_apps", "Cockpit"), h.events)

    def test_direct_apply_and_use_remain_immediate(self):
        for command in (("apply",), ("use", "Cockpit")):
            with self.subTest(command=command):
                h = Harness(self.lock, profile=True)
                self.assertEqual(h.run(*command), 0)
                self.assertFalse(any(e[0] in ("sleep", "helper_start") for e in h.events))
                self.assertIn(("ask", "head"), h.events)
                if command[0] == "use":
                    self.assertIn(("open_apps", "Cockpit"), h.events)

    def test_gamescope_reset_preserves_direct_use_actions_after_countdown(self):
        direct = Harness(self.lock, profile=True)
        reset = Harness(self.lock, profile=True)
        for h in (direct, reset):
            h.m.backend = lambda: "gamescope"
            h.m.apply_gamescope = lambda *args, h=h: h.events.append(("apply_gamescope",))
        self.assertEqual(direct.run("use", "Cockpit"), 0)
        self.assertEqual(reset.run("reset"), 0)
        # Reset loads once to choose the profile; every following action is use.
        end = next(i for i, e in enumerate(reset.events) if e[0] == "helper_end")
        self.assertEqual(reset.events[end + 1], ("load",))
        self.assertEqual(reset.events[end + 2:], direct.events)

    def test_built_helper_runs_directly_inside_container(self):
        h = Harness(self.lock)
        self.assertEqual(h.run("reset"), 0)
        helper = next(e for e in h.events if e[0] == "helper_start")
        self.assertEqual(helper[1], (h.m.RESET_TIMER, "--deadline", "106.000000000"))

    def test_built_helper_enters_its_release_container_from_host(self):
        h = Harness(self.lock)
        h.m.in_container = lambda: False
        self.assertEqual(h.run("reset"), 0)
        helper = next(e for e in h.events if e[0] == "helper_start")
        self.assertEqual(helper[1], (str(pathlib.Path(h.m.REPO) / "scripts/in-box"),
                                     h.m.RESET_TIMER, "--deadline", "106.000000000"))

    def test_failed_built_helper_does_not_start_a_second_countdown(self):
        h = Harness(self.lock)
        h.helper_elapsed = .5
        h.helper_code = 1
        self.assertEqual(h.run("reset"), 0)
        self.assertEqual(sum(e[0] == "helper_start" for e in h.events), 1)
        self.assertEqual(h.now, 103)

    def test_failed_reset_releases_pending_reset_lock(self):
        h = Harness(self.lock)
        def no_head():
            raise RuntimeError("head pose unavailable")
        h.on_head = no_head
        self.assertEqual(h.run("reset"), 1)
        h.on_head = None
        self.assertEqual(h.run("reset"), 0)
        self.assertEqual(sum(e[0] == "helper_start" for e in h.events), 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
