#!/usr/bin/env python3
"""frametop.conf's one grammar (docs/config.md): conf/ft_conf.py, conf/ft_conf.h in C and in C++,
and bash sourcing the file all read conf/test/vectors.json the same way, and what
ft_conf.set_value writes reads back unchanged in all four, without running anything."""
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "conf"))
import ft_conf  # noqa: E402

with open(os.path.join(HERE, "vectors.json"), encoding="utf-8") as f:
    V = json.load(f)

BUILD = tempfile.mkdtemp(prefix="ft-conf-test-")
C = CPP = None


def setUpModule():
    global C, CPP
    cc, cxx = shutil.which("gcc") or shutil.which("cc"), shutil.which("g++") or shutil.which("c++")
    if not cc or not cxx:
        print("conf-test: no C/C++ compiler here: the C and C++ readers are NOT checked", file=sys.stderr)
        return
    C, CPP = os.path.join(BUILD, "dump-c"), os.path.join(BUILD, "dump-cpp")
    subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", os.path.join(HERE, "dump.c"), "-o", C], check=True)
    subprocess.run([cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", os.path.join(HERE, "dump.cpp"), "-o", CPP],
                   check=True)


def tearDownModule():
    shutil.rmtree(BUILD, ignore_errors=True)


def unhex(h):
    return bytes.fromhex(h).decode("utf-8")


def run(*argv, env=None):
    return subprocess.run(argv, capture_output=True, text=True, check=True, env=env).stdout


def key_of(line):
    return line.split("=", 1)[0].strip()


class Base(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="ft-conf-")
        self.n = 0

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def conf(self, text):
        self.n += 1
        path = os.path.join(self.dir, f"c{self.n}.conf")
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(text)
        return path

    def bash(self, path, key):
        """KEY as bash has it after `set -eu; . path`, run in the temp folder (the canary's)."""
        out = subprocess.run(["bash", "-c", 'set -eu; . "$1"; printf %s "${!2}"', "_", path, key],
                             capture_output=True, text=True, cwd=self.dir, check=False, env={"HOME": "/home/u", "PATH": os.environ["PATH"]})
        self.assertEqual(out.returncode, 0, f"bash failed on {path}: {out.stderr}")
        self.assertFalse(os.path.exists(os.path.join(self.dir, "canary")), "sourcing ran a command")
        return out.stdout


class Grammar(Base):
    def test_python(self):
        for v in V["ok"] + V["bash_differs"]:
            self.assertEqual(ft_conf.parse_line(v["line"]), (key_of(v["line"]), v["value"]), v["line"])
        for line in V["skip"]:
            self.assertIsNone(ft_conf.parse_line(line), repr(line))
        for line in V["bad"]:
            with self.assertRaises(ft_conf.ConfError, msg=repr(line)):
                ft_conf.parse_line(line)

    def test_c(self):
        if not C:
            self.skipTest("no compiler")
        lines = [v["line"] for v in V["ok"] + V["bash_differs"]] + V["skip"] + V["bad"]
        out = run(C, "lines", self.conf("\n".join(lines) + "\n")).splitlines()
        self.assertEqual(len(out), len(lines))
        want = ([f"ok\t{key_of(v['line'])}\t{v['value'].encode().hex()}" for v in V["ok"] + V["bash_differs"]]
                + ["skip"] * len(V["skip"]) + ["bad"] * len(V["bad"]))
        for line, got, w in zip(lines, out, want):
            self.assertEqual(got, w, repr(line))

    def test_cpp_and_c_get(self):
        if not C:
            self.skipTest("no compiler")
        for v in V["ok"] + V["bash_differs"]:
            path, key = self.conf(v["line"] + "\n"), key_of(v["line"])
            self.assertEqual(run(CPP, "read", path), f"{key}\t{v['value'].encode().hex()}\n", v["line"])
            self.assertEqual(run(C, "get", path, key), f"1\t{v['value'].encode().hex()}\n", v["line"])
        for line in V["skip"] + V["bad"]:
            path = self.conf(line + "\n")
            self.assertEqual(run(CPP, "read", path), "", repr(line))
            self.assertEqual(run(C, "get", path, "KEY"), "0\t\n", repr(line))

    def test_bash(self):
        for v in V["ok"]:
            self.assertEqual(self.bash(self.conf(v["line"] + "\n"), key_of(v["line"])), v["value"], v["line"])
        # ~ is literal to the readers; bash expands it (docs/config.md: paths expand it themselves)
        for v in V["bash_differs"]:
            self.assertEqual(self.bash(self.conf(v["line"] + "\n"), "KEY"), "/home/u/x")

    def test_last_line_wins_and_bad_lines_left_out(self):
        text = "KEY=1\nKEY=2   # this one\nKEY=a, b\nOTHER = 3\n"
        said = []
        self.assertEqual(ft_conf.parse(text, said.append), {"KEY": "2"})
        self.assertEqual(len(said), 2)
        self.assertIn(":3: KEY left out", said[0])
        self.assertIn(":4: OTHER left out", said[1])
        if C:
            path = self.conf(text)
            self.assertEqual(run(CPP, "read", path), "KEY\t" + b"2".hex() + "\n")
            self.assertEqual(run(C, "get", path, "KEY"), "1\t32\n")
            self.assertEqual(run(C, "get", path, "OTHER"), "0\t\n")

    def test_hands_near_alike(self):
        """DEE-297: ft-screens (handcut::Setting is ft_conf::Setting), the pointer helper (ft_conf::Read),
        C (ft_conf_setting) and Python read HANDS_NEAR alike however the line writes it; before,
        ft-screens skipped an indented line and the relay kept the quotes."""
        cases = {"HANDS_NEAR=0\n": "0", "  HANDS_NEAR=0\n": "0", 'HANDS_NEAR="0"\n': "0", "HANDS_NEAR = 0\n": None,
                 "HANDS_NEAR=0   # off\n": "0", "HANDS_NEAR=0\nHANDS_NEAR=force\n": "force"}
        for n, (text, want) in enumerate(cases.items()):
            home = os.path.join(self.dir, f"home{n}")
            os.makedirs(os.path.join(home, ".config"))
            path = os.path.join(home, ".config", "frametop.conf")
            with open(path, "w") as f:
                f.write(text)
            self.assertEqual(ft_conf.read(path, warn=None).get("HANDS_NEAR"), want, repr(text))
            if not C:
                continue
            env = {k: v for k, v in os.environ.items() if k != "FT_HANDS_NEAR"}
            env["HOME"] = home
            self.assertEqual(unhex(run(CPP, "setting", "HANDS_NEAR", "1", env=env).strip()), want or "1", repr(text))
            self.assertEqual(run(C, "setting", "HANDS_NEAR", env=env), f"{int(want is not None)}\t{(want or '').encode().hex()}\n")
            env["FT_HANDS_NEAR"] = "on"   # the environment wins
            self.assertEqual(unhex(run(CPP, "setting", "HANDS_NEAR", "1", env=env).strip()), "on")
            self.assertEqual(run(C, "setting", "HANDS_NEAR", env=env), "1\t" + b"on".hex() + "\n")

    def test_numbers(self):
        for v in V["numbers"]:
            got = ft_conf.number({"K": v["text"]}, "K", None, warn=None)
            if v["value"] is None:
                self.assertIsNone(got, repr(v["text"]))
            else:
                self.assertAlmostEqual(got, v["value"], msg=repr(v["text"]))
            if not C:
                continue
            c = run(C, "num", v["text"]).split()
            cpp = float(run(CPP, "double", self.conf(f"K={ft_conf.quote(v['text'])}\n"), "K", "-777"))
            if v["value"] is None:
                self.assertEqual(c, ["0"], repr(v["text"]))
                self.assertEqual(cpp, -777, repr(v["text"]))
            else:
                self.assertAlmostEqual(float(c[1]), v["value"], msg=repr(v["text"]))
                self.assertAlmostEqual(cpp, v["value"], msg=repr(v["text"]))

    def test_number_casts(self):
        self.assertEqual(ft_conf.number({"K": "8"}, "K", 16, int), 8)
        self.assertEqual(ft_conf.number({"K": "8.0"}, "K", 16, int), 8)
        self.assertEqual(ft_conf.number({"K": "8.5"}, "K", 16, int, warn=None), 16)
        self.assertEqual(ft_conf.number({}, "K", 16, int), 16)
        self.assertEqual(ft_conf.number({"K": ""}, "K", 16, int), 16)

    def test_example_config_is_in_the_grammar(self):
        """Every line of the shipped example parses, so a fresh install has no warnings."""
        path = os.path.join(ROOT, "session", "frametop.conf.example")
        said = []
        with open(path, encoding="utf-8", newline="") as f:
            conf = ft_conf.parse(f.read(), said.append)
        self.assertEqual(said, [])
        self.assertEqual(conf["BACKEND"], "screens")
        if C:
            self.assertEqual(run(C, "get", path, "HANDS_CPUS"), "1\t" + b"5,6,7".hex() + "\n")


class Writer(Base):
    VALUES = ("a", "", "a b", "$(touch canary)", "`touch canary`", "a;b", "it's", 'say "hi"', "~/x",
              "a,b, c", "x#y", "  pad  ", "hé", "\\", "*", "'", "''", "vendor.app*,b c,$(touch canary)",
              "0.075", "-1")
    BEFORE = "# Frametop settings\nFIRST=1   # first\nKEY=old    # keep me\nLAST='x y'\n"

    def check_reads(self, path, key, value):
        self.assertEqual(ft_conf.read(path).get(key), value)
        self.assertEqual(self.bash(path, key), value)
        if C:
            self.assertEqual(run(C, "get", path, key), f"1\t{value.encode().hex()}\n")
            self.assertIn(f"{key}\t{value.encode().hex()}\n", run(CPP, "read", path))

    def test_round_trip(self):
        for value in self.VALUES:
            path = self.conf(self.BEFORE)
            ft_conf.set_value("KEY", value, path)
            with open(path, encoding="utf-8") as f:
                lines = f.read().split("\n")
            self.assertEqual(lines[0:2] + lines[3:], ["# Frametop settings", "FIRST=1   # first", "LAST='x y'", ""])
            self.assertTrue(lines[2].endswith("   # keep me"), lines[2])
            self.check_reads(path, "KEY", value)
            self.check_reads(path, "LAST", "x y")
            self.assertTrue(ft_conf.check(path))

    def test_bare_when_safe(self):
        path = self.conf("")
        ft_conf.set_value("A", "0.075", path)
        ft_conf.set_value("B", "vendor.app*,x", path)
        ft_conf.set_value("C", "a b", path)
        with open(path) as f:
            self.assertEqual(f.read(), "A=0.075\nB=vendor.app*,x\nC='a b'\n")

    def test_refuses(self):
        path = self.conf(self.BEFORE)
        for value in ("a\nB=1", "a\r", "a\0"):
            with self.assertRaises(ValueError):
                ft_conf.set_value("KEY", value, path)
        for key in ("", "1A", "A B", "A=B", "$(x)"):
            with self.assertRaises(ValueError):
                ft_conf.set_value(key, "1", path)
        with open(path) as f:
            self.assertEqual(f.read(), self.BEFORE)

    def test_changes_the_line_that_counts(self):
        path = self.conf("KEY=1\nKEY=2   # wins\n")
        ft_conf.set_value("KEY", "3", path)
        with open(path) as f:
            self.assertEqual(f.read(), "KEY=1\nKEY=3   # wins\n")

    def test_replaces_a_line_bash_cant_read(self):
        """The DEE-293 shape: POINTER_IGNORE written with ", " (bash ran the second item)."""
        path = self.conf("POINTER_IGNORE=a.b, c.d*   # ignored panels\nHANDS_NEAR = 0\n")
        self.assertFalse(ft_conf.check(path))
        ft_conf.set_value("POINTER_IGNORE", "a.b,c.d*", path)
        ft_conf.set_value("HANDS_NEAR", "0", path)
        with open(path) as f:
            self.assertEqual(f.read(), "POINTER_IGNORE=a.b,c.d*   # ignored panels\nHANDS_NEAR=0\n")
        self.assertTrue(ft_conf.check(path))

    def test_new_file_and_mode(self):
        path = os.path.join(self.dir, "sub", "frametop.conf")
        ft_conf.set_value("KEY", "1", path)
        self.assertEqual(stat.S_IMODE(os.stat(path).st_mode), 0o644)
        os.chmod(path, 0o600)
        ft_conf.set_value("KEY", "2", path)
        self.assertEqual(stat.S_IMODE(os.stat(path).st_mode), 0o600)
        self.assertEqual(os.listdir(os.path.dirname(path)), ["frametop.conf"])  # no temp file left

    def test_symlink_kept(self):
        target = self.conf("KEY=1\n")
        link = os.path.join(self.dir, "link.conf")
        os.symlink(target, link)
        ft_conf.set_value("KEY", "2", link)
        self.assertTrue(os.path.islink(link))
        self.assertEqual(ft_conf.read(target), {"KEY": "2"})

    def test_no_trailing_newline(self):
        path = self.conf("A=1")
        ft_conf.set_value("B", "2", path)
        with open(path) as f:
            self.assertEqual(f.read(), "A=1\nB=2\n")


class Shell(Base):
    """The session's fallback for a file bash can't source safely."""

    def test_check_and_shell(self):
        path = self.conf("A=1\nB='x y'\nC=a, b\nD=$(touch canary)\nE = 0\nF=~/f\n")
        out = subprocess.run([sys.executable, os.path.join(ROOT, "conf", "ft_conf.py"), "check", path],
                             capture_output=True, text=True, check=False)
        self.assertEqual(out.returncode, 1)
        self.assertEqual(out.stderr.count("left out"), 3)
        out = subprocess.run([sys.executable, os.path.join(ROOT, "conf", "ft_conf.py"), "shell", path],
                             capture_output=True, text=True, check=True)
        self.assertEqual(out.stdout, "A=1\nB='x y'\nF='~/f'\n")
        script = self.conf(out.stdout)
        for key, want in (("A", "1"), ("B", "x y"), ("F", "~/f")):
            self.assertEqual(self.bash(script, key), want)
        good = self.conf("A=1   # c\n\n# x\nB='y z'\n")
        self.assertEqual(subprocess.run([sys.executable, os.path.join(ROOT, "conf", "ft_conf.py"), "check", good], check=False).returncode, 0)


if __name__ == "__main__":
    unittest.main()
