"""ft-layout start or apply with no head pose (the headset off or asleep): the screens
aren't placed, but the visibility settings and the hidden screens still reach ft-screens."""
import importlib.util
import pathlib
import tempfile
import unittest


def load_layout_module(tmp):
    path = pathlib.Path(__file__).resolve().parents[1] / "ft_layout.py"
    spec = importlib.util.spec_from_file_location("no_head_layout", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.CONF_PATH = str(tmp / "frametop.conf")  # none: the screens backend
    module.LAYOUT_PATH = str(tmp / "layout.json")
    module.ANCHOR_PATH = str(tmp / "anchor.json")
    return module


class FakeScreens:
    """ft-screens with every screen up and no head pose; what it's asked is recorded."""

    def __init__(self, count):
        self.count, self.asked = count, []

    def ask(self, text, timeout=10.0):
        self.asked.append(text)
        if text == "screens":
            return "ok " + str(self.count) + "".join(f" {i + 1}:1" for i in range(self.count))
        if text == "head":
            raise RuntimeError("error no head pose")
        return "ok"


class NoHead(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.m = load_layout_module(pathlib.Path(self.tmp.name))
        self.m.save_layout({"screens": [{}, {"hidden": True}], "visibility": {"mode": "toggle"}})
        self.screens = FakeScreens(self.m.screen_count(self.m.load_layout()))
        self.m.screens_socket = lambda: self.screens

    def tearDown(self):
        self.tmp.cleanup()

    def test_settings_reach_screens_without_a_head_pose(self):
        with self.assertRaises(RuntimeError):
            self.m.apply_screens()
        asked = self.screens.asked
        self.assertIn("visibility toggle", asked)
        self.assertIn("conceal 2", asked)
        self.assertIn("reveal 1", asked)
        self.assertFalse([a for a in asked if a.startswith("place")])


if __name__ == "__main__":
    unittest.main()
