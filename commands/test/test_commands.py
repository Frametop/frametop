"""Frametop commands (commands/ft_commands.py, ft-cmd): names, the list, the aliases file,
~/.bashrc's lines, the CLI link, and running in the background. Standard library only; runs in
a temporary home. Run: python3 commands/test/test_commands.py"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
import warnings
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import ft_commands as fc  # noqa: E402

FT_CMD = str(HERE.parent / "ft-cmd")


class Home(unittest.TestCase):
    """Each test gets its own home: the module's paths point into it."""

    def setUp(self):
        self.home = tempfile.mkdtemp(prefix="ft-commands-test-")
        self.saved = {k: getattr(fc, k) for k in ("COMMANDS_PATH", "ALIASES_PATH", "BASHRC", "CLI_LINK")}
        fc.COMMANDS_PATH = os.path.join(self.home, ".config/frametop-commands.json")
        fc.ALIASES_PATH = os.path.join(self.home, ".config/frametop/aliases.sh")
        fc.BASHRC = os.path.join(self.home, ".bashrc")
        fc.CLI_LINK = os.path.join(self.home, ".local/bin/ft-cmd")

    def tearDown(self):
        for k, v in self.saved.items():
            setattr(fc, k, v)
        shutil.rmtree(self.home)

    def read(self, path):
        with open(path) as f:
            return f.read()


class Names(unittest.TestCase):
    def test_good(self):
        for name in ("stop-framecorder", "fc.stop", "a", "x_1", "A9"):
            self.assertEqual(fc.name_problem(name, path=""), "", name)

    def test_bad_shape(self):
        for name in ("", "has space", "-dash", ".dot", "a/b", "a;b", "x" * 41, "é", None):
            self.assertIn("letters, digits", fc.name_problem(name, path=""), name)

    def test_taken_shell_reserved_and_programs(self):
        self.assertIn("already", fc.name_problem("x", {"x"}, path=""))
        self.assertIn("shell word", fc.name_problem("cd", path=""))
        self.assertIn("ft-cmd's own words", fc.name_problem("list", path=""))
        self.assertIn("runs the commands", fc.name_problem("ft-cmd", path=""))
        self.assertIn("on your PATH", fc.name_problem("sh", path="/bin:/usr/bin"))

    def test_spoken(self):
        self.assertEqual(fc.spoken("stop-framecorder"), "stop framecorder")
        self.assertEqual(fc.spoken("My_Tool.v2"), "my tool v2")


class ListFile(Home):
    def test_parse_drops_bad_entries(self):
        out, warnings = fc.parse({"commands": [
            {"name": "ok", "run": " true "}, {"name": "ok", "run": "again"}, {"name": "bad name", "run": "x"},
            {"name": "empty", "run": "  "}, "nope"]})
        self.assertEqual(out, [{"name": "ok", "run": "true"}])
        self.assertEqual(len(warnings), 4)
        self.assertEqual(fc.parse({"commands": "x"})[0], [])
        self.assertEqual(fc.parse({}), ([], []))

    def test_save_load_round_trip(self):
        self.assertEqual(fc.load(), [])
        fc.save([{"name": "a", "run": "echo 'hi'"}])
        self.assertEqual(fc.load(), [{"name": "a", "run": "echo 'hi'"}])
        self.assertEqual(json.loads(self.read(fc.COMMANDS_PATH)), {"commands": [{"name": "a", "run": "echo 'hi'"}]})

    def test_unreadable_file_is_empty(self):
        os.makedirs(os.path.dirname(fc.COMMANDS_PATH))
        with open(fc.COMMANDS_PATH, "w") as f:
            f.write("{not json")
        self.assertEqual(fc.load(), [])


