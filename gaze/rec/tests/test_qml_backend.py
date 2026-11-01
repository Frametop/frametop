#!/usr/bin/env python3
"""main.qml against ft_eyerec.Backend: every backend.name(...) the window calls is a slot, and
every backend.name it reads is a property or a slot. A method that lost its @Slot shows up in
QML only as "is not a function" when its button is pressed (the hand recorder's Export, 2026-10-03).
Needs PySide6 (the dev container); skipped without it. FT_EYEREC_QML=PATH checks another
window against the same backend (the standalone Eye Recorder's, in its CI). The shared pages
in recorder/qml/ count as part of main.qml, so each is checked against both windows' backends.

  python3 -m gaze.rec.tests.test_qml_backend
"""
import os
import re
import unittest

from recorder import qmlcheck

HERE = os.path.dirname(os.path.abspath(__file__))
REC = os.path.dirname(HERE)
QML = os.environ.get("FT_EYEREC_QML") or os.path.join(REC, "main.qml")


class QmlBackendTest(unittest.TestCase):
    def setUp(self):
        try:
            from gaze.rec import ft_eyerec
        except ImportError as e:
            self.skipTest(f"no PySide6: {e}")
        meta = self.meta = ft_eyerec.Backend.staticMetaObject
        self.slots = {bytes(meta.method(i).name()).decode() for i in range(meta.methodCount())}
        self.props = {meta.property(i).name() for i in range(meta.propertyCount())}
        self.qml = qmlcheck.window_text(QML)   # with the shared pages in recorder/qml/

    def test_calls_are_slots(self):
        called = set(re.findall(r"\bbackend\.(\w+)\s*\(", self.qml))
        self.assertTrue(called)
        self.assertEqual(sorted(called - self.slots), [], "called from main.qml but not a slot")

    def test_call_arity(self):
        """Each backend.name(a, b) call passes as many arguments as some slot of that name takes
        (a decorator left at the old count fails only when the button is pressed)."""
        arity = {}
        meta = self.meta
        for i in range(meta.methodCount()):
            mm = meta.method(i)
            arity.setdefault(bytes(mm.name()).decode(), set()).add(mm.parameterCount())
        bad = []
        for m in re.finditer(r"\bbackend\.(\w+)\s*\(", self.qml):
            depth, args, k, seen = 1, 0, m.end(), False
            while depth and k < len(self.qml):
                c = self.qml[k]
                if c in "([{":
                    depth += 1
                elif c in ")]}":
                    depth -= 1
                elif c == "," and depth == 1:
                    args += 1
                elif not c.isspace() and depth >= 1:
                    seen = True
                k += 1
            n = args + 1 if seen else 0
            if m.group(1) in arity and n not in arity[m.group(1)]:
                bad.append("%s: %d arguments, slots take %s" % (m.group(1), n, sorted(arity[m.group(1)])))
        self.assertEqual(bad, [])

    def test_reads_exist(self):
        read = set(re.findall(r"\bbackend\.(\w+)\b(?!\s*\()", self.qml))
        self.assertTrue(read)
        self.assertEqual(sorted(read - self.props - self.slots), [], "read in main.qml but not on Backend")

    def test_compiles(self):
        """main.qml and the shared pages compile with Kirigami: imports, types and the properties
        each page is given (a shared page that lost a property fails here, not on the headset)."""
        try:
            errors = qmlcheck.compile_errors([QML, *qmlcheck.shared_files()])
        except RuntimeError as e:
            self.skipTest(f"can't compile QML here: {e}")
        if any("kirigami" in line.lower() and "not installed" in line for errs in errors.values() for line in errs):
            self.skipTest("no Kirigami")
        self.assertEqual({p: e for p, e in errors.items() if e}, {})


if __name__ == "__main__":
    unittest.main()
