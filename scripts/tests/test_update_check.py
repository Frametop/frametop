#!/usr/bin/env python3
"""scripts/update-check.py: what it says about the KWin effects the session turns off.

KWin 6.2 has a blur and a contrast effect, each with a factory named in kwin_wayland. KWin 6.6
(SteamOS 0.5.8) has no contrast effect of its own, which is not something to warn about; a
KWin without the blur effect is.

  python3 scripts/tests/test_update_check.py
"""
import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def load():
    spec = importlib.util.spec_from_file_location("update_check_kwin", ROOT / "scripts/update-check.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


BLUR, CONTRAST = b"\0KWin::blur_factory\0", b"\0KWin::contrast_factory\0"


class KWinEffects(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.m = load()

    def test_kwin_6_2_has_both(self):
        state, label, detail = self.m.kwin_effects(b"x" + BLUR + CONTRAST)
        self.assertEqual((state, label), ("ok", "KWin effects"))
        self.assertIn("blur and contrast", detail)

    def test_kwin_6_6_without_a_contrast_effect_is_fine(self):
        # 6.6.2 still names the protocol (ContrastManagerInterface), not an effect.
        state, _, detail = self.m.kwin_effects(b"KWin::ContrastManagerInterface" + BLUR)
        self.assertEqual(state, "ok")
        self.assertIn("no contrast effect", detail)

    def test_no_blur_effect_warns(self):
        for blob, named in ((CONTRAST, "no built-in blur effect"), (b"", "no built-in blur, contrast effect")):
            with self.subTest(named=named):
                state, _, detail = self.m.kwin_effects(blob)
                self.assertEqual(state, "warn")
                self.assertIn(named, detail)


if __name__ == "__main__":
    unittest.main()
