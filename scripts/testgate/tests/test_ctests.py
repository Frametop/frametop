#!/usr/bin/env python3
"""ctests (scripts/testgate/ctests.py, cbuild.py): a source several programs share compiles once,
a program that doesn't build fails alone while the rest run, a change runs only the tests it
reaches (headers included), and a Generate rewrites its file only when the text changed.
Made-up C programs in a temp folder, built with gcc and ninja.

  python3 scripts/testgate/tests/test_ctests.py
"""
import io
import os
import shutil
import sys
import tempfile
import textwrap
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import cbuild  # noqa: E402
import changed  # noqa: E402
import ctests  # noqa: E402
from cbuild import Check, Fail, Generate, Program, Skip  # noqa: E402

C = "-std=c11 -Wall -Werror"


@unittest.skipUnless(shutil.which("gcc") and shutil.which("ninja"), "no gcc or ninja here")
class CTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = self.tmp.name
        self.file("lib.h", "int answer(void);\n")
        self.file("lib.c", '#include "lib.h"\nint answer(void) { return 42; }\n')
        self.file("a.c", '#include "lib.h"\nint main(void) { return answer() == 42 ? 0 : 1; }\n')
        self.file("b.c", '#include "lib.h"\nint main(void) { return answer() == 42 ? 0 : 1; }\n')
        self.file("bad.c", "int main(void) { return nope; }\n")

    def tearDown(self):
        self.tmp.cleanup()

    def file(self, name, text):
        path = os.path.join(self.root, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write(textwrap.dedent(text))
        return path

    def run_items(self, items, changed=None):
        out = io.StringIO()
        r = ctests.Run(items, self.root, 2, changed, out=out)
        r.build()
        r.run()
        return r, out.getvalue()

    def programs(self):
        return [Program("a", ("a.c", "lib.c"), C, cc="gcc", test="a.c"),
                Program("b", ("b.c", "lib.c"), C, cc="gcc", test="b.c")]

    def test_a_shared_source_compiles_once(self):
        plan = cbuild.plan(self.programs() + [Program("c", ("a.c", "lib.c"), C + " -O2", cc="gcc", test="c")], self.root)
        self.assertEqual(plan.ninja.count(": cc lib.c"), 2)    # once for C, once for C -O2
        self.assertEqual(plan.objects["build/tests/a"][1], plan.objects["build/tests/b"][1])

    def test_all_pass(self):
        r, out = self.run_items(self.programs() + [Skip("x: SKIPPED: no ncnn")])
        self.assertEqual(r.status, 0, out)
        self.assertEqual(out.splitlines(), ["a.c: ok", "b.c: ok", "x: SKIPPED: no ncnn"])

    def test_a_program_that_doesnt_build_fails_alone(self):
        items = [Program("bad", ("bad.c",), C, cc="gcc", test="bad.c")] + self.programs()
        r, out = self.run_items(items)
        self.assertEqual(r.status, 1)
        self.assertIn("nope", out)                                  # the compiler's error
        self.assertIn("bad.c: FAILED (didn't build", out)
        self.assertIn("a.c: ok", out)
        self.assertIn("b.c: ok", out)

    def test_a_missing_source_or_package_fails_that_one(self):
        items = [Program("m", ("missing.c",), C, cc="gcc", test="m"),
                 Program("p", ("a.c", "lib.c"), C, cc="gcc", test="p", pkgs="no-such-package-ft"),
                 Check("uses-p", ("build/tests/p",), needs=("p",))] + self.programs()
        r, out = self.run_items(items)
        self.assertEqual(r.status, 1)
        self.assertIn("m: FAILED (can't build here: no missing.c)", out)
        self.assertIn("p: FAILED (can't build here: pkg-config", out)
        self.assertIn("uses-p: FAILED (needs p, which didn't build)", out)
        self.assertIn("a.c: ok", out)

    def test_a_failing_test_shows_its_output_and_fails(self):
        self.file("loud.c", '#include <stdio.h>\nint main(void) { puts("went wrong"); return 3; }\n')
        r, out = self.run_items([Program("loud", ("loud.c",), C, cc="gcc", test="loud.c"), Fail("y: FAILED (no stb)")])
        self.assertEqual(r.status, 1)
        self.assertEqual(out.splitlines()[:2], ["loud.c: FAILED", "went wrong"])

    def test_a_change_runs_what_it_reaches(self):
        self.run_items(self.programs())                  # built once: ninja knows what each read
        self.file("only-a.h", "#define A 1\n")
        self.file("a.c", '#include "lib.h"\n#include "only-a.h"\nint main(void) { return answer() == 42 ? 0 : A - 1; }\n')
        self.run_items(self.programs())
        _, out = self.run_items(self.programs(), changed=["only-a.h"])
        self.assertEqual(out.splitlines(), ["a.c: ok"])
        _, out = self.run_items(self.programs(), changed=["lib.h"])
        self.assertEqual(out.splitlines(), ["a.c: ok", "b.c: ok"])
        _, out = self.run_items(self.programs(), changed=["elsewhere/x.py"])
        self.assertEqual(out.splitlines(), [])
        # what decides the tests changed: all of them
        _, out = self.run_items(self.programs(), changed=["scripts/testgate/cprograms.py"])
        self.assertEqual(out.splitlines(), ["a.c: ok", "b.c: ok"])

    def test_a_check_runs_when_its_files_or_programs_change(self):
        self.file("t/check.sh", "exit 0\n")
        items = self.programs() + [Check("t/check.sh", ("sh", "t/check.sh", "build/tests/a"), needs=("a",))]
        self.run_items(items)
        self.assertEqual(self.run_items(items, changed=["t/check.sh"])[1].splitlines(), ["t/check.sh: ok"])
        self.assertEqual(self.run_items(items, changed=["a.c"])[1].splitlines(), ["a.c: ok", "t/check.sh: ok"])
        self.assertEqual(self.run_items(items, changed=["b.c"])[1].splitlines(), ["b.c: ok"])

    def test_tests_run_side_by_side_and_report_in_order(self):
        items = [Check(f"wait-{n}", ("sh", "-c", f"sleep {n}; echo said-{n}; exit {n == 0.1 and 1 or 0}"))
                 for n in (1, 0.1, 0)]
        start = time.monotonic()
        r = ctests.Run(items, self.root, 3, out=io.StringIO())
        r.build()
        r.run()
        self.assertLess(time.monotonic() - start, 1.8)
        self.assertEqual(r.status, 1)
        self.assertEqual(r.out.getvalue().splitlines(), ["wait-1: ok", "wait-0.1: FAILED", "said-0.1", "wait-0: ok"])

    def test_generate_keeps_an_unchanged_file(self):
        self.file("gen.sh", 'echo "int g(void) { return 1; }" > "$1"\n')
        g = Generate("build/tests/g.c", ("sh", "gen.sh", "{out}"))
        self.assertIsNone(cbuild.generate(g, self.root))
        path = os.path.join(self.root, "build/tests/g.c")
        before = os.stat(path).st_mtime_ns
        time.sleep(0.02)
        self.assertIsNone(cbuild.generate(g, self.root))
        self.assertEqual(os.stat(path).st_mtime_ns, before)
        self.assertIn("exit 1", cbuild.generate(Generate("build/tests/h.c", ("false",)), self.root))


class TestChanged(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = self.tmp.name
        for name, text in (("menu/test/test_menu.py", "import menu_model\n"),
                           ("voice/test/test_voice.py", "import ft_voiced\n"),
                           ("steam/test/test_steam.py", "from menu import menu_model\n")):
            os.makedirs(os.path.join(self.root, os.path.dirname(name)), exist_ok=True)
            with open(os.path.join(self.root, name), "w") as f:
                f.write(text)
        self.files = ["menu/test/test_menu.py", "voice/test/test_voice.py", "steam/test/test_steam.py"]

    def tearDown(self):
        self.tmp.cleanup()

    def test_python_picks_by_folder_and_by_name(self):
        pick = lambda c: changed.pick_python(self.files, c, self.root)  # noqa: E731
        self.assertEqual(pick(["voice/ft-voiced"]), ["voice/test/test_voice.py"])
        self.assertEqual(pick(["menu/menu_model.py"]), ["menu/test/test_menu.py", "steam/test/test_steam.py"])
        self.assertEqual(pick(["docs/readme.md"]), [])
        self.assertEqual(pick(["conf/ft_conf.py"]), self.files)

    def test_what_decides_the_tests_runs_everything(self):
        self.assertIsNone(changed.plan(["justfile"], self.root))
        self.assertIsNone(changed.plan(["scripts/testgate/cprograms.py"], self.root))


if __name__ == "__main__":
    unittest.main()