class Aliases(Home):
    def test_aliases_file_quotes_and_runs_ft_cmd(self):
        fc.save([{"name": "stop-fc", "run": "killall framecorder"}, {"name": "q", "run": "echo 'x'"}])
        text = self.read(fc.ALIASES_PATH)
        self.assertIn(f"alias stop-fc='{fc.CLI_LINK} stop-fc'", text)
        self.assertIn(f"alias q='{fc.CLI_LINK} q'", text)
        self.assertNotIn("killall", text)  # the alias runs ft-cmd, which reads the list
        # bash reads it without complaint, and the alias is defined
        r = subprocess.run(["bash", "-c", f". {fc.ALIASES_PATH} && alias stop-fc"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("ft-cmd stop-fc", r.stdout)

    def test_bashrc_lines_once_and_removed(self):
        with open(fc.BASHRC, "w") as f:
            f.write("export A=1")  # no newline at the end
        fc.save([{"name": "a", "run": "true"}])
        fc.save([{"name": "a", "run": "true"}, {"name": "b", "run": "true"}])
        text = self.read(fc.BASHRC)
        self.assertEqual(text.count(fc.BASHRC_MARK), 1)
        self.assertTrue(text.startswith("export A=1\n"))
        self.assertTrue(fc.remove_bashrc_lines())
        self.assertEqual(self.read(fc.BASHRC), "export A=1\n")
        self.assertFalse(fc.remove_bashrc_lines())

    def test_no_bashrc_lines_without_commands(self):
        fc.save([])
        self.assertFalse(os.path.exists(fc.BASHRC))

    def test_bashrc_sources_aliases(self):
        fc.save([{"name": "hello-there", "run": "true"}])
        r = subprocess.run(["bash", "-c", f"HOME={self.home}; shopt -s expand_aliases; . {fc.BASHRC}; alias hello-there"],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_cli_link(self):
        self.assertTrue(fc.link_cli())
        self.assertEqual(os.readlink(fc.CLI_LINK), fc.FT_CMD)
        self.assertFalse(fc.link_cli())
        os.remove(fc.CLI_LINK)
        with open(fc.CLI_LINK, "w") as f:
            f.write("mine")
        self.assertFalse(fc.link_cli())  # someone's own file stays
        self.assertEqual(self.read(fc.CLI_LINK), "mine")


class Running(Home):
    def test_unit_names(self):
        self.assertEqual(fc.unit_name("stop-fc", now=12.3456), "frametop-cmd-stop-fc-12345")

    def test_plain_process_fallback(self):
        """Without systemd-run it starts in its own session, Frametop's tools on its PATH."""
        out = os.path.join(self.home, "out")
        warnings.simplefilter("ignore", ResourceWarning)  # the detached process is never waited for
        real = shutil.which
        fc.shutil.which = lambda name, *a, **k: None if name == "systemd-run" else real(name, *a, **k)
        try:
            how = fc.run(f'echo "$FRAMETOP_COMMAND $PATH" > {out}', "t", env={"PATH": "/usr/bin:/bin"})
        finally:
            fc.shutil.which = real
        self.assertTrue(how.startswith("process "), how)
        for _ in range(50):
            if os.path.exists(out) and self.read(out).strip():
                break
            time.sleep(0.05)
        name, path = self.read(out).split(" ", 1)
        self.assertEqual(name, "t")
        self.assertIn(os.path.join(fc.REPO, "layout"), path)
        self.assertTrue(path.strip().endswith("/usr/bin:/bin"))

    def test_run_named_unknown(self):
        ok, message = fc.run_named("nothing")
        self.assertFalse(ok)
        self.assertIn("no command called nothing", message)


class Cli(Home):
    def cli(self, *args):
        env = dict(os.environ, HOME=self.home)
        return subprocess.run([sys.executable, FT_CMD, *args], capture_output=True, text=True, env=env)

    def test_add_list_remove(self):
        # ft-cmd uses the real paths, so give it this home through HOME
        r = self.cli("add", "my-thing", "echo", "hi")
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.cli("list")
        self.assertIn("my-thing: echo hi", r.stdout)
        self.assertEqual(self.cli("add", "my-thing", "x").returncode, 1)
        self.assertEqual(self.cli("add", "has space", "x").returncode, 1)
        self.assertIn(fc.BASHRC_MARK, self.read(os.path.join(self.home, ".bashrc")))
        self.assertEqual(os.readlink(os.path.join(self.home, ".local/bin/ft-cmd")), fc.FT_CMD)
        self.assertEqual(self.cli("remove", "my-thing").returncode, 0)
        self.assertIn("No commands yet", self.cli("list").stdout)
        self.assertEqual(self.cli("remove", "my-thing").returncode, 1)
        self.assertEqual(self.cli("nothing-here").returncode, 1)
        self.assertEqual(self.cli().returncode, 2)


if __name__ == "__main__":
    unittest.main()
