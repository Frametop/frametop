#!/usr/bin/env python3
"""session.get_ready (the window's Getting ready step) and openvr_env, with the processes,
units and panel faked: the parts go in order, a missing one starts (Frametop's hand tracking
when it's installed, else the recorder's own units), and the first that can't start stops it.

  python3 hands/rec/tests/test_ready.py
"""
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import session  # noqa: E402


class FakeHost:
    """What get_ready looks at, as plain flags, and what it started."""

    def __init__(self, steamvr=True, ring=True, tracker=True, panel=True, live=False):
        self.steamvr, self.ring, self.tracker, self.panel, self.live = steamvr, ring, tracker, panel, live
        self.started = []

    def install(self, test):
        host = self
        saved = {}

        def patch(name, value):
            saved[name] = getattr(session, name)
            setattr(session, name, value)

        class Panel:
            def __init__(self, *a, **k):
                pass

            def cmd(self, text, reply=False, timeout=0.5):
                return "ok hidden" if host.panel else None

            def close(self):
                pass

        class Done:
            returncode = 0
            stdout = stderr = ""

        def run(argv, **k):
            if "start" in argv and session.LIVE_HANDS_UNIT in argv:
                host.started.append(session.LIVE_HANDS_UNIT)
                host.live = host.ring = host.tracker = True
            return Done()

        def start_camd(path=None, log=None, stop=None, timeout=15.0):
            host.started.append(session.CAMD_UNIT)
            host.ring = True
            return True

        def start_unit(unit, what, argv, log=None, **k):
            host.started.append(unit)
            host.tracker = True
            return True

        def start_panel(panel_bin=None, stdout=None, timeout=15.0):
            host.started.append("ft-handpanel")
            host.panel = True
            return "panel process"

        patch("steamvr_running", lambda: host.steamvr)
        patch("ring_alive", lambda path=None: host.ring)
        patch("tracker_running", lambda: host.tracker)
        patch("unit_active", lambda unit: host.live and unit == session.LIVE_HANDS_UNIT)
        patch("start_camd", start_camd)
        patch("start_unit", start_unit)
        patch("start_panel", start_panel)
        patch("Panel", Panel)
        patch("READY_WAIT_S", 0.3)
        saved_run = session.subprocess.run
        session.subprocess.run = run
        test.addCleanup(lambda: [setattr(session, k, v) for k, v in saved.items()])
        test.addCleanup(lambda: setattr(session.subprocess, "run", saved_run))
        return self


def ready(host, **kw):
    parts = []
    r = session.get_ready(on_part=lambda key, state, text: parts.append((key, state)), **kw)
    return r, parts


class GetReadyTest(unittest.TestCase):
    def test_all_running(self):
        host = FakeHost().install(self)
        r, parts = ready(host)
        self.assertTrue(r["ok"])
        self.assertEqual(r["units"], [])
        self.assertIsNone(r["panel"])
        self.assertEqual([k for k, s in parts if s == "ok"], [k for k, _ in session.READY_PARTS])
        self.assertEqual(host.started, [])

    def test_no_steamvr(self):
        host = FakeHost(steamvr=False, ring=False).install(self)
        r, parts = ready(host)
        self.assertFalse(r["ok"])
        self.assertIn("SteamVR", r["error"])
        self.assertEqual(parts[-1], ("steamvr", "failed"))
        self.assertEqual(host.started, [])   # nothing started without SteamVR

    def test_standalone_starts_its_own(self):
        host = FakeHost(ring=False, tracker=False, panel=False).install(self)
        r, _ = ready(host)
        self.assertTrue(r["ok"], r["error"])
        self.assertEqual(host.started, [session.CAMD_UNIT, session.HANDS_UNIT, "ft-handpanel"])
        self.assertEqual(r["units"], [session.CAMD_UNIT, session.HANDS_UNIT])
        self.assertEqual(r["panel"], "panel process")

    def test_frametop_starts_its_hand_tracking(self):
        host = FakeHost(ring=False, tracker=False).install(self)
        r, _ = ready(host, frametop_hands=True)
        self.assertTrue(r["ok"], r["error"])
        self.assertEqual(host.started, [session.LIVE_HANDS_UNIT])   # it brings its cameras
        self.assertEqual(r["units"], [session.LIVE_HANDS_UNIT])

    def test_testing_session_needs_frametop(self):
        host = FakeHost().install(self)
        r, parts = ready(host, need_live=True)
        self.assertFalse(r["ok"])
        self.assertEqual(r["error"], session.DEBUG_NEEDS_HANDS)
        # Frametop's tracking off, the recorder's own running: Frametop's starts
        host = FakeHost(live=False).install(self)
        r, _ = ready(host, frametop_hands=True, need_live=True)
        self.assertTrue(r["ok"], r["error"])
        self.assertEqual(host.started, [session.LIVE_HANDS_UNIT])

    def test_panel_fails(self):
        host = FakeHost(panel=False).install(self)

        def broken(panel_bin=None, stdout=None, timeout=15.0):
            raise RuntimeError("ft-handpanel exited (is SteamVR running?)")
        session.start_panel = broken
        r, parts = ready(host)
        self.assertFalse(r["ok"])
        self.assertEqual(parts[-1], ("panel", "failed"))
        self.assertIn("ft-handpanel", r["error"])


class OpenvrEnvTest(unittest.TestCase):
    def test_frametop_desktop(self):
        """Frametop's desktop's XDG_CONFIG_HOME has no openvrpaths.vrpath: ~/.config instead."""
        home = tempfile.mkdtemp()
        self.addCleanup(lambda: __import__("shutil").rmtree(home))
        os.makedirs(os.path.join(home, ".config", "openvr"))
        open(os.path.join(home, ".config", "openvr", "openvrpaths.vrpath"), "w").close()
        os.makedirs(os.path.join(home, ".config", "frametop"))
        saved = dict(os.environ)
        self.addCleanup(lambda: (os.environ.clear(), os.environ.update(saved)))
        os.environ["HOME"] = home
        os.environ["XDG_CONFIG_HOME"] = os.path.join(home, ".config", "frametop")
        self.assertEqual(session.openvr_env()["XDG_CONFIG_HOME"], os.path.join(home, ".config"))
        os.environ["XDG_CONFIG_HOME"] = os.path.join(home, ".config")
        self.assertEqual(session.openvr_env()["XDG_CONFIG_HOME"], os.path.join(home, ".config"))
        del os.environ["XDG_CONFIG_HOME"]
        self.assertNotIn("XDG_CONFIG_HOME", session.openvr_env())


if __name__ == "__main__":
    unittest.main()
