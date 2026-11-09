"""Drag-bar target setting: persistence, validation and live IPC, with no headset input."""
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

os.environ["HOME"] = tempfile.mkdtemp(prefix="ft-display-bar-")
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "settings"))
import ft_display_settings as fds  # noqa: E402
import ft_settings  # noqa: E402  (Frametop Settings, the window the display pages are in)
from PySide6.QtGui import QGuiApplication  # noqa: E402
from PySide6.QtQml import QQmlApplicationEngine  # noqa: E402

PAGES = Path(fds.HERE) / "DisplayPages.qml"


def page_errors(backend, first_marker, last_marker):
    """Frametop Settings opened on Visibility & pins; QML errors from DisplayPages.qml's lines
    first_marker..last_marker. None when Frametop Settings doesn't load here (no Kirigami)."""
    errors = []
    engine = QQmlApplicationEngine()
    engine.warnings.connect(lambda ws: errors.extend(w.toString() for w in ws))
    ft_settings.load(engine, ft_settings.Settings(start="visibility", backends={"display": backend}))
    if not engine.rootObjects():
        return None
    for _ in range(20):
        QGuiApplication.processEvents()
    lines = PAGES.read_text().split("\n")
    first = next(i for i, t in enumerate(lines) if first_marker in t) + 1
    last = next(i for i in range(first, len(lines)) if last_marker in lines[i]) + 1
    for root in engine.rootObjects():
        root.close()
    engine.deleteLater()
    return [e for e in errors if any(f"DisplayPages.qml:{n}:" in e for n in range(first, last + 1))]


