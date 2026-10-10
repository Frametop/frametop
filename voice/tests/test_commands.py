"""Voice command matching and the settings round trip. Run: python3 -m unittest"""
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import voice_commands as vc  # noqa: E402

FLAG = {"say": ["flag that", "flag this", "flag it", "flag"], "run": ["~/.local/bin/tool", "flag"]}
TOOL = os.path.expanduser("~/.local/bin/tool")


def parsed(*entries):
    commands, warnings = vc.parse(list(entries))
    return commands


class Normalise(unittest.TestCase):
    def test_case_punctuation_space(self):
        self.assertEqual(vc.normalise("  Flag   that,  fingers-crossed! "), "flag that fingers crossed")
        self.assertEqual(vc.normalise("..."), "")


class Matching(unittest.TestCase):
    def setUp(self):
        self.commands = parsed(FLAG)

    def check(self, text, phrase, rest, argv):
        m = vc.match(text, self.commands)
        self.assertIsNotNone(m, text)
        self.assertEqual((m.phrase, m.rest, m.argv), (phrase, rest, argv))

    def test_whisper_forms(self):
        self.check("Flag that.", "flag that", "", [TOOL, "flag"])
        self.check(" flag that", "flag that", "", [TOOL, "flag"])
        self.check("FLAG THAT!", "flag that", "", [TOOL, "flag"])

    def test_rest_is_one_argument_with_user_words(self):
        self.check("Flag that, fingers crossed.", "flag that", "fingers crossed",
                   [TOOL, "flag", "fingers crossed"])
        self.check("Flag that: the left hand is off...", "flag that", "the left hand is off",
                   [TOOL, "flag", "the left hand is off"])
        self.check("Flag that, is it wrong?", "flag that", "is it wrong?", [TOOL, "flag", "is it wrong?"])

    def test_longest_phrase_wins(self):
        self.check("Flag it.", "flag it", "", [TOOL, "flag"])
        self.check("Flag, the pinch.", "flag", "the pinch", [TOOL, "flag", "the pinch"])
        long = parsed(FLAG, {"say": ["flag that hand"], "run": ["other"]})
        self.assertEqual(vc.match("Flag that hand now.", long).argv, ["other", "now"])
        self.assertEqual(vc.match("Flag that, now.", long).argv, [TOOL, "flag", "now"])

    def test_ties_go_to_first_entry(self):
        both = parsed({"say": ["go"], "run": ["a"]}, {"say": ["Go!"], "run": ["b"]})
        self.assertEqual(vc.match("go", both).argv, ["a"])

    def test_no_match(self):
        for text in ("Flagging that.", "Please flag that.", "", "fla", "Type hello world."):
            self.assertIsNone(vc.match(text, self.commands), text)
        self.assertIsNone(vc.match("flag", []))

    def test_rest_none(self):
        cmds = parsed({**FLAG, "rest": "none"})
        m = vc.match("Flag that, fingers crossed.", cmds)
        self.assertEqual((m.rest, m.argv), ("fingers crossed", [TOOL, "flag"]))

    def test_shell_text_stays_one_argument(self):
        m = vc.match("flag that; rm -rf $HOME `x`", self.commands)
        self.assertEqual(m.argv, [TOOL, "flag", "rm -rf $HOME `x`"])


class Parsing(unittest.TestCase):
    def test_malformed_entries_are_skipped_with_warnings(self):
        entries = [
            "flag",
            {"run": ["x"]},
            {"say": "flag", "run": "x --y"},
            {"say": ["flag"], "run": []},
            {"say": ["flag"], "run": ["", "x"]},
            {"say": ["flag"], "run": ["x", 3]},
            {"say": [1, None], "run": ["x"]},
            {"say": ["...", " "], "run": ["x"]},
            {"say": ["flag"], "run": ["x"], "rest": "all"},
            {"say": "single", "run": ["ok"]},
        ]
        commands, warnings = vc.parse(entries)
        self.assertEqual(len(commands), 1)
        self.assertEqual(len(warnings), len(entries) - 1)
        self.assertEqual(vc.match("Single.", commands).argv, ["ok"])

    def test_not_a_list(self):
        self.assertEqual(vc.parse(None), ([], []))
        commands, warnings = vc.parse({"say": ["x"]})
        self.assertEqual((commands, len(warnings)), ([], 1))


class SettingsRoundTrip(unittest.TestCase):
    def test_app_save_keeps_commands_and_unknown_keys(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "settings.json"
            path.write_text(json.dumps({"wake_phrases": ["HEY COMPUTER"], "commands": [FLAG], "extra": 1}))
            os.environ["FRAME_VOICE_SETTINGS"] = str(path)
            sys.modules.pop("settings", None)
            import settings
            try:
                data = settings.load()
                self.assertEqual(data["commands"], [FLAG])
                data["sounds"] = True
                data["commands"] = []  # a stale copy must not overwrite the file's list
                settings.save(data)
                saved = json.loads(path.read_text())
                self.assertEqual((saved["commands"], saved["extra"], saved["sounds"]), ([FLAG], 1, True))
                path.write_text("{}")
                self.assertEqual(settings.load()["commands"], [])
            finally:
                del os.environ["FRAME_VOICE_SETTINGS"]
                sys.modules.pop("settings", None)


if __name__ == "__main__":
    unittest.main()
