#!/usr/bin/env python3
"""conf/ft_remote_keys.py: the key combinations a remote display's computer gets. What a rules
file without the table gets, which pressed sides the relay can see, and which entries of a
table of its own are kept. Then what it and the relay share from conf/ft_input_actions.py: a
combination's key codes, a "keys:" action's, and the release_keys combination a key completes.
Nothing is read from a file."""
import os
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "conf"))
import ft_remote_keys as rk  # noqa: E402
import ft_input_actions as actions  # noqa: E402
from ft_input_actions import DEFAULT_KEY_BINDINGS  # noqa: E402


def sendable(combo):
    """A stand-in for the relay's limit: anything but a combination with the volume-up key."""
    return "115" not in combo.split("+")


class Defaults(unittest.TestCase):
    def test_the_gaze_clicks_stay_frametops(self):
        self.assertNotIn("125+36", rk.DEFAULT)   # Meta+J
        self.assertNotIn("125+37", rk.DEFAULT)   # Meta+K

    def test_frametops_other_meta_combinations_go_as_they_are(self):
        for combo in ("125", "125+57", "125+17", "125+35", "42+125+33", "42+125+35", "42+125+32", "56+125+15"):
            self.assertEqual(rk.DEFAULT.get(combo), combo, combo)

    def test_no_tiling_keys_and_nothing_without_meta(self):
        self.assertNotIn("42+125+105", rk.DEFAULT)   # Meta+Shift+Left: a tile's while it has the focus
        self.assertNotIn("29+42+56+1", rk.DEFAULT)   # Ctrl+Alt+Shift+Esc: the way out of stuck keys
        for combo in rk.DEFAULT:
            self.assertIn(combo, DEFAULT_KEY_BINDINGS)
            self.assertIn("125", combo.split("+"))

    def test_a_file_without_the_table_gets_a_copy_of_them(self):
        for rules in ({}, {"remote_key_bindings": None}, {"remote_key_bindings": ["125+57"]}, None):
            got = rk.read(rules, sendable)
            self.assertEqual(got, dict(rk.DEFAULT))
            got.clear()
        self.assertTrue(rk.DEFAULT)


class Pressed(unittest.TestCase):
    def test_what_the_relay_can_see(self):
        for combo in ("125", "97", "183", "125+57", "42+125+57", "29+42+56+125+1"):
            self.assertTrue(rk.pressed_ok(combo), combo)

    def test_what_it_never_sees(self):
        for combo in ("", "125+", "57+125", "125+42+57", "126+57", "125+125+57", "125+42", "a+57", "125+300",
                      "0", "125 + 57", None, 125):
            self.assertFalse(rk.pressed_ok(combo), repr(combo))


class Read(unittest.TestCase):
    def test_an_own_table_is_the_whole_list_even_empty(self):
        self.assertEqual(rk.read({"remote_key_bindings": {}}, sendable), {})
        self.assertEqual(rk.read({"remote_key_bindings": {"42+125+57": "125+57"}}, sendable), {"42+125+57": "125+57"})

    def test_entries_the_relay_cant_use_are_left_out(self):
        table = {"125+57": "125+57", "57+125": "125+57", "125+36": "125+115", "125+37": 37, "42+125+57": "125+57"}
        self.assertEqual(rk.read({"remote_key_bindings": table}, sendable),
                         {"125+57": "125+57", "42+125+57": "125+57"})

    def test_a_number_too_long_to_read_is_no_key(self):
        # (int() refuses a decimal string past 4300 digits: a hand-written file mustn't stop the relay)
        huge = "1" * 5000
        asked = []
        table = {huge: "57", "125+" + huge: "125+57", "125+57": huge, "125+36": "125+" + huge, "42+125+57": "125+57"}
        self.assertEqual(rk.read({"remote_key_bindings": table}, lambda sent: asked.append(sent) or True),
                         {"42+125+57": "125+57"})
        self.assertEqual(asked, ["125+57"], "a sent side that isn't key codes isn't asked about")
        self.assertFalse(rk.pressed_ok(huge))

    def test_a_combination_sent_as_itself_isnt_asked_about(self):
        self.assertEqual(rk.read({"remote_key_bindings": {"125+115": "125+115"}}, lambda combo: False),
                         {"125+115": "125+115"})


class Shared(unittest.TestCase):
    HUGE = "9" * 5000  # (int() refuses a decimal string past 4300 digits)

    def test_a_combinations_key_codes(self):
        self.assertEqual(actions.combo_codes("29+125+106"), [29, 125, 106])
        for no in ("", "a", "0", "256", "125+", "１", self.HUGE, None, 57):
            self.assertIsNone(actions.combo_codes(no), no if no != self.HUGE else "a number too long to read")

    def test_a_keys_actions_key_codes(self):
        self.assertEqual(actions.keys_codes("keys:125+32"), [125, 32])
        self.assertEqual(actions.keys_codes("keys:1+2+3+4+5+6"), [1, 2, 3, 4, 5, 6])
        for no in ("keys:", "125+32", "keys:1+2+3+4+5+6+7", "keys:125+115", "keys:116", "keys:" + self.HUGE, None):
            self.assertIsNone(actions.keys_codes(no), no if no and len(no) < 99 else "a number too long to read")

    def test_a_name_for_a_number_too_long_to_read(self):
        self.assertEqual(actions.combo_name(self.HUGE), self.HUGE)
        self.assertEqual(actions.combo_name("29+125+106"), "Ctrl+Meta+Right")

    def test_the_release_keys_combination_a_key_completes(self):
        bound = {"29+42+56+1": "release_keys", "125+57": "frunner", "88": "release_keys"}
        self.assertEqual(actions.release_combo(bound, {29, 42, 56}, 1), "29+42+56+1")
        self.assertEqual(actions.release_combo(bound, {29, 42, 56, 125}, 1), "29+42+56+1", "more modifiers held")
        self.assertEqual(actions.release_combo(bound, ["29", "42", "56"], "1"), "29+42+56+1", "as a rules file writes them")
        self.assertIsNone(actions.release_combo(bound, {29, 42}, 1))
        self.assertIsNone(actions.release_combo(bound, {29, 42, 56}, 57))
        self.assertIsNone(actions.release_combo(bound, {125}, 88), "a key bound alone isn't one with modifiers held")


if __name__ == "__main__":
    unittest.main()
