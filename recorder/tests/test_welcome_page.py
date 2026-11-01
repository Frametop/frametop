"""The recorders' shared Welcome page (recorder/qml/WelcomePage.qml), run offscreen against a stand-in
backend: before agreeing, the three checkboxes gate "Agree and continue", which records the
agreement through the window's accept and goes on with next; after agreeing, Continue goes on;
"changed since you agreed" shows only when the window says so; a window's own fields sit with the
checkboxes. Needs PySide6 and Kirigami (the image's python3); skipped without them. Nothing is
read from or sent to the host.

  python3 recorder/tests/test_welcome_page.py
"""
import os
import pathlib
import tempfile
import unittest

os.environ["HOME"] = tempfile.mkdtemp(prefix="ft-welcome-page-test-")
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
os.environ["QML_DISABLE_DISK_CACHE"] = "1"
SHARED = pathlib.Path(__file__).resolve().parents[1] / "qml"

try:
    from PySide6.QtCore import Property, QObject, Signal, Slot, QUrl
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtQml import QQmlComponent, QQmlEngine
    from PySide6.QtQuick import QQuickItem  # noqa: F401  (the page's items come back as QQuickItems)
except ImportError as e:  # (no PySide6 here)
    QObject, MISSING = object, str(e)
else:
    MISSING = None

WINDOW = """
import QtQuick
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami
import "."
Item {
    property var accepted: []
    property int nexts: 0
    WelcomePage {
        objectName: "page"
        draftText: ""
        changedSinceAgreed: CHANGED
        accept: (a, r, g) => { parent.accepted = [a, r, g]; backend.acceptConsent(a, r, g) }
        next: () => parent.nexts += 1
        Controls.Label { objectName: "mine"; text: "a window's own field" }
    }
}
"""


if MISSING is None:
    class Backend(QObject):
        changed = Signal()

        def __init__(self, needs):
            super().__init__()
            self._needs, self.calls = needs, []

        def _get_needs(self):
            return self._needs

        needsConsent = Property(bool, _get_needs, notify=changed)
        consentText = Property(str, lambda self: "The text.", constant=True)
        consentAccepted = Property(str, lambda self: "2026-10-01", constant=True)
        contributor = Property(str, lambda self: "c-123", constant=True)

        @Slot(bool, bool, bool)
        def acceptConsent(self, a, r, g):
            self.calls.append(("accept", a, r, g))
            self._needs = False
            self.changed.emit()

        @Slot(str)
        def copy(self, text):
            self.calls.append(("copy", text))


def find(item, pred):
    """The first item under item (itself included) that pred accepts, depth first."""
    if pred(item):
        return item
    for child in item.childItems():
        hit = find(child, pred)
        if hit is not None:
            return hit
    return None


def by_text(root, text):
    return find(root, lambda i: i.property("text") == text and i.metaObject().indexOfMethod("click()") >= 0)


@unittest.skipIf(MISSING, f"no PySide6: {MISSING}")
class WelcomePage(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QGuiApplication.instance() or QGuiApplication([])

    def load(self, needs, changed="false"):
        self.backend = Backend(needs)
        engine = self.engine = QQmlEngine()
        engine.rootContext().setContextProperty("backend", self.backend)
        comp = self.comp = QQmlComponent(engine)
        comp.setData(WINDOW.replace("CHANGED", changed).encode(), QUrl.fromLocalFile(str(SHARED / "test.qml")))
        root = comp.create()
        if root is None:
            errors = "; ".join(e.toString() for e in comp.errors())
            if "kirigami" in errors.lower():
                self.skipTest(f"no Kirigami here: {errors[:200]}")
            self.fail(errors)
        QQmlEngine.setObjectOwnership(root, QQmlEngine.ObjectOwnership.CppOwnership)
        self.root = root
        self.app.processEvents()
        return root

    def visible(self, text):
        hit = find(self.root, lambda i: i.property("text") == text)
        self.assertIsNotNone(hit, text)
        return hit.isVisible()

    def test_agree_needs_all_three_boxes(self):
        root = self.load(True)
        agree = by_text(root, "Agree and continue")
        self.assertFalse(agree.property("enabled"))
        boxes = ["I'm 18 or older", "I don't live in Illinois, Texas or Washington (USA)", "I agree to the text above"]
        for left_out in boxes:  # any one unchecked keeps it off
            for label in boxes:
                by_text(root, label).setProperty("checked", label != left_out)
            self.app.processEvents()
            self.assertFalse(agree.property("enabled"), left_out)
        for label in boxes:
            by_text(root, label).setProperty("checked", True)
        self.app.processEvents()
        self.assertTrue(agree.property("enabled"))
        agree.metaObject().invokeMethod(agree, "click")
        self.app.processEvents()
        self.assertEqual(self.backend.calls, [("accept", True, True, True)])
        self.assertEqual(root.property("nexts"), 1)

    def test_agreed_shows_the_id_and_continue(self):
        root = self.load(False)
        self.assertFalse(self.visible("Agree and continue"))
        self.assertFalse(self.visible("I'm 18 or older"))
        self.assertFalse(find(root, lambda i: i.objectName() == "mine").isVisible())
        go = by_text(root, "Continue")
        go.metaObject().invokeMethod(go, "click")
        copy = by_text(root, "Copy contributor id")
        copy.metaObject().invokeMethod(copy, "click")
        self.app.processEvents()
        self.assertEqual((root.property("nexts"), self.backend.calls), (1, [("copy", "c-123")]))

    def test_the_window_says_when_the_text_changed(self):
        changed = "This text changed since you last agreed to it. Please read it again; your contributor id stays the same."
        self.load(True, "false")
        self.assertFalse(self.visible(changed))
        self.load(True, "true")
        self.assertTrue(self.visible(changed))

    def test_a_windows_own_fields_show_with_the_boxes(self):
        root = self.load(True)
        self.assertTrue(find(root, lambda i: i.objectName() == "mine").isVisible())


if __name__ == "__main__":
    unittest.main()
