"""Resize-corner defaults, persistence, validation, live IPC and headless QML."""
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

os.environ["HOME"] = tempfile.mkdtemp(prefix="ft-resize-settings-")
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import ft_display_settings as fds  # noqa: E402
from PySide6.QtGui import QGuiApplication  # noqa: E402
from PySide6.QtCore import QUrl  # noqa: E402
from PySide6.QtQml import QQmlApplicationEngine  # noqa: E402


class ResizeCorner(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QGuiApplication.instance() or QGuiApplication([])

    def setUp(self):
        Path(fds.CONF_PATH).parent.mkdir(parents=True, exist_ok=True)
        Path(fds.CONF_PATH).write_text("# preserve this comment\n")
        self.b = fds.Backend()
        self.b._running = True
        self.asked = []
        self.b._ask_screens = lambda text, then=None: self.asked.append(text)
        backend = patch.object(fds.ft_layout, "backend", return_value="screens")
        backend.start()
        self.addCleanup(backend.stop)

    def test_defaults_and_example_agree(self):
        defaults = {"resizeCornerStyle": "native", "resizeCornerColor": "#f6f9ff",
                    "resizeCornerLength": 4.7, "resizeCornerWidth": 5.1, "resizeCornerOpacity": 0.27,
                    "resizeCornerGap": 3.1, "resizeCornerPadding": 2.4, "resizeCornerExclusion": 0.5,
                    "resizeCornerRoundness": 100, "resizeCornerReach": 20, "resizeCornerPreview": False,
                    "resizeCornerHaptics": True, "resizeCornerAlways": [False, False, False, True]}
        self.assertEqual({p: getattr(self.b, p) for p in defaults}, defaults)
        example = fds.ft_conf.read(str(Path(fds.HERE).parent / "session/frametop.conf.example"))
        with patch.object(fds.ft_layout, "read_conf", return_value=example):
            self.assertEqual({p: getattr(self.b, p) for p in defaults}, defaults)

    def test_settings_persist_and_apply_live(self):
        qml = (Path(fds.HERE) / "main.qml").read_text()
        for prop, key, value in (
            ("Style", "STYLE", "glow"), ("Color", "COLOR", "#abcdef"),
            ("Length", "LENGTH", 5.5), ("Width", "WIDTH", 1.5), ("Opacity", "OPACITY", 0.8),
            ("Gap", "GAP", 3.5), ("Padding", "PADDING", 1.5), ("Exclusion", "EXCLUSION", 0.7),
            ("Reach", "REACH", 25.5), ("HoverColor", "HOVER_COLOR", "#00ffff"),
            ("Roundness", "ROUNDNESS", 50.5), ("Style", "STYLE", "native"),
            ("Preview", "PREVIEW", True), ("Haptics", "HAPTICS", False),
        ):
            getattr(self.b, "setResizeCorner" + prop)(value)
            self.assertEqual(getattr(self.b, "resizeCorner" + prop), value)
            wire = str(int(value)) if isinstance(value, bool) else str(value)
            self.assertEqual(self.asked[-1], f"handsetting RESIZE_CORNER_{key} {wire}")
            self.assertIn("backend.setResizeCorner" + prop + "(", qml)
        self.assertIn("# preserve this comment", Path(fds.CONF_PATH).read_text())

    def test_validation_and_injection_rejection(self):
        for bad in ("red", "#123", "#12345678", "#12345z", "#112233\nclick native"):
            self.b.setResizeCornerColor(bad)
        self.b.setResizeCornerStyle("glow\nclick native")
        self.assertEqual(self.asked, [])
        for prop, value, want in (("Length", -1, 1), ("Width", 30, 6), ("Width", float("nan"), 5.1),
                                  ("Opacity", 2, 1), ("Gap", -1, 0), ("Padding", 0, 0.1),
                                  ("Exclusion", 20, 5), ("Reach", 100, 50), ("Roundness", 120, 100)):
            getattr(self.b, "setResizeCorner" + prop)(value)
            self.assertEqual(getattr(self.b, "resizeCorner" + prop), want)
        Path(fds.CONF_PATH).write_text("RESIZE_CORNER_STYLE=junk\nRESIZE_CORNER_WIDTH=nan\nRESIZE_CORNER_TARGET=50\n")
        self.assertEqual(self.b.resizeCornerStyle, "native")
        self.assertEqual(self.b.resizeCornerWidth, 5.1)
        self.assertEqual(self.b.resizeCornerPadding, 2.4)

    def test_all_persistent_corner_combinations_and_legacy_preferences(self):
        for mask in range(16):
            for c, key in enumerate(("TL", "TR", "BL", "BR")):
                on = bool(mask & (1 << c))
                self.b.setResizeCornerAlways(c, on)
                self.assertEqual(self.asked[-1], f"handsetting RESIZE_CORNER_ALWAYS_{key} {int(on)}")
            self.assertEqual(self.b.resizeCornerAlways, [bool(mask & (1 << c)) for c in range(4)])
        count = len(self.asked)
        for bad in (-1, 4, 100):
            self.b.setResizeCornerAlways(bad, True)
        self.assertEqual(len(self.asked), count)
        self.b.setResizeCornerStyle("glow")
        self.b.setResizeCornerPreview(True)
        self.b.setResizeCornerWidth(4)
        self.b.setResizeCornerHaptics(False)
        self.b.setResizeCornerStyle("native")
        self.assertEqual(self.b.resizeCornerAlways, [True] * 4)
        self.assertTrue(self.b.resizeCornerPreview)
        self.assertEqual(self.b.resizeCornerWidth, 4)
        self.assertFalse(self.b.resizeCornerHaptics)

    def test_save_without_live_compositor(self):
        self.b._running = False
        self.b.setResizeCornerGap(3)
        self.assertEqual(self.b.resizeCornerGap, 3)
        self.assertEqual(self.asked, [])

    def test_qml_page_and_color_dialogs_load(self):
        engine = QQmlApplicationEngine()
        engine.rootContext().setContextProperty("backend", self.b)
        engine.rootContext().setContextProperty("startPage", "visibility")
        engine.load(QUrl.fromLocalFile(str(Path(fds.HERE) / "main.qml")))
        self.assertTrue(engine.rootObjects(), "Resize settings QML failed to load")
        for root in engine.rootObjects():
            root.close()
        engine.deleteLater()
        self.app.processEvents()


if __name__ == "__main__":
    unittest.main()
