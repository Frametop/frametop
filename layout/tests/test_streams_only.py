"""Streams only (FT_STREAMS_ONLY=1, ft-layout streams): ft-screens runs without KWin and Plasma.
Arranging places the remote displays and leaves the desktop's screens alone (they never come),
and `streams stop` never stops a running desktop. With no marker file, the desktop's screens are
arranged as before."""
import importlib.util
import pathlib
import subprocess
import tempfile
import unittest


def load_layout_module(tmp):
    path = pathlib.Path(__file__).resolve().parents[1] / "ft_layout.py"
    spec = importlib.util.spec_from_file_location("streams_only_layout", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.CONF_PATH = str(tmp / "frametop.conf")  # none: the screens backend
    module.LAYOUT_PATH = str(tmp / "layout.json")
    module.ANCHOR_PATH = str(tmp / "anchor.json")
    module.STREAMS_ONLY_PATH = str(tmp / "frametop-streams-only")
    return module


class FakeScreens:
    """ft-screens with `up` of the desktop's screens shown and a head pose; what it's asked is
    recorded. A screen it has no panel for answers with an error, as ft-screens does."""

    def __init__(self, up):
        self.up, self.asked = up, []

    def ask(self, text, timeout=10.0):
        self.asked.append(text)
        if text == "screens":
            return "ok " + str(self.up) + "".join(f" {i + 1}:1" for i in range(self.up))
        if text == "remotes":
            return "ok 0"
        if text == "head":
            return "ok 0 1.6 0 0"
        if text.startswith("get "):
            n = int(text.split()[1])
            if n <= self.up:
                return "ok 0 1.6 -2 1 0 0 0 1 0 0 0 1 2.0 1.0 0 none"
            raise RuntimeError(f"error no screen {n}")
        return "ok"

    def close(self):
        pass


LAYOUT = {"screens": [{}, {"hidden": True}],
          "hosts": [{"name": "oma7", "address": "192.0.2.30",
                     "displays": [{"id": "oma7-dp-2", "client": "frametop-1", "app": "Desktop"}]}]}


class Arrange(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.m = load_layout_module(pathlib.Path(self.tmp.name))
        self.m.save_layout(LAYOUT)

    def tearDown(self):
        self.tmp.cleanup()

    def streams_only(self):
        pathlib.Path(self.m.STREAMS_ONLY_PATH).touch()

    def test_marker(self):
        self.assertFalse(self.m.streams_only())
        self.assertEqual(self.m.native_count(), 2)
        self.streams_only()
        self.assertTrue(self.m.streams_only())
        self.assertEqual(self.m.native_count(), 0)
        self.assertEqual(self.m.screen_count(), 2, "the layout's screens stay configured")

    def test_streams_only_places_the_remote_displays_only(self):
        self.streams_only()
        screens = FakeScreens(up=0)
        self.m.screens_socket = lambda: screens
        self.m.apply_screens(wait=60)  # returns at once: no desktop screens to wait for
        asked = screens.asked
        self.assertEqual(asked.count("screens"), 1)
        self.assertTrue(any(a.startswith("remote 101 start frametop-1 192.0.2.30 Desktop ") for a in asked), asked)
        self.assertTrue(any(a.startswith("place 101 ") for a in asked), asked)
        self.assertIn("reveal 101", asked)
        for n in (1, 2):
            self.assertFalse([a for a in asked if a.split()[:2] in (["place", str(n)], ["width", str(n)],
                                                                   ["conceal", str(n)], ["reveal", str(n)])],
                             f"screen {n} was arranged: {asked}")

    def test_desktop_arranges_its_screens_as_before(self):
        screens = FakeScreens(up=2)
        self.m.screens_socket = lambda: screens
        self.m.apply_screens()
        asked = screens.asked
        self.assertTrue(any(a.startswith("place 1 ") for a in asked), asked)
        self.assertTrue(any(a.startswith("place 2 ") for a in asked), asked)
        self.assertIn("conceal 2", asked)
        self.assertTrue(any(a.startswith("place 101 ") for a in asked), asked)

    def test_remote_displays_sit_lower_without_the_screens(self):
        # The row goes above the desktop's screens; with none, it's at eye height.
        layout = self.m.load_layout()
        displays = self.m.remote_displays(layout)
        above = self.m.plan_remotes(layout, 2, displays)[0]
        alone = self.m.plan_remotes(layout, 0, displays)[0]
        self.assertLess(alone["face"][1], above["face"][1])

    def test_reset_moves_the_remote_displays_only(self):
        self.streams_only()
        screens = FakeScreens(up=0)
        self.m.screens_socket = lambda: screens
        self.m.recenter_screens()
        self.assertFalse([a for a in screens.asked if a in ("get 1", "get 2")], screens.asked)
        self.assertIn("get 101", screens.asked)


class StreamsCommand(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.m = load_layout_module(pathlib.Path(self.tmp.name))
        self.sessions, self.ran, self.running = [], [], False
        self.m.run_session = lambda setenv, what: self.sessions.append(setenv)
        self.m.session_running = lambda: self.running

        def run(argv, **kw):
            self.ran.append(argv)
            return subprocess.CompletedProcess(argv, 0, "", "")

        self.m.subprocess.run = run
        self.addCleanup(setattr, self.m.subprocess, "run", subprocess.run)

    def tearDown(self):
        self.tmp.cleanup()

    def test_start(self):
        self.assertEqual(self.m.main(["ft-layout", "streams", "start"]), 0)
        self.assertEqual(self.sessions, [["FT_STREAMS_ONLY=1"]])

    def test_start_while_running_starts_nothing(self):
        self.running = True
        self.assertEqual(self.m.main(["ft-layout", "streams", "start"]), 0)
        self.assertEqual(self.sessions, [])

    def test_stop_leaves_the_desktop_alone(self):
        self.running = True  # the desktop: no marker
        self.assertEqual(self.m.main(["ft-layout", "streams", "stop"]), 0)
        self.assertEqual(self.ran, [])

    def test_stop(self):
        self.running = True
        pathlib.Path(self.m.STREAMS_ONLY_PATH).touch()
        self.assertEqual(self.m.main(["ft-layout", "streams", "stop"]), 0)
        self.assertEqual(self.ran, [["systemctl", "--user", "stop", "frametop-desktop"]])

    def test_usage(self):
        self.assertEqual(self.m.main(["ft-layout", "streams", "restart"]), 2)
        self.assertEqual((self.sessions, self.ran), ([], []))


if __name__ == "__main__":
    unittest.main()
