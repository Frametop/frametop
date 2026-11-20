"""Frametop Settings' Quick Settings page (ft_quick_settings.py): the toggles are the same list, in the
same order, as the palm card's (screens/quicksettings.h), and picking them writes QUICK_SETTINGS as the
card reads it. No Qt window or VR needed."""
import os
import re
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "quicksettings"))
sys.path.insert(0, str(ROOT / "conf"))
import ft_conf  # noqa: E402
import ft_quick_settings as qs  # noqa: E402
from ft_input_actions import QUICK_SETTINGS_ACTIONS  # noqa: E402


class Catalogue(unittest.TestCase):
    def test_the_card_has_the_same_toggles(self):
        text = (ROOT / "screens/quicksettings.h").read_text()
        block = text[text.index("kAll[] = {"):text.index("};", text.index("kAll[] = {"))]
        card = re.findall(r'\{"([a-z_]+)", "([^"]+)", State::', block)
        self.assertEqual(card, list(QUICK_SETTINGS_ACTIONS))

    def test_the_relay_runs_them(self):
        relay = (ROOT / "input/input-relay.py").read_text()
        self.assertIn("QUICK_SETTINGS_NAMES", relay[relay.index('if cmd == "action"'):])
        actions = relay[relay.index("ACTIONS = ("):relay.index(")", relay.index("ACTIONS = ("))]
        for name, _ in QUICK_SETTINGS_ACTIONS:
            self.assertIn(f'"{name}"', actions, name)


class Picking(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.conf = os.path.join(tmp.name, "frametop.conf")
        Path(self.conf).write_text("POINTER=1   # kept\n")
        self.b = qs.Backend(conf=self.conf)

    def shown(self):
        return [t["name"] for t in self.b.toggles if t["on"]]

    def test_unset_shows_every_one(self):
        self.assertEqual(self.shown(), list(qs.NAMES))
        self.assertEqual([t["label"] for t in self.b.toggles], [label for _, label in QUICK_SETTINGS_ACTIONS])

    def test_turning_off_and_on(self):
        self.b.setShown("dictate", False)
        self.b.setShown("keyboard_toggle", False)
        self.assertEqual(ft_conf.read(self.conf)["QUICK_SETTINGS"], "screens_toggle,desktopless_toggle,hands_toggle,pause_toggle")
        self.assertEqual(ft_conf.read(self.conf)["POINTER"], "1")
        self.b.setShown("dictate", True)
        self.assertEqual(self.shown(), ["screens_toggle", "desktopless_toggle", "hands_toggle", "pause_toggle", "dictate"])

    def test_none_left_is_empty_not_unset(self):
        for name in qs.NAMES:
            self.b.setShown(name, False)
        self.assertEqual(ft_conf.read(self.conf)["QUICK_SETTINGS"], "")
        self.assertEqual(self.shown(), [])

    def test_unknown_names(self):
        ft_conf.set_value("QUICK_SETTINGS", "bogus,hands_toggle", self.conf)
        self.assertEqual(self.shown(), ["hands_toggle"])
        self.b.setShown("bogus", True)   # not a toggle: nothing changes
        self.assertEqual(ft_conf.read(self.conf)["QUICK_SETTINGS"], "bogus,hands_toggle")


if __name__ == "__main__":
    unittest.main()
