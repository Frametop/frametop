"""hands/ft-hands-stop, offline: it stops the ft-hands that wrote its pidfile and no other process, a
stale or linked pidfile signals nothing, and it always exits 0 (a unit's ExecStartPre and ExecStopPost).

The stand-ins are python3 started through a link named ft-hands, so their comm is ft-hands."""
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

STOP = pathlib.Path(__file__).resolve().parents[1] / "ft-hands-stop"


class FtHandsStop(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="ft-hands-stop-"))
        self.procs = []
        for name in ("ft-hands", "ft-other"):
            (self.tmp / name).symlink_to(sys.executable)

    def tearDown(self):
        for p in self.procs:
            if p.poll() is None:
                p.kill()
            p.wait()
        shutil.rmtree(self.tmp)

    def start(self, name="ft-hands", pidfile=None):
        """A sleeping stand-in, with --pidfile PIDFILE on its command line as ft-hands has it."""
        argv = [str(self.tmp / name), "-I", "-c", "import time; time.sleep(60)"]
        if pidfile:
            argv += ["--pidfile", str(pidfile)]
        p = subprocess.Popen(argv)
        self.procs.append(p)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:   # until the link's name shows (exec done)
            try:
                if pathlib.Path(f"/proc/{p.pid}/comm").read_text().strip() == name:
                    return p
            except OSError:
                pass
            time.sleep(0.02)
        self.fail(f"{name} never started")

    def pidfile(self, pid, name="hands.pid"):
        f = self.tmp / name
        f.write_text(f"{pid}\n")
        return f

    def stop(self, *args):
        r = subprocess.run(["bash", str(STOP), *map(str, args)], capture_output=True, text=True, timeout=30)
        return r.returncode

    def alive(self, p):
        time.sleep(0.2)
        return p.poll() is None

    def test_it_stops_its_own_and_leaves_the_others(self):
        f = self.tmp / "hands.pid"
        mine = self.start(pidfile=f)
        other = self.start(pidfile=self.tmp / "handrec.pid")   # the Hand Recorder's tracker
        f.write_text(f"{mine.pid}\n")
        self.assertEqual(self.stop(f), 0)
        self.assertEqual(mine.wait(timeout=5), -15)   # TERM, not KILL
        self.assertTrue(self.alive(other))
        self.assertFalse(f.exists(), "the stale pidfile goes")

    def test_a_pid_that_went_to_another_program_is_not_signalled(self):
        f = self.tmp / "hands.pid"
        for p in (self.start("ft-other", pidfile=f),   # the name doesn't match
                  self.start()):                         # an ft-hands without this pidfile
            f.write_text(f"{p.pid}\n")
            self.assertEqual(self.stop(f), 0)
            self.assertTrue(self.alive(p))
            self.assertFalse(f.exists(), "a stale pidfile goes")

    def test_a_linked_or_missing_pidfile_does_nothing(self):
        f = self.tmp / "hands.pid"
        p = self.start(pidfile=f)
        real = self.pidfile(p.pid, "real.pid")
        f.symlink_to(real)
        self.assertEqual(self.stop(f), 0)
        self.assertTrue(self.alive(p))
        self.assertTrue(real.exists())
        self.assertEqual(self.stop(self.tmp / "none.pid"), 0)
        self.assertEqual(self.stop(self.pidfile("not a pid", "junk.pid")), 0)
        self.assertEqual(self.stop(), 2)   # no pidfile given: usage

    def test_one_that_ignores_term_is_killed(self):
        f = self.tmp / "hands.pid"
        p = subprocess.Popen([str(self.tmp / "ft-hands"), "-I", "-c",
                              "import signal, time; signal.signal(signal.SIGTERM, signal.SIG_IGN); "
                              "open(%r, 'w').close(); time.sleep(60)" % str(self.tmp / "ready"),
                              "--pidfile", str(f)])
        self.procs.append(p)
        deadline = time.monotonic() + 5
        while not (self.tmp / "ready").exists():
            self.assertLess(time.monotonic(), deadline)
            time.sleep(0.02)
        f.write_text(f"{p.pid}\n")
        self.assertEqual(self.stop(f), 0)
        self.assertEqual(p.wait(timeout=5), -9)


if __name__ == "__main__":
    unittest.main()
