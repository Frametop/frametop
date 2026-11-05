#!/usr/bin/env python3
"""session/frametop-session.sh loading frametop.conf: a conf in the grammar
(docs/config.md) is sourced as before; one with a line bash would run (POINTER_IGNORE written with
", ", "KEY = value", $(...)) no longer stops the desktop under set -eu: its valid settings load
and nothing in it runs. The script's own lines from conf= to backend= run here, with HOME a
temporary folder, so nothing else of the session starts.

  python3 session/tests/test_conf_load.py
"""
import os
import re
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(os.path.realpath(__file__))
SESSION = os.path.join(HERE, "..")
with open(os.path.join(SESSION, "frametop-session.sh")) as f:
    SCRIPT = f.read()
LOAD = re.search(r"^conf=\$HOME/\.config/frametop\.conf\n.*?(?=^backend=)", SCRIPT, re.MULTILINE | re.DOTALL).group(0)


def load(conf_text, here=SESSION):
    """(exit code, {BACKEND, SCREENS, FLOAT_SLOTS, POINTER_IGNORE}, stderr, files made in the cwd)."""
    with tempfile.TemporaryDirectory() as home, tempfile.TemporaryDirectory() as cwd:
        os.makedirs(os.path.join(home, ".config"))
        with open(os.path.join(home, ".config", "frametop.conf"), "w") as f:
            f.write(conf_text)
        script = ("set -eu\nhere=$1\n" + LOAD +
                  'printf "%s\\n" "$BACKEND" "$SCREENS" "$FLOAT_SLOTS" "${POINTER_IGNORE-}"\n')
        p = subprocess.run(["bash", "-c", script, "_", here], capture_output=True, text=True, cwd=cwd,
                           env=dict(os.environ, HOME=home), timeout=20, check=False)
        vals = dict(zip(("BACKEND", "SCREENS", "FLOAT_SLOTS", "POINTER_IGNORE"), p.stdout.split("\n")))
        return p.returncode, vals, p.stderr, os.listdir(cwd)


class ConfLoad(unittest.TestCase):
    def test_example_sources(self):
        with open(os.path.join(SESSION, "frametop.conf.example")) as f:
            code, vals, err, _ = load(f.read())
        self.assertEqual((code, vals["BACKEND"], vals["FLOAT_SLOTS"], err), (0, "screens", "16", ""))

    def test_quoted_values(self):
        code, vals, err, made = load("SCREENS=3\nFLOAT_SLOTS=\"8\"\nPOINTER_IGNORE='a b,$(touch canary)'\n")
        self.assertEqual(code, 0, err)
        self.assertEqual((vals["SCREENS"], vals["FLOAT_SLOTS"], vals["POINTER_IGNORE"]), ("3", "8", "a b,$(touch canary)"))
        self.assertEqual(made, [])

    def test_bad_lines_dont_stop_or_run(self):
        code, vals, err, made = load("BACKEND=gamescope\nPOINTER_IGNORE=a.b, touch   # the old writer\n"
                                     "HANDS_NEAR = 0\nSCREENS=$(touch canary)\nFLOAT_SLOTS=4\n")
        self.assertEqual(code, 0, err)
        self.assertEqual(made, [], "a line of the conf ran")
        self.assertEqual((vals["BACKEND"], vals["SCREENS"], vals["FLOAT_SLOTS"], vals["POINTER_IGNORE"]),
                         ("gamescope", "2", "4", ""))
        self.assertIn("POINTER_IGNORE left out", err)
        self.assertIn("HANDS_NEAR left out", err)
        self.assertIn("SCREENS left out", err)

    def test_names_bash_keeps(self):
        # UID is read-only (setting it stopped the session under set -e); here is the script's own
        code, vals, err, _ = load("UID=5\nhere=/nowhere\nIFS=x\nSCREENS=3\n")
        self.assertEqual((code, vals["SCREENS"]), (0, "3"), err)
        self.assertIn("UID left out", err)
        self.assertIn("here left out", err)

    def test_check_that_cant_run_sources(self):
        # no conf/ft_conf.py next to the script (exit 2), or one that fails (a Python error, exit 1):
        # the file is sourced as before, said once
        for broken in (None, "raise RuntimeError('a bug')\n"):
            with tempfile.TemporaryDirectory() as tree:
                if broken:
                    os.makedirs(os.path.join(tree, "conf"))
                    with open(os.path.join(tree, "conf", "ft_conf.py"), "w") as f:
                        f.write(broken)
                code, vals, err, _ = load("SCREENS=3\nFLOAT_SLOTS='8'\n", os.path.join(tree, "session"))
            self.assertEqual((code, vals["SCREENS"], vals["FLOAT_SLOTS"]), (0, "3", "8"), err)
            self.assertIn("couldn't check", err)

    def test_missing_conf(self):
        with tempfile.TemporaryDirectory() as home:
            p = subprocess.run(["bash", "-c", "set -eu\nhere=$1\n" + LOAD + 'echo "$BACKEND"', "_", SESSION],
                               capture_output=True, text=True, env=dict(os.environ, HOME=home), check=False)
        self.assertEqual((p.returncode, p.stdout), (0, "screens\n"))


if __name__ == "__main__":
    unittest.main()
