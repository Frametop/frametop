#!/usr/bin/env python3
"""testgate (scripts/testgate/testgate.py): a skip is reported as one, never as "ok",
a pattern that matches nothing fails the gate, so do a file whose tests never run and one that
hangs. Made-up test files in a temp folder.

  python3 scripts/testgate/tests/test_testgate.py
"""
import contextlib
import io
import os
import sys
import tempfile
import textwrap
import time
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
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
        self.assertEqual(testgate.run(self.file("m.py", "print('all passed')\n"), limit=None)[0], "ok")

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

    def test_a_pattern_that_matches_nothing_fails(self):
        self.case("a.py", "def test_a(self): pass")
        status, out = self.gate("a.py", "gone/*.py")
        self.assertEqual(status, 1)
        self.assertIn("gone/*.py: FAILED, matches no file", out)
        self.assertIn("python files: 1 ok, 0 skipped, 0 failed", out)


if __name__ == "__main__":
    unittest.main()
