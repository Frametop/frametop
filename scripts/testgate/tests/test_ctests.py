#!/usr/bin/env python3
"""ctests (scripts/testgate/ctests.py, cbuild.py): a source several programs share compiles once,
a program that doesn't build fails alone while the rest run, a change runs only the tests it
reaches (headers included), and a Generate rewrites its file only when the text changed.
Made-up C programs in a temp folder, built with gcc and ninja.

  python3 scripts/testgate/tests/test_ctests.py
"""
import contextlib
import io
import os
import unittest.mock
import shutil
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import cbuild  # noqa: E402
import changed  # noqa: E402
import ctests  # noqa: E402
import jobs  # noqa: E402
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

    def test_a_helper_a_test_runs_counts_as_its_input(self):
        self.file("helper.c", "int main(void) { return 0; }\n")
        self.file("runner.c", '#include <stdlib.h>\n#include <stdio.h>\nint main(int c, char **v) '
                  '{ char cmd[256]; snprintf(cmd, sizeof cmd, "%s", v[1]); return system(cmd) ? 1 : 0; }\n')
        helper = Program("helper", ("helper.c",), C, cc="gcc")
        runner = Program("runner", ("runner.c",), C, cc="gcc", test="runner.c", args=("build/tests/helper",),
                         needs=("helper",))
        self.run_items([helper, runner])
        self.assertEqual(self.run_items([helper, runner], changed=["helper.c"])[1].splitlines(), ["runner.c: ok"])
        # a helper that can't build fails what runs it, even with an old binary left
        gone = Program("helper", ("helper.c",), C, cc="gcc", pkgs="no-such-package-ft")
        r, out = self.run_items([gone, runner])
        self.assertEqual(r.status, 1)
        self.assertIn("runner.c: FAILED (needs helper, which didn't build)", out)

    def test_a_file_a_test_reads_counts_and_a_first_build_runs(self):
        p = [Program("a", ("a.c", "lib.c"), C, cc="gcc", test="a.c", watch=("data/table.txt",))]
        # no build before this one: no record of what it reads, so it runs
        self.assertEqual(self.run_items(p, changed=["elsewhere.txt"])[1].splitlines(), ["a.c: ok"])
        self.assertEqual(self.run_items(p, changed=["elsewhere.txt"])[1].splitlines(), [])
        self.assertEqual(self.run_items(p, changed=["data/table.txt"])[1].splitlines(), ["a.c: ok"])

    def test_a_generated_header_counts_through_what_its_made_from(self):
        self.file("gen.sh", 'cat proto.txt > "$1"\n')
        self.file("proto.txt", "#define ANSWER 42\n")
        self.file("g.c", '#include "build/tests/gen.h"\nint main(void) { return ANSWER == 42 ? 0 : 1; }\n')
        items = [Generate("build/tests/gen.h", ("sh", "gen.sh", "{out}"), watch=("proto.txt", "gen.sh")),
                 Program("g", ("g.c",), C + " -I.", cc="gcc", test="g.c")]
        self.run_items(items)
        self.file("proto.txt", "#define ANSWER 41\n")
        r, out = self.run_items(items, changed=["proto.txt"])
        self.assertEqual((r.status, out.splitlines()), (1, ["g.c: FAILED"]))

    def test_a_test_that_cant_start_fails_alone_and_ninja_status_doesnt_matter(self):
        items = [Check("gone", ("./no-such-program-ft",)), Program("bad", ("bad.c",), C, cc="gcc", test="bad.c")] \
            + self.programs()
        with unittest.mock.patch.dict(os.environ, NINJA_STATUS="%p "):
            r, out = self.run_items(items)
        self.assertEqual(r.status, 1)
        self.assertIn("gone: FAILED\nctests: can't run ./no-such-program-ft", out)
        self.assertIn("bad.c: FAILED (didn't build", out)
        self.assertIn("b.c: ok", out)

    def test_a_header_a_rebuild_starts_reading_counts(self):
        self.file("h1.h", "#define ONE 1\n")
        self.file("h2.h", "#define TWO 2\n")
        self.file("a.c", '#include "h1.h"\nint main(void) { return ONE - 1; }\n')
        p = [Program("a", ("a.c",), C, cc="gcc", test="a.c")]
        self.run_items(p)
        # h1.h now includes h2.h (an edit the changed set doesn't name), and h2.h breaks a.c
        self.file("h1.h", '#include "h2.h"\n#define ONE (TWO - 1)\n')
        self.file("h2.h", "#define TWO 3\n")
        r, out = self.run_items(p, changed=["h2.h"])
        self.assertEqual((r.status, out.splitlines()[:1]), (1, ["a.c: FAILED"]))

    def test_a_check_that_names_a_missing_program_fails_on_a_change_too(self):
        items = self.programs() + [Check("uses-x", ("sh", "-c", "exit 0"), needs=("no-such-program",))]
        self.run_items(items)
        r, out = self.run_items(items, changed=["elsewhere.txt"])
        self.assertEqual(r.status, 1)
        self.assertIn("uses-x: FAILED (needs no-such-program, which didn't build)", out)

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

    def test_name_with_its_extension_and_the_tests_that_read_everything(self):
        os.makedirs(os.path.join(self.root, "scripts/tests"))
        with open(os.path.join(self.root, "scripts/tests/test_install.py"), "w") as f:
            f.write('RUNNERS = ["power/run.sh", "gaze/run.sh"]\n')
        files = self.files + ["scripts/tests/test_install.py", "docs/tests/test_doc_links.py",
                              "scripts/tests/test_app_imports.py"]
        pick = lambda c: changed.pick_python(files, c, self.root)  # noqa: E731
        self.assertEqual(pick(["power/run.sh"]), ["scripts/tests/test_install.py", "docs/tests/test_doc_links.py"])
        self.assertIn("scripts/tests/test_app_imports.py", pick(["settings/sections.py"]))
        self.assertIn("docs/tests/test_doc_links.py", pick(["README.md"]))

    def test_the_verify_skill_runs_the_testbench_tests(self):
        with unittest.mock.patch.object(changed, "python_files", return_value=[]):
            runs = changed.plan([".claude/skills/verify-frametop/scripts/verify-frametop"], self.root)
        self.assertIn("test-testbench", [w for w, _, _ in runs])

    def git_repo(self):
        def g(*a):
            subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid", *a], cwd=self.root,
                           check=True, capture_output=True)
        with open(os.path.join(self.root, ".gitignore"), "w") as f:
            f.write("build/\n")
        g("init", "-q")
        g("add", ".")
        g("commit", "-qm", "one")
        return g

    def test_a_rename_is_both_paths_and_the_stamp_is_the_tree_a_run_started_on(self):
        g = self.git_repo()
        with unittest.mock.patch.object(changed, "TREE", self.root), \
                contextlib.redirect_stdout(io.StringIO()):
            start = changed.git("rev-parse", "HEAD").strip()
            changed.mark_start("r1")
            # during the run: test_menu.py moves, and that's committed
            g("mv", "menu/test/test_menu.py", "menu/test/test_menus.py")
            g("commit", "-qm", "two")
            changed.mark_green("r1")
            base, edited = changed.read_green()
            self.assertEqual((base, edited), (start, []))
            self.assertEqual(changed.changed_since(base), ["menu/test/test_menu.py", "menu/test/test_menus.py"])

    def test_each_run_records_its_own_start_and_a_run_with_none_records_nothing(self):
        g = self.git_repo()
        with unittest.mock.patch.object(changed, "TREE", self.root), \
                contextlib.redirect_stdout(io.StringIO()):
            one = changed.git("rev-parse", "HEAD").strip()
            changed.mark_start("a")     # run a starts on one
            with open(os.path.join(self.root, "x.py"), "w") as f:
                f.write("broken\n")
            g("add", ".")
            g("commit", "-qm", "two")
            changed.mark_start("b")     # run b starts on two (and will fail)
            changed.mark_green("a")
            self.assertEqual(changed.read_green(), (one, []))     # a's tree, not b's
            # no git (./ft dev test's image): nothing recorded, and b's file isn't taken
            with unittest.mock.patch.object(changed, "git", side_effect=OSError("no git")):
                changed.mark_start("c")
                changed.mark_green("c")
            self.assertEqual(changed.read_green(), (one, []))
            # an empty start record (a stopped write) records nothing either
            open(os.path.join(self.root, changed.PENDING + "d"), "w").close()
            self.assertEqual(changed.mark_green("d"), 0)
            self.assertEqual(changed.read_green(), (one, []))
            self.assertTrue(os.path.exists(os.path.join(self.root, changed.PENDING + "b")))

    def test_paths_come_whole(self):
        self.assertEqual(changed._paths("my file.py\0a/b c.md\0"), {"my file.py", "a/b c.md"})

    def test_what_decides_the_tests_runs_everything(self):
        self.assertIsNone(changed.plan(["justfile"], self.root))
        self.assertIsNone(changed.plan(["scripts/testgate/cprograms.py"], self.root))


class Jobs(unittest.TestCase):
    def test_the_tightest_quota_above_holds(self):
        with tempfile.TemporaryDirectory() as fs:
            for path, quota in (("", "max 100000"), ("user", "200000 100000"), ("user/app", "max 100000")):
                os.makedirs(os.path.join(fs, path), exist_ok=True)
                with open(os.path.join(fs, path, "cpu.max"), "w") as f:
                    f.write(quota + "\n")
            with unittest.mock.patch.object(jobs, "_cgroup", return_value="user/app"), \
                    unittest.mock.patch.object(jobs.os, "sched_getaffinity", return_value=set(range(8))):
                self.assertEqual(jobs._cpus(fs), 2)
        self.assertEqual(jobs.jobs({"FT_TEST_JOBS": "1"}), 1)


if __name__ == "__main__":
    unittest.main()