class GrabBar(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QGuiApplication.instance() or QGuiApplication([])

    def setUp(self):
        Path(fds.CONF_PATH).parent.mkdir(parents=True, exist_ok=True)
        Path(fds.CONF_PATH).write_text("# keep this\nGRAB_BAR_PADDING_Y=2 # keep this too\n")
        self.b = fds.Backend()
        self.b._running = True
        self.asked = []
        self.b._ask_screens = lambda text, then=None: self.asked.append(text)
        self.backend = patch.object(fds.ft_layout, "backend", return_value="screens")
        self.backend.start()
        self.addCleanup(self.backend.stop)

    def test_default_and_live_setting(self):
        self.assertEqual(self.b.grabBarPaddingY, 2)
        self.assertEqual(self.b.grabBarPaddingX, 2)
        for cm in (3, 2.7, 5, 50, 0):
            self.b.setGrabBarPaddingY(cm)
            self.assertEqual(self.b.grabBarPaddingY, cm)
            self.assertEqual(self.asked[-1], f"handsetting GRAB_BAR_PADDING_Y {cm:g}")
        text = Path(fds.CONF_PATH).read_text()
        self.assertIn("# keep this", text)
        self.assertIn("# keep this too", text)

    def test_move_bar_hover_haptics(self):
        self.assertTrue(self.b.grabBarHaptics)
        self.b.setGrabBarHaptics(False)
        self.assertFalse(self.b.grabBarHaptics)
        self.assertEqual(self.asked[-1], "handsetting GRAB_BAR_HAPTICS 0")
        self.b.setGrabBarHaptics(True)
        self.assertEqual(self.asked[-1], "handsetting GRAB_BAR_HAPTICS 1")
        self.assertIn("backend.setGrabBarHaptics(checked)", PAGES.read_text())

    def test_bad_persisted_values_use_default(self):
        for value in ("nan", "inf", "junk", "-1", "51"):
            Path(fds.CONF_PATH).write_text(f"GRAB_BAR_PADDING_Y={value}\n")
            self.assertEqual(self.b.grabBarPaddingY, 2)

    def test_setter_bounds_and_nonfinite_values(self):
        for value, want in ((-1, 0), (60, 50), (float("nan"), 2), (float("inf"), 2)):
            self.b.setGrabBarPaddingY(value)
            self.assertEqual(self.b.grabBarPaddingY, want)

    def test_saved_without_running_screens(self):
        self.b._running = False
        self.b.setGrabBarPaddingY(3)
        self.assertEqual(self.b.grabBarPaddingY, 3)
        self.assertEqual(self.asked, [])
        self.b._running = True
        with patch.object(fds.ft_layout, "backend", return_value="gamescope"):
            self.b.setGrabBarPaddingY(2)
        self.assertEqual(self.asked, [])

    def test_ui_exposes_the_setting(self):
        qml = PAGES.read_text()
        self.assertIn("value: backend.grabBarPaddingY", qml)
        self.assertIn("onMoved: backend.setGrabBarPaddingY(value)", qml)
        self.assertIn("onMoved: backend.setGrabBarPaddingX(value)", qml)
        self.assertIn("from: 0; to: 50; stepSize: 0.1", qml)
        self.assertNotIn("grabBarHitScale", qml)
        self.assertNotIn("grabBarHitWidth", qml)
        for setter in ("setGrabBarPreview", "setGrabBarPreviewAlways", "setGrabBarPreviewColor",
                       "setGrabBarOutlineColor", "setGrabBarPreviewOpacity", "setGrabBarOutlineOpacity",
                       "setGrabBarOutlineWidth"):
            self.assertIn(f"backend.{setter}(", qml)

    def test_axes_are_independent_in_centimetres(self):
        self.b.setGrabBarPaddingY(50)
        self.b.setGrabBarPaddingX(2.5)
        self.assertEqual(self.b.grabBarPaddingY, 50)
        self.assertEqual(self.b.grabBarPaddingX, 2.5)
        self.assertEqual(self.asked[-1], "handsetting GRAB_BAR_PADDING_X 2.5")

    def test_preview_switches_and_style_are_saved_and_sent(self):
        for prop, setter, key, value in (
            ("grabBarPreview", "setGrabBarPreview", "GRAB_BAR_PREVIEW", True),
            ("grabBarPreviewAlways", "setGrabBarPreviewAlways", "GRAB_BAR_PREVIEW_ALWAYS", True),
            ("grabBarPreviewOpacity", "setGrabBarPreviewOpacity", "GRAB_BAR_PREVIEW_OPACITY", 0.2),
            ("grabBarOutlineOpacity", "setGrabBarOutlineOpacity", "GRAB_BAR_OUTLINE_OPACITY", 0.8),
            ("grabBarOutlineWidth", "setGrabBarOutlineWidth", "GRAB_BAR_OUTLINE_WIDTH", 3),
        ):
            getattr(self.b, setter)(value)
            self.assertEqual(getattr(self.b, prop), value)
            self.assertEqual(self.asked[-1], f"handsetting {key} {value:g}")
        self.b.setGrabBarPreview(False)
        self.assertFalse(self.b.grabBarPreview)
        # Disabling the preview doesn't erase the user's style or always-visible choice.
        self.assertTrue(self.b.grabBarPreviewAlways)
        self.assertEqual(self.b.grabBarOutlineWidth, 3)

    def test_colors_validate_without_allowing_extra_commands(self):
        for setter, prop, key in ((self.b.setGrabBarPreviewColor, "grabBarPreviewColor", "GRAB_BAR_PREVIEW_COLOR"),
                                  (self.b.setGrabBarOutlineColor, "grabBarOutlineColor", "GRAB_BAR_OUTLINE_COLOR")):
            setter("#AABBCC")
            self.assertEqual(getattr(self.b, prop), "#aabbcc")
            self.assertEqual(self.asked[-1], f"handsetting {key} #aabbcc")
            count = len(self.asked)
            for bad in ("red", "#123", "#11223344", "#12345z", "#112233\nclick native"):
                setter(bad)
                self.assertEqual(getattr(self.b, prop), "#aabbcc")
            self.assertEqual(len(self.asked), count)

    def test_preview_style_bounds(self):
        self.b.setGrabBarPreviewOpacity(-1)
        self.b.setGrabBarOutlineOpacity(2)
        self.b.setGrabBarOutlineWidth(30)
        self.assertEqual(self.b.grabBarPreviewOpacity, 0)
        self.assertEqual(self.b.grabBarOutlineOpacity, 1)
        self.assertEqual(self.b.grabBarOutlineWidth, 8)

    def test_qml_page_loads_with_color_dialogs(self):
        errors = page_errors(self.b, 'label: "Window controls"', 'label: "Resize corners"')
        if errors is None:
            self.skipTest("Frametop Settings didn't load here (no Kirigami?)")
        self.assertEqual(errors, [], "Display Settings QML failed to load")


if __name__ == "__main__":
    unittest.main()
