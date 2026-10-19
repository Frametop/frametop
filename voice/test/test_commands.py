"""Frametop Voice's command matching, its settings round trip, and carrying over frame-voice's
settings. Standard library only. Run: python3 voice/test/test_commands.py"""
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


class Settings(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = Path(self.tmp.name) / "frametop-voice.json"
        os.environ["FRAMETOP_VOICE_SETTINGS"] = str(self.path)
        sys.modules.pop("voice_settings", None)
        import voice_settings
        self.vs = voice_settings

    def tearDown(self):
        del os.environ["FRAMETOP_VOICE_SETTINGS"]
        sys.modules.pop("voice_settings", None)
        self.tmp.cleanup()

    def test_app_save_keeps_commands_and_unknown_keys(self):
        self.path.write_text(json.dumps({"wake_phrases": ["HEY COMPUTER"], "commands": [FLAG], "extra": 1,
                                         "advanced": {"dictation": {"min_seconds": 1.0}}}))
        data = self.vs.load()
        self.assertEqual((data["commands"], data["advanced"]), ([FLAG], {"dictation": {"min_seconds": 1.0}}))
        data["sounds"] = True
        data["commands"] = []  # a stale copy must not overwrite the file's list
        self.vs.save(data)
        saved = json.loads(self.path.read_text())
        self.assertEqual((saved["commands"], saved["extra"], saved["sounds"]), ([FLAG], 1, True))
        self.assertEqual(saved["advanced"], {"dictation": {"min_seconds": 1.0}})
        self.path.write_text("{}")
        self.assertEqual(self.vs.load()["commands"], [])

    def test_defaults(self):
        data = self.vs.load()
        self.assertEqual((data["wake_word"], data["controller"], data["controller_mode"]),
                         (False, ["left/dpad_down"], "hold"))

    def test_bad_values_fall_back(self):
        self.path.write_text(json.dumps({"controller": ["left/nope", "right/a"], "controller_mode": "x",
                                         "wake_word": "yes", "wake_phrases": ["hey, scarlet!", 3, ""]}))
        data = self.vs.load()
        self.assertEqual((data["controller"], data["controller_mode"], data["wake_word"], data["wake_phrases"]),
                         (["right/a"], "hold", False, ["HEY SCARLET"]))

    def test_language_and_model(self):
        self.assertEqual((self.vs.load()["language"], self.vs.load()["model"]), ("en", "small.en"))
        self.path.write_text(json.dumps({"language": "de", "model": "small.en"}))
        self.assertEqual(self.vs.model_for(self.vs.load()), "small")  # small.en knows only English
        self.path.write_text(json.dumps({"language": "auto", "model": "large-v3-turbo"}))
        self.assertEqual(self.vs.model_for(self.vs.load()), "large-v3-turbo")
        self.path.write_text(json.dumps({"language": "German", "model": "huge"}))
        self.assertEqual((self.vs.load()["language"], self.vs.load()["model"]), ("en", "small.en"))
        self.path.write_text(json.dumps({"language": "eu"}))  # whisper's, though not on the page
        self.assertEqual(self.vs.load()["language"], "eu")

    def test_model_file_falls_back_until_fetched(self):
        os.environ["FRAMETOP_VOICE_MODELS"] = self.tmp.name
        try:
            sys.modules.pop("voice_settings", None)
            import voice_settings as vs
            self.path.write_text(json.dumps({"language": "fr"}))
            self.assertEqual(vs.model_file(), Path(self.tmp.name) / "ggml-small.en-q5_1.bin")
            (Path(self.tmp.name) / "ggml-small-q5_1.bin").write_bytes(b"x")
            self.assertFalse(vs.model_installed("small"))  # not checked yet: no .sha256
            (Path(self.tmp.name) / "ggml-small-q5_1.bin.sha256").write_text("sum\n")
            self.assertEqual(vs.model_file(), Path(self.tmp.name) / "ggml-small-q5_1.bin")
        finally:
            del os.environ["FRAMETOP_VOICE_MODELS"]

    def test_frame_voice_carried_over(self):
        old = {"keyboard": ["KEY_RIGHTCTRL"], "controller": [{"device": "/user/hand/left",
                                                              "input": "/input/dpad_left/click"}],
               "keyboard_mode": "toggle", "controller_mode": "hold", "live_typing": True, "sounds": True,
               "wake_word": True, "wake_phrases": ["HEY SCARLET"], "commands": [FLAG]}
        new, bindings, notes = self.vs.from_frame_voice(old)
        self.assertEqual(new, {"live_typing": True, "sounds": True, "wake_word": True, "wake_phrases": ["HEY SCARLET"],
                               "commands": [FLAG], "controller_mode": "hold", "controller": ["left/dpad_left"]})
        self.assertEqual((bindings, notes), ({"97": "dictate"}, []))
        new, bindings, notes = self.vs.from_frame_voice({"keyboard": ["KEY_F13", "BTN_SIDE"], "keyboard_mode": "hold",
                                                         "controller": [{"device": "/devices/cv/X", "input": "/input/a/click"}]})
        self.assertEqual((new["controller"], bindings, len(notes)), ([], {"183": "dictate_hold"}, 2))

    def test_dictation_keys_in_rules(self):
        rules = {}
        self.assertFalse(self.vs.set_dictation_keys(rules, {"97": "dictate"}))  # the defaults have it
        self.assertEqual(rules, {})
        self.assertTrue(self.vs.set_dictation_keys(rules, {"183": "dictate_hold"}))
        self.assertEqual(rules["key_bindings"], {**self.vs.DEFAULT_KEY_BINDINGS, "183": "dictate_hold"})
        rules = {"key_bindings": {"125": "steam_menu", "97": "dictate"}}
        self.assertFalse(self.vs.set_dictation_keys(rules, {"97": "dictate"}))
        self.assertTrue(self.vs.set_dictation_keys(rules, {}))
        self.assertEqual(rules["key_bindings"], {"125": "steam_menu"})

    def test_models_match_models_sh(self):
        src = (Path(__file__).resolve().parents[1] / "models.sh").read_text()
        extra = src[src.index("\nextra=("):src.index("\n)", src.index("\nextra=("))]
        fetched = dict(line.strip().strip('"').split()[:2] for line in extra.strip().splitlines()[1:])
        self.assertEqual(fetched, {n: f for n, (f, _, _) in self.vs.WHISPER_MODELS.items() if n != "small.en"})
        self.assertIn(self.vs.WHISPER_MODELS["small.en"][0], src)  # fetched with the others

    def test_defaults_match_the_relay(self):
        relay = Path(__file__).resolve().parents[2] / "input" / "input-relay.py"
        src = relay.read_text()
        for name in ("DEFAULT_KEY_BINDINGS", "VOICE_KEY_BINDINGS", "VR_BUTTONS"):
            ns = {}
            start = src.index(f"\n{name} = ") + 1
            end = src.index("\n", src.index("}" if name != "VR_BUTTONS" else ")", start))
            exec(src[start:end], ns)
            self.assertEqual(ns[name], getattr(self.vs, name), name)


if __name__ == "__main__":
    unittest.main()


class StripWake(unittest.TestCase):
    PHRASES = ["HEY COMPUTER"]

    def test_wake_then_command(self):
        self.assertEqual(vc.strip_wake("Hey, Computer. Flag that.", self.PHRASES), ("Flag that.", True))

    def test_misheard_wake_alone(self):
        text, had = vc.strip_wake("A Computer.", self.PHRASES)
        self.assertTrue(had)
        self.assertEqual(text.strip(" .!?,"), "")

    def test_command_only(self):
        self.assertEqual(vc.strip_wake("Flag that.", self.PHRASES), ("Flag that.", False))

    def test_wake_word_later_stays(self):
        self.assertEqual(vc.strip_wake("Flag that, Computer missed it", self.PHRASES)[1], False)
