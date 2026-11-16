"""Screens pinned to the SteamVR dashboard (ft-screens' "pin N dashboard", screens/vr.cpp): ft-layout
reads the pin from "get", saved or waiting for the dashboard to open, and a layout that saved one
pins it again. The layout file is in a temp folder; ft-screens is faked.

  python3 layout/tests/test_dashboard_pin.py
"""
import importlib.util
import pathlib
import tempfile
import unittest

REL = [1, 0, 0, 0.9, 0, 1, 0, 0.1, 0, 0, 1, 0.05]
POSE = "0.9000 1.2000 -1.0000  1 0 0  0 1 0  0 0 1  1.2000 0.6750 0.000"


def load_layout_module(tmp):
    path = pathlib.Path(__file__).resolve().parents[1] / "ft_layout.py"
    spec = importlib.util.spec_from_file_location("dashboard_pin_layout", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.CONF_PATH = str(tmp / "frametop.conf")  # none: the screens backend
    module.LAYOUT_PATH = str(tmp / "layout.json")
    module.ANCHOR_PATH = str(tmp / "anchor.json")
    return module


class FakeScreens:
    """ft-screens with every screen up and a head pose; what it's asked is recorded."""

    def __init__(self, count):
        self.count, self.asked = count, []

    def ask(self, text, timeout=10.0):
        self.asked.append(text)
        if text == "screens":
            return "ok " + str(self.count) + "".join(f" {i + 1}:1" for i in range(self.count))
        if text == "head":
            return "ok 0.0 1.6 0.0 0.0"
        return "ok"


class DashboardPin(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.m = load_layout_module(pathlib.Path(self.tmp.name))

    def tearDown(self):
        self.tmp.cleanup()

    def test_get_with_the_pin(self):
        g = self.m.parse_get("ok " + POSE + " dashboard " + " ".join(map(str, REL)))
        self.assertEqual(g["hand"], "dashboard")
        self.assertEqual(g["rel"], REL)

    def test_get_waiting_without_a_pin_of_its_own(self):
        # pinned "as it is" before the dashboard opened: no offset yet, so none is saved
        g = self.m.parse_get("ok " + POSE + " dashboard")
        self.assertEqual(g["hand"], "dashboard")
        self.assertNotIn("rel", g)

    def test_a_saved_dashboard_pin_is_pinned_again(self):
        self.m.save_layout({"mode": "custom", "screens": [{"pin": {"hand": "dashboard", "rel": REL}}, {}]})
        screens = FakeScreens(2)
        self.m.screens_socket = lambda: screens
        self.m.apply_screens()
        pins = [a for a in screens.asked if a.startswith("pin ")]
        self.assertEqual(len(pins), 1)
        self.assertTrue(pins[0].startswith("pin 1 dashboard 1.00000 0.00000 0.00000 0.90000"), pins[0])


if __name__ == "__main__":
    unittest.main()
