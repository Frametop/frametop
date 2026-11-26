#!/usr/bin/env python3
"""hands/rec/handsneeded.py: the heartbeat that keeps Frametop's ft-hands unparked while the recorder
records (hands/track/park.h), and the recorder's own ft-hands, stopped through its pidfile.

  python3 -m hands.rec.tests.test_handsneeded
"""
import os
import pathlib
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

from hands.rec import session  # noqa: E402
from hands.rec.handsneeded import HandsNeeded  # noqa: E402
import recorder.units  # noqa: E402


class HandsNeededTest(unittest.TestCase):
    def test_beats_then_goes(self):
        with tempfile.TemporaryDirectory() as d:
            n = HandsNeeded(os.path.join(d, "run"), beat_s=0.05)
            n.start()
            self.assertTrue(os.path.exists(n.path))
            old = time.time() - 10
            os.utime(n.path, (old, old))
            time.sleep(0.3)
            self.assertGreater(os.stat(n.path).st_mtime, time.time() - 3)   # touched again: fresh
            n.stop()
            self.assertFalse(os.path.exists(n.path))
            n.stop()   # twice is fine

    def test_never_follows_a_link(self):
        with tempfile.TemporaryDirectory() as d:
            target = os.path.join(d, "elsewhere")
            os.symlink(target, os.path.join(d, "needed"))
            n = HandsNeeded(d, beat_s=10)
            n.start()
            n.stop()
            self.assertFalse(os.path.exists(target))


class OwnTrackerTest(unittest.TestCase):
    def test_in_frametops_container_it_has_a_pidfile(self):
        with mock.patch.object(session, "STANDALONE", None):
            argv, post = session.own_tracker("/tmp/ring")
        pidfile = os.path.join(session.run_dir(), session.HANDS_PIDFILE)
        self.assertEqual(argv[:2], [session.IN_BOX, session.FT_HANDS])
        self.assertEqual(argv[argv.index("--pidfile") + 1], pidfile)
        self.assertEqual(argv[-2:], ["--ring", "/tmp/ring"])
        self.assertEqual(post, [session.FT_HANDS_STOP, pidfile])
        self.assertTrue(os.access(session.FT_HANDS_STOP, os.X_OK))

    def test_on_the_host_systemd_stops_it(self):
        with mock.patch.object(session, "STANDALONE", {"version": "x"}):
            argv, post = session.own_tracker()
        self.assertEqual(argv[0], session.FT_HANDS)
        self.assertNotIn("--pidfile", argv)
        self.assertIsNone(post)

    def test_the_unit_runs_the_stop_after_it(self):
        with mock.patch.object(recorder.units, "unit_active", return_value=False), \
                mock.patch.object(recorder.units.subprocess, "run") as run:
            run.return_value = mock.Mock(returncode=0, stdout="", stderr="")
            recorder.units.start_unit("u.service", "x", ["/bin/true"], stop_post=["/a b/stop", "/run/p.pid"])
        cmd = run.call_args[0][0]
        prop = cmd[cmd.index("-p", cmd.index("TimeoutStopSec=5")) + 1]
        self.assertEqual(prop, "ExecStopPost=-" + shlex.join(["/a b/stop", "/run/p.pid"]))
        self.assertEqual(cmd[-1], "/bin/true")


class FindProcessesTest(unittest.TestCase):
    def test_it_finds_an_ft_hands_named_exe(self):
        """An ft-hands from before it set its own name is named "exe" once it has started again through
        /proc/self/exe: tracker_running() has to see it, or the recorder says hand tracking didn't start
        while Frametop's runs."""
        with tempfile.TemporaryDirectory() as d:
            prog = pathlib.Path(d, "ft-hands")
            shutil.copy(pathlib.Path(sys.executable).resolve(), prog)
            pathlib.Path(d, "exe").symlink_to("ft-hands")
            p = subprocess.Popen([os.path.join(d, "exe"), "-I", "-c", "import time; time.sleep(30)", "--status", "60"])
            try:
                deadline = time.monotonic() + 5
                while pathlib.Path("/proc/%d/comm" % p.pid).read_text().strip() != "exe":
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(0.02)
                found = dict(session.find_processes("ft-hands"))
                self.assertIn(p.pid, found)
                self.assertEqual(found[p.pid][-2:], ["--status", "60"])
            finally:
                p.kill()
                p.wait()


if __name__ == "__main__":
    unittest.main()
