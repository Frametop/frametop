"""input_rules: Input Settings' reads and writes of the relay's rules file and frametop.conf,
without Qt. Files are in a temporary directory. Run: python3 input-settings/test/test_input_rules.py"""
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

os.environ["HOME"] = tempfile.mkdtemp()
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import input_rules  # noqa: E402
import ft_store  # noqa: E402  (input_rules put conf/ on the path)


class ReadJson(unittest.TestCase):
    def setUp(self):
        self.path = Path(tempfile.mkdtemp()) / "frametop-input.json"

    def read(self, text):
        self.path.write_text(text)
        return input_rules.read_json(str(self.path))

    def test_missing_unreadable_and_not_an_object_read_as_empty(self):
        self.assertEqual(input_rules.read_json(str(self.path)), {})
        self.assertEqual(self.read("{"), {})
        self.assertEqual(self.read("[1, 2]"), {})
        self.assertEqual(self.read("3"), {})

    def test_renamed_actions_in_the_three_tables(self):
        got = self.read(json.dumps({
            "buttons": {"dev": {"272": "spotwatch", "273": "left"}},
            "controller_buttons": {"right/a": "spotwatch", "n": 3},
            "key_bindings": {"125": "spotwatch"},
            "other": {"x": "spotwatch"},
        }))
        self.assertEqual(got["buttons"], {"dev": {"272": "frunner", "273": "left"}})
        self.assertEqual(got["controller_buttons"], {"right/a": "frunner", "n": 3})
        self.assertEqual(got["key_bindings"], {"125": "frunner"})
        self.assertEqual(got["other"], {"x": "spotwatch"})   # only the action tables

    def test_a_table_that_is_not_an_object_is_left_as_it_is(self):
        self.assertEqual(self.read('{"key_bindings": ["spotwatch"]}')["key_bindings"], ["spotwatch"])


class SaveRules(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.path = self.dir / "frametop-input.json"

    def test_writes_the_whole_rules_indented(self):
        rules = {"devices": {"a": {"role": "pointer"}}, "pause_auto": False}
        input_rules.save_rules(rules, str(self.path))
        self.assertEqual(self.path.read_text(), json.dumps(rules, indent=2))
        self.assertEqual(input_rules.read_json(str(self.path)), rules)

    def test_an_empty_file_is_saved_over(self):
        self.path.write_text("  \n")
        input_rules.save_rules({"a": 1}, str(self.path))
        self.assertEqual(json.loads(self.path.read_text()), {"a": 1})

    def test_a_file_that_does_not_read_is_kept_with_a_copy(self):
        for bad in ("{not json", "[1]"):
            with self.subTest(bad=bad):
                self.path.write_text(bad)
                with self.assertRaises(ft_store.StoreError):
                    input_rules.save_rules({"a": 1}, str(self.path))
                self.assertEqual(self.path.read_text(), bad)
                copies = [p for p in self.dir.iterdir() if p.name.startswith("frametop-input.json.bad-")]
                self.assertTrue(copies)
                self.assertIn(bad, [p.read_text() for p in copies])


class Conf(unittest.TestCase):
    def test_write_keeps_comments_and_reads_back(self):
        path = Path(tempfile.mkdtemp()) / "frametop.conf"
        path.write_text("# mine\nPOINTER_FOLLOW=on\n")
        input_rules.write_conf_value("POINTER_DISTANCE", "1.5", str(path))
        input_rules.write_conf_value("POINTER_FOLLOW", "off", str(path))
        self.assertIn("# mine", path.read_text())
        conf = input_rules.read_conf(str(path))
        self.assertEqual((conf["POINTER_FOLLOW"], conf["POINTER_DISTANCE"]), ("off", "1.5"))


if __name__ == "__main__":
    unittest.main()
