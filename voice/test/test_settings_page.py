"""Frametop Input Settings' Voice page: dictation keys' tap or hold, the language, and the
whisper model (a missing one is fetched first). HOME is a temporary directory and nothing
reaches the relay, systemd or the network. Needs PySide6 (the image's python3).
Run: python3 voice/test/test_settings_page.py"""
import json
import os
import sys
import tempfile
import time
import unittest
from pathlib import Path

HOME = tempfile.mkdtemp()
os.environ["HOME"] = HOME
os.environ.pop("FRAMETOP_VOICE_SETTINGS", None)
os.environ["FRAMETOP_VOICE_MODELS"] = f"{HOME}/models"
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "input-settings"))
import ft_input_settings as fis  # noqa: E402
import voice_settings  # noqa: E402  (ft_input_settings put voice/ on the path)
from PySide6.QtCore import QCoreApplication  # noqa: E402

RULES = Path(fis.RULES_PATH)
Path(HOME, ".config/systemd/user").mkdir(parents=True)
Path(HOME, ".config/systemd/user", fis.VOICE_UNIT).touch()  # Frametop Voice is installed


def install(name):
    path = voice_settings.model_path(name)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"model")
    Path(f"{path}.sha256").write_text("sum\n")


class Page(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QCoreApplication.instance() or QCoreApplication([])

    def setUp(self):
        for p in (RULES, voice_settings.PATH):
            p.unlink(missing_ok=True)
        for f in Path(HOME, "models").glob("*") if Path(HOME, "models").exists() else ():
            f.unlink()
        install("small.en")
        self.ran, self.sent, self.messages = [], [], []
        fis.host = self.fake_host
        self.b = fis.Backend()
        self.b._send = self.sent.append
        self.b.message.connect(lambda text, err: self.messages.append((text, err)))

    def fake_host(self, *cmd, timeout=20):
        if cmd[1:2] == ("model",):
            self.ran.append(cmd)
            install(cmd[2])
        return fis.subprocess.CompletedProcess(cmd, 0, "", "")

    def wait_fetch(self):
        for _ in range(200):
            self.app.processEvents()
            if not self.b.voiceModelFetching:
                break
            time.sleep(0.01)
        self.app.processEvents()

    def test_key_mode_default_is_tap(self):
        self.assertEqual(self.b.dictationKeyMode, "dictate")
        self.assertEqual([k["label"] for k in self.b.dictationKeys], ["Right Ctrl tap"])

    def test_hold_rewrites_only_dictation_keys(self):
        self.b.setDictationKeyMode("dictate_hold")
        bound = json.loads(RULES.read_text())["key_bindings"]
        self.assertEqual(bound["97"], "dictate_hold")
        self.assertEqual(bound["125"], "steam_menu")  # the defaults kept, now its own list
        self.assertEqual(self.b.dictationKeyMode, "dictate_hold")
        self.assertEqual([k["label"] for k in self.b.dictationKeys], ["Right Ctrl"])
        self.assertIn("reload", self.sent)
        self.b.setDictationKeyMode("dictate")
        self.assertEqual(json.loads(RULES.read_text())["key_bindings"]["97"], "dictate")

    def test_mixed_and_no_keys(self):
        RULES.parent.mkdir(parents=True, exist_ok=True)
        RULES.write_text(json.dumps({"key_bindings": {"97": "dictate", "125+32": "dictate_hold"}}))
        self.assertEqual(self.b.dictationKeyMode, "")
        RULES.write_text(json.dumps({"key_bindings": {"125": "steam_menu"}}))
        self.b.setDictationKeyMode("dictate_hold")
        self.assertEqual(json.loads(RULES.read_text())["key_bindings"], {"125": "steam_menu"})
        self.assertTrue(self.messages[-1][1])
        self.b.setDictationKeyMode("bogus")
        self.assertEqual(json.loads(RULES.read_text())["key_bindings"], {"125": "steam_menu"})

    def test_english_models_hide_for_other_languages(self):
        self.assertIn("small.en", [m["value"] for m in self.b.voiceModels])
        install("small")
        self.b.setVoiceLanguage("de")
        self.assertEqual(voice_settings.load()["language"], "de")
        self.assertNotIn("small.en", [m["value"] for m in self.b.voiceModels])
        self.assertEqual(self.b.voiceModel, "small")  # small.en, stored, means small for German
        self.assertEqual(self.ran, [])  # already there: nothing fetched

    def test_language_fetches_its_model(self):
        self.b.setVoiceLanguage("fr")
        self.wait_fetch()
        self.assertEqual(self.ran, [(str(ROOT / "voice/models.sh"), "model", "small")])
        self.assertTrue(voice_settings.model_installed("small"))
        self.b.setVoiceLanguage("xx")  # not one of LANGUAGES
        self.assertEqual(voice_settings.load()["language"], "fr")

    def test_model_chosen_after_its_download(self):
        self.b.setVoiceModel("large-v3-turbo")
        self.assertEqual(voice_settings.load()["model"], "small.en")  # not until it's there
        self.wait_fetch()
        self.assertEqual(voice_settings.load()["model"], "large-v3-turbo")
        self.assertIn(("Whisper model: large-v3-turbo, downloaded", False), self.messages)

    def test_failed_download_keeps_the_model(self):
        fis.host = lambda *cmd, timeout=20: fis.subprocess.CompletedProcess(cmd, 1, "", "curl: (6) no host")
        self.messages.clear()
        self.b.setVoiceModel("small")
        self.wait_fetch()
        self.assertEqual(voice_settings.load()["model"], "small.en")
        self.assertIn(("Couldn't download whisper small: curl: (6) no host", True), self.messages)


if __name__ == "__main__":
    unittest.main()
