"""hands/ft-hands-stop, offline: it stops the ft-hands that wrote its pidfile, and one from before pidfiles
started with exactly the arguments it's given, and no other process; a stale or linked pidfile signals
nothing, and it always exits 0 (a unit's ExecStartPre and ExecStopPost).

The stand-ins are a copy of python3 named ft-hands (the program ft-hands-stop reads from /proc/PID/exe),
started as ft-hands (a host one's process name) or through a link named exe (podman starts a program in
the container so that its process name is "exe"), and a link named ft-other for another program."""
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

STOP = pathlib.Path(__file__).resolve().parents[1] / "ft-hands-stop"
SLEEP = ["-I", "-c", "import time; time.sleep(60)"]


class FtHandsStop(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="ft-hands-stop-"))
        self.procs = []
        shutil.copy(pathlib.Path(sys.executable).resolve(), self.tmp / "ft-hands")
        (self.tmp / "exe").symlink_to("ft-hands")
        (self.tmp / "ft-other").symlink_to(pathlib.Path(sys.executable).resolve())

    def tearDown(self):
        for p in self.procs:
            if p.poll() is None:
                p.kill()
            p.wait()
        shutil.rmtree(self.tmp)

    def start(self, name="ft-hands", args=()):
        """A sleeping stand-in started as name, with args after its own (as ft-hands has them)."""
        p = subprocess.Popen([str(self.tmp / name), *SLEEP, *map(str, args)])
        self.procs.append(p)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:   # until the name shows (exec done)
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
        for name in ("ft-hands", "exe"):   # on the host, and in the container
            with self.subTest(name):
                f = self.tmp / "hands.pid"
                mine = self.start(name, ("--pidfile", f))
                other = self.start(name, ("--pidfile", self.tmp / "handrec.pid"))   # the Hand Recorder's
                f.write_text(f"{mine.pid}\n")
                self.assertEqual(self.stop(f), 0)
                self.assertEqual(mine.wait(timeout=5), -15)   # TERM, not KILL
                self.assertTrue(self.alive(other))
                self.assertFalse(f.exists(), "the stale pidfile goes")

    def test_a_pid_that_went_to_another_program_is_not_signalled(self):
        f = self.tmp / "hands.pid"
        for p in (self.start("ft-other", ("--pidfile", f)),   # not an ft-hands program
                  self.start()):                                # an ft-hands without this pidfile
            f.write_text(f"{p.pid}\n")
            self.assertEqual(self.stop(f), 0)
            self.assertTrue(self.alive(p))
            self.assertFalse(f.exists(), "a stale pidfile goes")

    def test_one_from_before_pidfiles_is_stopped_by_its_exact_arguments(self):
        f = self.tmp / "hands.pid"
        old = self.start("exe", ("--status", "60"))                         # the unit's, before an update
        recorder = self.start("exe", ("--no-gestures", "--status", "0"))    # the Hand Recorder's
        cutouts = self.start("exe", ("--status", "60", "--no-gestures"))    # more arguments
        new = self.start("exe", ("--status", "60", "--pidfile", self.tmp / "other.pid"))
        host = self.start("ft-other", ("--status", "60"))                   # not an ft-hands program
        self.assertEqual(self.stop(f, *SLEEP, "--status", "60"), 0)   # its whole arguments, the stand-in's too
        self.assertEqual(old.wait(timeout=5), -15)
        for p in (recorder, cutouts, new, host):
            self.assertTrue(self.alive(p))
        self.assertEqual(self.stop(f), 0)   # without them, only the pidfile's
        self.assertTrue(self.alive(cutouts))

    def test_a_linked_or_missing_pidfile_does_nothing(self):
        f = self.tmp / "hands.pid"
        p = self.start(args=("--pidfile", f))
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
        p = subprocess.Popen([str(self.tmp / "exe"), "-I", "-c",
                              "import signal, time; signal.signal(signal.SIGTERM, signal.SIG_IGN); "
                              "open(%r, 'w').close(); time.sleep(60)" % str(self.tmp / "ready"),
                              "--pidfile", str(f)])
        self.procs.append(p)
        deadline = time.monotonic() + 5
        while not (self.tmp / "ready").exists():
            self.assertLess(time.monotonic(), deadline)
            time.sleep(0.02)
        f.write_text(f"{p.pid}\n")
        t = time.monotonic()
        self.assertEqual(self.stop(f), 0)
        self.assertLess(time.monotonic() - t, 4.5, "inside the units' TimeoutStopSec=5")
        self.assertEqual(p.wait(timeout=5), -9)


if __name__ == "__main__":
    unittest.main()
