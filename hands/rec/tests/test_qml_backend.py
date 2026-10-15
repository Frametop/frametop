#!/usr/bin/env python3
"""main.qml against ft_handrec.Backend: every backend.name(...) the window calls is a slot, and
every backend.name it reads is a property or a slot. A method that lost its @Slot shows up in
QML only as "is not a function" when its button is pressed (2026-10-03: Export did nothing).
Needs PySide6 (the dev container); skipped without it. FT_HANDREC_QML=PATH checks another
window against the same backend (the standalone Hand Recorder's, in its CI).

  python3 hands/rec/tests/test_qml_backend.py
"""
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REC = os.path.dirname(HERE)
sys.path.insert(0, REC)
QML = os.environ.get("FT_HANDREC_QML") or os.path.join(REC, "main.qml")


class QmlBackendTest(unittest.TestCase):
    def setUp(self):
        try:
            import ft_handrec
        except ImportError as e:
            self.skipTest(f"no PySide6: {e}")
        meta = self.meta = ft_handrec.Backend.staticMetaObject
        self.slots = {bytes(meta.method(i).name()).decode() for i in range(meta.methodCount())}
        self.props = {meta.property(i).name() for i in range(meta.propertyCount())}
        with open(QML) as f:
            self.qml = f.read()

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

    def test_handlers_are_signals(self):
        """Each function onName(...) in a Connections on backend has a signal name (a renamed
        signal leaves its handler silently unused: Export and upload would stop at Export)."""
        meta = self.meta
        signals = {bytes(meta.method(i).name()).decode() for i in range(meta.methodCount())
                   if meta.method(i).methodType() == meta.method(i).MethodType.Signal}
        handlers = set()
        for m in re.finditer(r"Connections\s*\{\s*target:\s*backend\b", self.qml):
            depth, k = 1, m.end()
            while depth and k < len(self.qml):
                depth += {"{": 1, "}": -1}.get(self.qml[k], 0)
                k += 1
            block = self.qml[m.end():k]
            handlers |= {h[0].lower() + h[1:] for h in re.findall(r"function on([A-Z]\w*)\s*\(", block)}
        self.assertIn("exportDone", handlers)
        self.assertEqual(sorted(handlers - signals), [], "handled in main.qml but not a Backend signal")



class GuidanceTest(unittest.TestCase):
    """The demo clips to follow: recorded by default when there are recorded clips, the choice
    kept in BASE/settings.json and handed to the session."""

    def test_choice(self):
        import json
        import shutil
        import tempfile
        try:
            import ft_handrec
            import takes
        except ImportError as e:
            self.skipTest(f"no PySide6: {e}")
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp)
        rec = os.path.join(tmp, "rec")
        os.makedirs(rec)
        backend = ft_handrec.Backend(takes.Store(os.path.join(tmp, "base")), {"recorded_demos_dir": rec})
        self.assertEqual([c["value"] for c in backend.guidanceChoices], ["recorded", "skeleton"])
        self.assertEqual(backend.guidance, "skeleton")   # no recorded clips
        with open(os.path.join(rec, "2a.gif"), "wb") as f:
            f.write(b"GIF89a")
        self.assertEqual(backend.guidance, "recorded")
        backend.setGuidance("skeleton")
        backend.setGuidance("nonsense")
        with open(os.path.join(tmp, "base", "settings.json")) as f:
            self.assertEqual(json.load(f), {"guidance": "skeleton"})
        self.assertEqual(backend.guidance, "skeleton")


class TestingSessionTest(unittest.TestCase):
    """The testing session: on the board only with Frametop's hand tracking installed, under the
    testing consent (TESTING.md) that bounty rounds use too in the test phase, and its uploads
    go to the test dataset."""

    def backend(self, frametop_hands):
        import shutil
        import tempfile
        try:
            import ft_handrec
            import takes
        except ImportError as e:
            self.skipTest(f"no PySide6: {e}")
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp)
        old = os.environ.get("FT_HANDREC_FRAMETOP_HANDS")
        os.environ["FT_HANDREC_FRAMETOP_HANDS"] = frametop_hands
        self.addCleanup(lambda: os.environ.pop("FT_HANDREC_FRAMETOP_HANDS") if old is None
                        else os.environ.__setitem__("FT_HANDREC_FRAMETOP_HANDS", old))
        return ft_handrec, takes, ft_handrec.Backend(takes.Store(os.path.join(tmp, "base")),
                                                     {"dry_run": True}, ignore_cameras=True)

    def test_board(self):
        ft, _, backend = self.backend("1")
        rows = backend.bounties
        self.assertEqual(rows[0]["id"], ft.DEBUG_ID)
        self.assertEqual(rows[0]["done"], 0)
        self.assertIn("3 min", rows[0]["minutesText"])
        self.assertIn("min", backend.planText({}, True, False, ft.DEBUG_ID))
        _, _, backend = self.backend("0")
        self.assertNotIn(ft.DEBUG_ID, [b["id"] for b in backend.bounties])

    def test_consent_kinds(self):
        ft, takes, backend = self.backend("1")
        bounty = next(b["id"] for b in backend.bounties if b["id"] != ft.DEBUG_ID)
        testing = "testing" if takes.TEST_PHASE else "public"
        self.assertEqual(backend.roundConsentKind(ft.DEBUG_ID), "testing")
        self.assertEqual(backend.roundConsentKind(bounty), testing)
        self.assertEqual(backend.roundConsentKind(""), "public")
        backend.setConsentKind("public")
        self.assertFalse(backend.startSession({}, "auto", True, False, ft.DEBUG_ID))
        self.assertEqual(backend.consentKind, "testing")   # switched to the text to agree to
        self.assertTrue(backend.needsConsent)
        self.assertIn("# Testing sessions", backend.consentText)
        self.assertEqual(backend.dataset, ft.hub.HF_TEST_DATASET)
        backend.acceptConsent(True, True, True, "right")
        self.assertFalse(backend.needsConsent)
        self.assertTrue(backend.has_consent("testing"))
        self.assertFalse(backend.has_consent("public"))
        backend.setConsentKind("public")
        self.assertTrue(backend.needsConsent)
        self.assertEqual(backend.dataset, ft.hub.HF_DATASET)
        profile = backend.store.profile()
        self.assertIn("testing_consent", profile)
        self.assertNotIn("consent", profile)
        # after the test phase a bounty asks for the dataset's own consent and goes to it
        if not takes.TEST_PHASE:
            backend.setConsentKind(backend.roundConsentKind(bounty))
            self.assertEqual(backend.consentKind, "public")
            self.assertTrue(backend.needsConsent)
            self.assertIn("bounty rounds", backend.consentText)
            self.assertEqual(backend.dataset, ft.hub.HF_DATASET)


if __name__ == "__main__":
    unittest.main()
