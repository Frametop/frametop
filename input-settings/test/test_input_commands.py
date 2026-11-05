"""input_commands: the Commands page's logic without Qt: the list, the name check (here and on the
host), adding, the bindings a removed command takes with it, and running one. HOME is a
temporary directory and the host runner is a fake, so nothing runs on the host.
Run: python3 input-settings/test/test_input_commands.py"""
import os
import sys
import tempfile
import unittest
from pathlib import Path

os.environ["HOME"] = tempfile.mkdtemp()
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import input_commands  # noqa: E402
import ft_commands  # noqa: E402  (input_commands put commands/ on the path)


class Done:
    def __init__(self, code=0, out="", err=""):
        self.returncode, self.stdout, self.stderr = code, out, err


class Host:
    """A fake host runner: records each command, answers with `answer`."""
    def __init__(self, answer=None):
        self.ran, self.answer = [], answer or Done(1)

    def __call__(self, *cmd, timeout=20):
        self.ran.append((cmd, timeout))
        return self.answer


class Commands(unittest.TestCase):
    def setUp(self):
        Path(ft_commands.COMMANDS_PATH).parent.mkdir(parents=True, exist_ok=True)
        Path(ft_commands.COMMANDS_PATH).write_text('{"commands": []}')

    def test_add_lists_it_with_how_to_say_it(self):
        saved, text = input_commands.add("  backup-notes ", " rsync -a a b ", lambda n: "")
        self.assertTrue(saved)
        said = ft_commands.spoken("backup-notes")
        self.assertEqual(text, f'backup-notes saved: say "run {said}", search it in frunner, or map it to a button')
        listed = input_commands.listed()
        self.assertEqual([(c["name"], c["run"], c["said"]) for c in listed], [("backup-notes", "rsync -a a b", said)])

    def test_add_refuses_a_problem_or_nothing_to_run(self):
        self.assertEqual(input_commands.add("x", "ls", lambda n: "taken"), (False, "taken"))
        self.assertEqual(input_commands.add("x", "  ", lambda n: ""), (False, "what should it run?"))
        self.assertEqual(input_commands.listed(), [])

    def test_add_says_when_the_file_does_not_save(self):
        Path(ft_commands.COMMANDS_PATH).write_text("{not json")
        saved, text = input_commands.add("x", "ls", lambda n: "")
        self.assertFalse(saved)
        self.assertTrue(text.startswith("Couldn't save x: "), text)

    def test_name_problem_here_then_on_the_host(self):
        self.assertTrue(input_commands.add("mine", "ls", lambda n: "")[0])
        host = Host(Done(0))
        self.assertEqual(input_commands.name_problem(" mine ", host),
                         ft_commands.name_problem("mine", {"mine"}))
        self.assertEqual(host.ran, [])   # a problem here: the host isn't asked
        self.assertEqual(input_commands.name_problem("htop", host),
                         "htop is a program on the headset; an alias by that name would hide it")
        self.assertEqual(host.ran, [(("sh", "-c", 'command -v -- "$1"', "sh", "htop"), 5)])
        self.assertEqual(input_commands.name_problem("free-name", Host(Done(1))), "")


class Bindings(unittest.TestCase):
    def test_drop_bindings_takes_every_binding_to_it(self):
        rules = {"controller_buttons": {"right/a": "run:x", "right/b": "run:y"},
                 "key_bindings": {"125+30": "run:x", "125": "steam_menu"},
                 "buttons": {"dev1": {"272": "run:x"}, "dev2": "not a table", "dev3": {"273": "run:xx"}},
                 "other": {"k": "run:x"}}
        self.assertEqual(input_commands.drop_bindings(rules, "x"), 3)
        self.assertEqual(rules["controller_buttons"], {"right/b": "run:y"})
        self.assertEqual(rules["key_bindings"], {"125": "steam_menu"})
        self.assertEqual(rules["buttons"], {"dev1": {}, "dev2": "not a table", "dev3": {"273": "run:xx"}})
        self.assertEqual(rules["other"], {"k": "run:x"})
        self.assertEqual(input_commands.drop_bindings({}, "x"), 0)

    def test_removed_text(self):
        self.assertEqual(input_commands.removed_text("x", 0), "x removed")
        self.assertEqual(input_commands.removed_text("x", 1), "x removed, and 1 binding to it")
        self.assertEqual(input_commands.removed_text("x", 2), "x removed, and 2 bindings to it")


class RunNow(unittest.TestCase):
    def test_runs_the_host_link_when_it_is_there(self):
        link = Path(tempfile.mkdtemp()) / "ft-cmd"
        link.write_text("")
        host = Host(Done(0, "x: started (unit frametop-cmd-x-1)\n"))
        self.assertEqual(input_commands.run_now("x", host, str(link)), ("x: started (unit frametop-cmd-x-1)", False))
        self.assertEqual(host.ran, [((str(link), "run", "x"), 15)])

    def test_else_the_checkout_one_and_its_errors(self):
        host = Host(Done(1, "", "no such command\n"))
        self.assertEqual(input_commands.run_now("x", host, "/nonexistent/ft-cmd"), ("no such command", True))
        self.assertEqual(host.ran[0][0][0], ft_commands.FT_CMD)
        self.assertEqual(input_commands.run_now("x", Host(Done(0)), "/nonexistent/ft-cmd"), ("x: started", False))


if __name__ == "__main__":
    unittest.main()
