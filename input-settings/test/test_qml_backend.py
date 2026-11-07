"""Frametop Settings' input pages (InputPages.qml) against ft_input_settings.Backend, offline: every backend.name(...)
the window calls is a slot (or signal) taking that many arguments, and every backend.name it reads
is a property or a method. Qt says nothing when a binding names something the backend lost: the
control just shows nothing, or its button does nothing, on the headset. Reads the class's
metaobject only, so no Backend runs, and nothing is read from or sent to the host. Needs PySide6
(the image's python3).

  python3 input-settings/test/test_qml_backend.py
"""
import os
import re
import sys
import tempfile
import unittest
from pathlib import Path

os.environ["HOME"] = tempfile.mkdtemp(prefix="ft-input-settings-qml-test-")
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
ROOT = Path(__file__).resolve().parents[2]
QML = (ROOT / "input-settings" / "InputPages.qml").read_text()
sys.path.insert(0, str(ROOT / "input-settings"))
import ft_input_settings as fis  # noqa: E402


def call_arity(text, start):
    """How many arguments the call whose "(" is just before start passes."""
    depth, commas, seen, k = 1, 0, False, start
    while depth and k < len(text):
        c = text[k]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == "," and depth == 1:
            commas += 1
        elif not c.isspace() and depth >= 1:
            seen = True
        k += 1
    return commas + 1 if seen else 0


class QmlBackend(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        meta = fis.Backend.staticMetaObject
        cls.arity = {}
        for i in range(meta.methodCount()):
            m = meta.method(i)
            cls.arity.setdefault(bytes(m.name()).decode(), set()).add(m.parameterCount())
        cls.props = {meta.property(i).name() for i in range(meta.propertyCount())}

    def test_calls_are_methods(self):
        called = set(re.findall(r"\bbackend\.(\w+)\s*\(", QML))
        self.assertTrue(called)
        self.assertEqual(sorted(called - set(self.arity)), [], "called from InputPages.qml but not a slot")

    def test_call_arity(self):
        bad = sorted({f"{m.group(1)}: {call_arity(QML, m.end())} arguments, it takes {sorted(self.arity[m.group(1)])}"
                      for m in re.finditer(r"\bbackend\.(\w+)\s*\(", QML)
                      if m.group(1) in self.arity and call_arity(QML, m.end()) not in self.arity[m.group(1)]})
        self.assertEqual(bad, [])

    def test_reads_exist(self):
        read = set(re.findall(r"\bbackend\.(\w+)\b(?!\s*\()", QML))
        self.assertTrue(read)
        self.assertEqual(sorted(read - self.props - set(self.arity)), [], "read in InputPages.qml but not on Backend")


if __name__ == "__main__":
    unittest.main()
