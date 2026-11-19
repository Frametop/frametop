#!/usr/bin/env python3
"""testgate (scripts/testgate/testgate.py): a skip is reported as one, never as "ok",
a pattern that matches nothing fails the gate, so do a file whose tests never run and one that
hangs. Made-up test files in a temp folder.

  python3 scripts/testgate/tests/test_testgate.py
"""
import contextlib
import io
import json
import os
import signal
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import procs  # noqa: E402
import testgate  # noqa: E402

UNITTEST = """\
import os, unittest
class T(unittest.TestCase):
{body}
unittest.main()
"""


class Gate(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()

    def file(self, name, text):
        path = os.path.join(self.dir, name)
        with open(path, "w") as f:
            f.write(textwrap.dedent(text))
        return path

    def case(self, name, body):
        return self.file(name, UNITTEST.format(body=textwrap.indent(textwrap.dedent(body), "    ")))

    def gate(self, *patterns):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            status = testgate.main([os.path.join(self.dir, p) for p in patterns])
        return status, out.getvalue()

    def test_passing(self):
        how, ran, skipped, _, _ = testgate.run(self.case("a.py", "def test_a(self): pass\ndef test_b(self): pass"))
        self.assertEqual((how, ran, skipped), ("ok", 2, []))

    def test_all_skipped_is_skipped_with_its_reason(self):
        path = self.file("panel.py", """\
            import unittest
            @unittest.skipUnless(False, "the panel isn't built")
            class T(unittest.TestCase):
                def test_a(self): pass
                def test_b(self): pass
            unittest.main()
            """)
        self.assertEqual(testgate.run(path)[:3], ("skipped", 2, ["the panel isn't built"] * 2))
        status, out = self.gate("panel.py")
        self.assertEqual(status, 0)
        self.assertIn("panel.py: SKIPPED: the panel isn't built (x2)", out)
        self.assertNotIn("panel.py: ok", out)

    def test_some_skipped_says_so(self):
        path = self.case("b.py", """\
            def test_a(self): pass
            def test_b(self): self.skipTest("no headset")
            """)
        self.assertEqual(testgate.run(path)[:3], ("ok", 2, ["no headset"]))
        self.assertIn("b.py: ok, 1 skipped: no headset", self.gate("b.py")[1])

    def test_skipped_subtests_are_one_tests(self):
        path = self.case("s.py", """\
            def test_a(self):
                for i in range(3):
                    with self.subTest(i=i):
                        self.skipTest("no camera")
            def test_b(self): pass
            """)
        self.assertEqual(testgate.run(path)[:3], ("ok", 2, ["no camera"] * 3))

    def test_output_that_isnt_utf8(self):
        path = self.file("u.py", "import sys\nsys.stdout.buffer.write(b'\\xff\\n')\nraise SystemExit(1)\n")
        self.assertEqual(testgate.run(path)[0], "failed")

    def test_skip_in_setupclass(self):
        path = self.case("c.py", """\
            @classmethod
            def setUpClass(cls): raise unittest.SkipTest("no PySide6")
            def test_a(self): pass
            """)
        self.assertEqual(testgate.run(path)[:3], ("skipped", 0, ["no PySide6"]))

    def test_failure_fails_and_shows_its_output(self):
        path = self.case("d.py", "def test_a(self): self.assertEqual(1, 2)")
        self.assertEqual(testgate.run(path)[0], "failed")
        status, out = self.gate("d.py")
        self.assertEqual(status, 1)
        self.assertIn("d.py: FAILED", out)
        self.assertIn("AssertionError: 1 != 2", out)

    def test_plain_script_skips(self):
        whole = self.file("e.py", "import sys\nprint('no device here')\nsys.exit(77)\n")
        self.assertEqual(testgate.run(whole)[:3], ("skipped", 0, ["no device here"]))
        part = self.file("f.py", "print('ok  one')\nprint('skip  ft-frunner itself: no PyGObject here')\n")
        self.assertEqual(testgate.run(part)[:3], ("ok", 0, ["ft-frunner itself: no PyGObject here"]))
        self.assertEqual(testgate.run(self.file("g.py", "raise SystemExit(1)\n"))[0], "failed")

    def test_a_forked_child_doesnt_report_over_its_parent(self):
        path = self.case("h.py", """\
            def test_a(self):
                pid = os.fork()
                if pid == 0:
                    raise SystemExit(0)   # atexit runs here, in the child
                os.waitpid(pid, 0)
            """)
        self.assertEqual(testgate.run(path)[:3], ("ok", 1, []))

    def test_a_child_python_doesnt_report(self):
        path = self.case("i.py", """\
            def test_a(self):
                import subprocess, sys
                code = "import os, unittest; print(os.environ.get('FT_TEST_REPORT'))"
                self.assertEqual(subprocess.check_output([sys.executable, "-c", code], text=True), "None\\n")
            """)
        self.assertEqual(testgate.run(path)[0], "ok")

    def test_tests_defined_but_never_run_fail(self):
        path = self.file("j.py", """\
            import unittest
            class T(unittest.TestCase):
                def test_a(self): pass
                def test_b(self): pass
            """)   # no unittest.main(): it would pass having run nothing
        how, ran, _, _, stderr = testgate.run(path)
        self.assertEqual((how, ran), ("failed", 0))
        self.assertIn("defines 2 unittest test(s) but ran none", stderr)
        # A plain script with no TestCase still runs as before.
        self.assertEqual(testgate.run(self.file("k.py", "print('all passed')\n"))[:2], ("ok", 0))

    def test_a_hung_file_is_killed_with_what_it_started(self):
        marker = os.path.join(self.dir, "child-alive")
        path = self.file("l.py", f"""\
            import subprocess, sys, time
            # a child that would outlive the test, then the test hangs
            subprocess.Popen([sys.executable, "-c",
                              "import time; time.sleep(3); open({marker!r}, 'w').close()"])
            time.sleep(60)
            """)
        how, _, _, _, stderr = testgate.run(path, limit=1)
        self.assertEqual(how, "failed")
        self.assertIn("still running after 1 s, killed", stderr)
        time.sleep(3.5)
        self.assertFalse(os.path.exists(marker))   # the child went with it

    def test_the_timeout_can_be_set(self):
        with unittest.mock.patch.dict(os.environ, FT_TEST_TIMEOUT="5"):
            self.assertEqual(testgate.timeout(), 5)
        with unittest.mock.patch.dict(os.environ, FT_TEST_TIMEOUT="soon"):
            self.assertEqual(testgate.timeout(), testgate.TIMEOUT)
        with unittest.mock.patch.dict(os.environ, FT_TEST_TIMEOUT="0"):
            self.assertIsNone(testgate.timeout())   # no limit
        slow = self.file("m.py", "import time; time.sleep(1.5); print('all passed')\n")
        with unittest.mock.patch.dict(os.environ, FT_TEST_TIMEOUT="1"):
            self.assertEqual(testgate.run(slow, limit=None)[0], "ok")     # None is no limit, not the default
            self.assertEqual(testgate.run(slow)[0], "failed")

    def test_a_server_left_running_gets_sigterm(self):
        """A passing file's leftover is ended, and asked first: it gets SIGTERM and can clean up."""
        marker = os.path.join(self.dir, "cleaned-up")
        path = self.file("n.py", f"""\
            import subprocess, sys
            subprocess.Popen([sys.executable, "-c",
                              "import signal, sys, time\\n"
                              "signal.signal(signal.SIGTERM, lambda *a: (open({marker!r}, 'w').close(), sys.exit(0)))\\n"
                              "print('up', flush=True)\\n"
                              "time.sleep(60)"], stdout=subprocess.PIPE).stdout.readline()
            print("all passed")
            """)
        start = time.monotonic()
        self.assertEqual(testgate.run(path)[0], "ok")
        self.assertTrue(os.path.exists(marker))
        self.assertLess(time.monotonic() - start, 20)

    def test_a_leftover_in_a_process_group_of_its_own_is_ended_too(self):
        """ninja gives each command a process group of its own, in its session: a leftover there
        that is slow to go on SIGTERM still goes (SIGKILL), with the session."""
        pidfile = os.path.join(self.dir, "pid")
        child = ("import os, signal, time\n"
                 "signal.signal(signal.SIGTERM, lambda *a: time.sleep(5))\n"
                 f"open({pidfile!r}, 'w').write(str(os.getpid()))\n"
                 "time.sleep(60)")
        path = self.file("o.py", f"""\
            import os, subprocess, sys, time
            subprocess.Popen([sys.executable, "-c", {child!r}], process_group=0)
            while not os.path.exists({pidfile!r}): time.sleep(0.05)
            print("all passed")
            """)
        self.assertEqual(testgate.run(path)[0], "ok")
        with open(pidfile) as f:
            pid = int(f.read())
        try:
            with open(f"/proc/{pid}/stat") as f:
                state = f.read().rsplit(")", 1)[1].split()[0]
        except OSError:
            state = "gone"
        self.assertIn(state, ("gone", "Z"))

    def test_a_leftover_that_keeps_forking_is_ended_with_all_it_forked(self):
        """It ignores SIGTERM and forks every millisecond: one forked while /proc is read still goes."""
        loop = ("import os, signal, time\n"
                "signal.signal(signal.SIGTERM, lambda *a: None)\n"
                "print('ready', flush=True)\n"
                "while True:\n"
                "    if os.fork() == 0:\n"
                "        time.sleep(60); os._exit(0)\n"
                "    time.sleep(0.001)\n")
        p = subprocess.Popen([sys.executable, "-c", loop], stdout=subprocess.PIPE, start_new_session=True)
        try:
            self.assertEqual(p.stdout.readline().strip(), b"ready")
            procs.end_session(p.pid)
            time.sleep(0.1)
            self.assertEqual(procs.members(p.pid), [])
        finally:
            for pid in procs.members(p.pid):
                try:
                    os.kill(pid, signal.SIGKILL)
                except OSError:
                    pass
            p.stdout.close()
            p.wait()

    def test_a_run_started_ignoring_sighup_keeps_ignoring_it(self):
        """nohup: a closed terminal doesn't end the run, and the files it runs still start with
        SIGHUP's default (not nohup's ignore); without nohup, a hangup ends it (and cleans up)."""
        started = os.path.join(self.dir, "started")
        path = self.file("w.py", f"""\
            import os, signal, time
            with open({started!r} + ".tmp", "w") as f:   # whole: the test reads it once it's there
                f.write(str(signal.getsignal(signal.SIGHUP) == signal.SIG_DFL))
            os.replace({started!r} + ".tmp", {started!r})
            time.sleep(1.5)
            print("all passed")
            """)
        gate = os.path.join(os.path.dirname(os.path.abspath(testgate.__file__)), "testgate.py")
        for ignore, want in ((True, 0), (False, 128 + signal.SIGHUP)):
            if os.path.exists(started):
                os.unlink(started)
            disposition = signal.SIG_IGN if ignore else signal.SIG_DFL
            p = subprocess.Popen([sys.executable, gate, "--jobs", "1", path], stdout=subprocess.DEVNULL,
                                 preexec_fn=lambda d=disposition: signal.signal(signal.SIGHUP, d))
            try:
                deadline = time.monotonic() + 20
                while not os.path.exists(started) and time.monotonic() < deadline:   # its handler is in
                    time.sleep(0.02)
                p.send_signal(signal.SIGHUP)
                self.assertEqual(p.wait(timeout=20), want)
            finally:
                p.kill()
                p.wait()
            with open(started) as f:
                self.assertEqual(f.read(), "True")

    def test_a_stopped_runner_starts_nothing(self):
        """A worker that gets to its test after the runner was stopped doesn't start it."""
        marker = os.path.join(self.dir, "started")
        live = procs.Live()
        procs.stop(live)
        with self.assertRaises(procs.Stopped):
            procs.run_session([sys.executable, "-c", f"open({marker!r}, 'w')"], live, None)
        time.sleep(0.3)
        self.assertFalse(os.path.exists(marker))

    def test_stuck_sessions_end_together(self):
        """Four tests that ignore SIGTERM, stopped at once: about a second in all, not one each."""
        live = procs.Live()
        stuck = "import signal, time; signal.signal(signal.SIGTERM, signal.SIG_IGN); print('up', flush=True); time.sleep(60)"
        ps = [subprocess.Popen([sys.executable, "-c", stuck], stdout=subprocess.PIPE, start_new_session=True)
              for _ in range(4)]
        try:
            for p in ps:
                p.stdout.readline()
                live.sids.add(p.pid)
            start = time.monotonic()
            procs.stop(live)
            self.assertLess(time.monotonic() - start, 2.5)
            self.assertEqual([procs.members(p.pid) for p in ps], [[]] * 4)
        finally:
            for p in ps:
                p.kill()
                p.stdout.close()
                p.wait()

    def test_a_leader_whose_first_thread_ended_still_counts(self):
        """Its first thread is gone (a zombie in /proc), the rest still run: it's ended too."""
        code = ("import ctypes, threading, time\n"
                "threading.Thread(target=time.sleep, args=(60,)).start()\n"
                "print('up', flush=True)\n"
                "ctypes.CDLL(None).pthread_exit(None)\n")
        p = subprocess.Popen([sys.executable, "-c", code], stdout=subprocess.PIPE, start_new_session=True)
        try:
            self.assertEqual(p.stdout.readline().strip(), b"up")
            deadline = time.monotonic() + 5
            while procs._threads(p.pid) != 1 and time.monotonic() < deadline:   # wait for the zombie
                with open(f"/proc/{p.pid}/stat") as f:
                    if f.read().rsplit(")", 1)[1].split()[0] == "Z":
                        break
                time.sleep(0.02)
            self.assertEqual(procs.members(p.pid), [p.pid])
            procs.end_session(p.pid)
            self.assertEqual(procs.members(p.pid), [])
        finally:
            p.kill()
            p.stdout.close()
            p.wait()

    def test_on_stop_twice_under_nohup_and_its_way_out(self):
        """Called twice, an ignored SIGHUP is still ignored; and it's SIG_IGN again at the exit."""
        code = ("import atexit, os, signal, sys\n"
                f"sys.path.insert(0, {os.path.dirname(os.path.abspath(procs.__file__))!r})\n"
                "import procs\n"
                "signal.signal(signal.SIGHUP, signal.SIG_IGN)\n"
                "atexit.register(lambda: print('at exit', signal.getsignal(signal.SIGHUP) is signal.SIG_IGN))\n"
                "procs.on_stop(lambda s, _: sys.exit(128 + s))\n"
                "procs.on_stop(lambda s, _: sys.exit(128 + s))\n"
                "os.kill(os.getpid(), signal.SIGHUP)\n"
                "print('survived')\n")
        p = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True, timeout=20)
        self.assertEqual((p.returncode, p.stdout.split("\n")[:2]), (0, ["survived", "at exit True"]))

    def test_a_pattern_that_matches_nothing_fails(self):
        self.case("a.py", "def test_a(self): pass")
        status, out = self.gate("a.py", "gone/*.py")
        self.assertEqual(status, 1)
        self.assertIn("gone/*.py: FAILED, matches no file", out)
        self.assertIn("python files: 1 ok, 0 skipped, 0 failed", out)

    def test_files_run_side_by_side_and_report_in_order(self):
        for name, wait in (("p1.py", 1.5), ("p2.py", 0), ("p3.py", 0)):
            self.file(name, f"import time; time.sleep({wait}); print('all passed')\n")
        out = io.StringIO()
        start = time.monotonic()
        times = os.path.join(self.dir, "times.json")
        with contextlib.redirect_stdout(out):
            status = testgate.main([os.path.join(self.dir, "p*.py")], njobs=3, times=times)
        self.assertLess(time.monotonic() - start, 2.5)
        self.assertEqual(status, 0)
        lines = [line.rsplit("/", 1)[-1] for line in out.getvalue().splitlines()[:3]]
        self.assertEqual(lines, ["p1.py: ok", "p2.py: ok", "p3.py: ok"])
        with open(times) as f:
            self.assertEqual(len(json.load(f)), 3)

    def test_a_file_that_runs_alone_runs_alone(self):
        """q2 says it runs alone: q1 has ended before it starts, and q3 starts after it ends."""
        log = os.path.join(self.dir, "log")
        for name, first in (("q1.py", "# runs with others"), ("q2.py", testgate.ALONE), ("q3.py", "# runs with others")):
            self.file(name, f"""\
                {first}
                import os, time
                with open({log!r}, "a") as f: f.write("start {name}\\n")
                time.sleep(0.3)
                with open({log!r}, "a") as f: f.write("end {name}\\n")
                """)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(testgate.main([os.path.join(self.dir, "q*.py")], njobs=3), 0)
        with open(log) as f:
            events = f.read().split("\n")
        q2 = events.index("start q2.py")
        self.assertEqual(events[q2 - 1], "end q1.py")
        self.assertEqual(events[q2 + 1], "end q2.py")

    def test_each_file_has_its_own_tmpdir_and_patterns_come_from_a_file(self):
        self.file("r.py", "import os, tempfile; assert tempfile.gettempdir() == os.environ['TMPDIR'] != '/tmp'\n")
        listing = self.file("suites.txt", f"# a comment\n{os.path.join(self.dir, 'r.py')}\n")
        self.assertEqual(testgate.expand([f"@{listing}", "x.py"]), [os.path.join(self.dir, "r.py"), "x.py"])
        self.assertEqual(testgate.run(os.path.join(self.dir, "r.py"))[0], "ok")


if __name__ == "__main__":
    unittest.main()
