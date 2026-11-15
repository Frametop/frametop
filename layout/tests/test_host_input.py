"""A host's "input" in the layout: which of its keyboard and mouse it lends (layout/host_input.py)."""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import host_input  # noqa: E402


class HostInput(unittest.TestCase):
    def test_what_each_value_lends(self):
        self.assertEqual(host_input.lent({"input": True}), {"keyboard", "mouse"})
        self.assertEqual(host_input.lent({"input": "keyboard"}), {"keyboard"})
        self.assertEqual(host_input.lent({"input": "mouse"}), {"mouse"})
        self.assertEqual(host_input.lent({"input": False}), set())
        self.assertEqual(host_input.lent({}), set())

    def test_a_garbled_value_lends_nothing(self):
        # As ft-stream reads it: nothing is taken from the computer on a guess.
        for garbled in ("Keyboard", "both", "on", 1, ["keyboard"], {"keyboard": True}, None):
            self.assertEqual(host_input.lent({"input": garbled}), set(), garbled)

    def test_the_value_for_kinds(self):
        self.assertIs(host_input.value({"keyboard", "mouse"}), True)
        self.assertEqual(host_input.value(["keyboard"]), "keyboard")
        self.assertEqual(host_input.value({"mouse"}), "mouse")
        self.assertIs(host_input.value(()), False)
        with self.assertRaises(ValueError):
            host_input.value({"trackpad"})

    def test_round_trip(self):
        for kinds in ({"keyboard", "mouse"}, {"keyboard"}, {"mouse"}, set()):
            self.assertEqual(host_input.lent({"input": host_input.value(kinds)}), kinds)
            self.assertEqual(host_input.WORDS[host_input.word(kinds)], kinds)


if __name__ == "__main__":
    unittest.main()
