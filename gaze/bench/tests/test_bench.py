#!/usr/bin/env python3
"""The gaze bench without a headset: the scenarios' targets, bench.qml against gazebench.Bench,
and a whole run recorded with stand-ins for ft-gaze (fake_ft_gaze.py), our tracker and the gaze
service (their sockets), into a temporary folder. Needs PySide6 (the dev container); the parts
that need it are skipped without it.

  scripts/in-box python3 gaze/bench/tests/test_bench.py
"""
import json
import math
import os
import random
import re
import socket
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
BENCH = HERE.parent
sys.path.insert(0, str(BENCH))
os.environ["FT_BENCH_EYES_SOCKET"] = f"ft_eyes_benchtest_{os.getpid()}"
os.environ["FT_BENCH_GAZED_SOCKET"] = f"ft_gazed_benchtest_{os.getpid()}"
os.environ["FT_BENCH_SCREENS_SOCKET"] = f"ft_screens_benchtest_{os.getpid()}"
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
import benchrec  # noqa: E402

try:
    from PySide6.QtCore import QUrl
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtQml import QQmlApplicationEngine
    from PySide6.QtQuickControls2 import QQuickStyle
    import gazebench
    APP = QGuiApplication.instance() or QGuiApplication([sys.argv[0]])
    QQuickStyle.setStyle("Fusion")  # the app's
except ImportError as e:
    gazebench, APP, NO_QT = None, None, str(e)

SCENARIOS = json.loads((BENCH / "scenarios.json").read_text())
FAKE_GAZE = HERE / "fake_ft_gaze.py"


class FakeService(threading.Thread):
    """A datagram server on an abstract socket that answers with reply(text)."""

    def __init__(self, name, reply):
        super().__init__(daemon=True)
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.sock.bind("\0" + name)
        self.reply, self.heard, self.done = reply, [], False
        self.start()

    def stop(self):
        self.done = True
        self.join(1)
        self.sock.close()

    def run(self):
        self.sock.settimeout(0.05)
        while not self.done:
            try:
                data, addr = self.sock.recvfrom(65536)
            except TimeoutError:
                continue
            except OSError:
                return
            text = data.decode()
            self.heard.append(text)
            r = self.reply(text)
            if r is not None and addr:
                self.sock.sendto(r.encode(), addr)


class ScenarioTest(unittest.TestCase):
    def setUp(self):
        if gazebench is None:
            self.skipTest(f"no PySide6: {NO_QT}")

    def test_targets(self):
        for sc in SCENARIOS["scenarios"]:
            ts = gazebench.make_trials(sc, random.Random(1))
            self.assertTrue(ts, sc["id"])
            for p in ts:
                self.assertTrue(0 <= p["u"] <= 1 and 0 <= p["v"] <= 1, (sc["id"], p))
            if sc["id"] == "edges":
                self.assertEqual(len(ts), 16)
                self.assertTrue(all(p["u"] in (0, 1) or p["v"] in (0, 1) for p in ts))
            if sc["id"] == "small":
                self.assertTrue(all("decoy" in p for p in ts))
            if sc["id"] == "hold":
                self.assertTrue(all(p["dwell"] == 3.0 for p in ts))
            if sc["id"] == "light":
                self.assertEqual({p["bg"] for p in ts}, set(sc["backgrounds"]))
        rays = gazebench.make_trials(next(s for s in SCENARIOS["scenarios"] if s["id"] == "eyes-only"), random.Random(1))
        outer = [p for p in rays if p["u"] in (0, 1) or p["v"] in (0, 1)]
        self.assertEqual(len(outer), 8)  # the 1.0 ring reaches the edges and corners

    def test_place(self):
        b = gazebench.Bench.__new__(gazebench.Bench)
        b.screen = {"w": 1000, "h": 800}
        self.assertEqual(b.place(0, 0), (gazebench.INSET, gazebench.INSET))
        self.assertEqual(b.place(1, 1), (1000 - gazebench.INSET, 800 - gazebench.INSET))


class QmlTest(unittest.TestCase):
    """bench.qml's bench.name(...) calls are slots and its bench.name reads are properties or slots."""

    def setUp(self):
        if gazebench is None:
            self.skipTest(f"no PySide6: {NO_QT}")
        meta = gazebench.Bench.staticMetaObject
        self.slots = {bytes(meta.method(i).name()).decode() for i in range(meta.methodCount())}
        self.props = {meta.property(i).name() for i in range(meta.propertyCount())}
        self.qml = (BENCH / "bench.qml").read_text()

    def test_names(self):
        called = set(re.findall(r"\bbench\.(\w+)\s*\(", self.qml))
        self.assertEqual(sorted(called - self.slots), [])
        read = set(re.findall(r"\bbench\.(\w+)\b(?!\s*\()", self.qml)) - {"qml"}
        self.assertEqual(sorted(read - self.props - self.slots), [])


class RunTest(unittest.TestCase):
    """A warm-up played through, recorded and summed up, and bench.qml loaded on it."""

    def setUp(self):
        if gazebench is None:
            self.skipTest(f"no PySide6: {NO_QT}")
        self.tmp = tempfile.TemporaryDirectory()
        tmp = Path(self.tmp.name)
        self.saved = {k: getattr(benchrec, k) for k in ("STATE", "EYES_STATE", "LESSON_LOG", "CONF")}
        benchrec.STATE = tmp / "state"
        benchrec.EYES_STATE = benchrec.STATE / "eyes"
        benchrec.LESSON_LOG = benchrec.STATE / "pointer-lessons.jsonl"
        benchrec.CONF = tmp / "frametop.conf"
        benchrec.EYES_STATE.mkdir(parents=True)
        (benchrec.EYES_STATE / "calibration.json").write_text('{"version": 1, "fits": []}')
        benchrec.CONF.write_text("GAZE_TRACKER=own\nOTHER=1\n")
        benchrec.LESSON_LOG.write_text(json.dumps({"time": time.time() - 100, "kind": "own"}) + "\n")
        self.finds = []

        def eyes(text):
            w = text.split()
            if w[0] == "status":
                return json.dumps({"calibration": {"made": "2026-10-17 10:00:00"}, "feature": "pupil", "model": None,
                                   "eyes": {"right": {"shift": [0, 0], "clicks": 0, "jump": False, "jump_px": [0, 0],
                                                      "reseat": False}}})
            if w[:2] == ["finds", "start"]:
                self.finds.append(w[2])
                Path(w[2]).write_text(json.dumps({"t": 1.0, "cam": 0, "lost": True}) + "\n")
                return "ok"
            if w[:2] == ["finds", "stop"]:
                return "ok 1"
            return "fail"

        def gazed(text):
            if text == "status":
                return json.dumps({"tracker": "own", "calibration_made": "2026-10-17 10:00:00",
                                   "checks": {"gaze_mode": True, "headset_worn": True}})
            return "ok"

        self.eyes = FakeService(benchrec.EYES_SOCK, eyes)
        self.gazed = FakeService(benchrec.GAZED_SOCK, gazed)
        # The fake ft-gaze's screen 2 is 1000 x 800; the window is 800 x 640 on it, and Qt's pixel
        # ratio (2) isn't the scale between them (1.25), as on the Frame's 3440-pixel screen
        self.screens = FakeService(benchrec.SCREENS_SOCK, lambda text: "ok 2 1:3440x1440:2.503 2:1000x800:2.0"
                                   if text == "screens" else "fail")

    def tearDown(self):
        for k, v in self.saved.items():
            setattr(benchrec, k, v)
        self.eyes.stop()
        self.gazed.stop()
        self.screens.stop()
        self.tmp.cleanup()

    def wait(self, cond, timeout=5.0):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            APP.processEvents()
            if cond():
                return True
            time.sleep(0.02)
        return False

    def test_run(self):
        base = Path(self.tmp.name) / "runs"
        bench = gazebench.Bench(SCENARIOS, str(base), images=False, gaze_prog=FAKE_GAZE)
        try:
            self.assertTrue(self.wait(lambda: bench.gaze.latest() is not None), "no samples from the fake ft-gaze")
            self.assertTrue(self.wait(lambda: "gaze mode on" in bench.status), bench.status)
            bench.setScreen(800, 640, 2.0, "test", 0, 0)
            bench.start("warmup")
            self.assertEqual(bench.page, "run")
            self.assertFalse(bench.targetOn)          # the intro first
            self.assertIn("Click anywhere to start", bench.message)
            bench.click(10, 10, "left")
            self.assertTrue(bench.targetOn)
            time.sleep(0.5)                            # samples in the look window
            n = len(bench.trials)
            for k in range(n):
                if k == 2:                             # take one back, then do it again
                    bench.undo()
                    self.assertEqual(bench.i, 1)
                    time.sleep(0.4)
                    bench.click(bench.tx, bench.ty, "left")
                if k == 4:                             # a skip
                    bench.click(5, 5, "right")
                    continue
                tx, ty = bench.tx, bench.ty
                bench.click(tx + (60 if k == 5 else 3), ty, "left")  # 5: off the target: the click is the truth
                time.sleep(0.4)
                self.wait(lambda: True, 0.05)
            self.assertEqual(bench.page, "done")
            self.assertIn("Our tracker", bench.summary)
            run = next(base.iterdir())
            self.assertTrue(self.wait(lambda: "end" in json.loads((run / "run.json").read_text())))
        finally:
            bench.shutdown()
        info = json.loads((run / "run.json").read_text())
        self.assertEqual(info["scenario"]["id"], "warmup")
        self.assertEqual(info["conf"], ["GAZE_TRACKER=own"])
        self.assertEqual(info["screen"]["w"], 800)
        self.assertEqual(info["screens"], {"1": [3440, 1440], "2": [1000, 800]})
        self.assertIsNotNone(info["last_lesson"])
        self.assertGreater(info["gaze_lines"], 50)
        self.assertEqual(info["finds_rows"], "ok 1")
        self.assertTrue((run / "start" / "eyes-calibration.json").exists())
        self.assertEqual(self.finds, [str(run / "own.jsonl")])
        rows = [json.loads(line) for line in (run / "trials.jsonl").read_text().splitlines()]
        kinds = [r["kind"] for r in rows]
        self.assertEqual(kinds[:2], ["message", "message-done"])
        self.assertIn("undo", kinds)
        trials = [r for r in rows if r["kind"] == "trial"]
        self.assertEqual(sum(1 for r in trials if r.get("skipped")), 1)
        off = [r for r in trials if r.get("truth", {}).get("from") == "click"]
        self.assertEqual(len(off), 1)
        for r in trials:
            if r.get("skipped"):
                continue
            q = r["quick"]
            self.assertEqual(q["screen"], 2)
            # The fake's tracker hits (510, 400), SteamVR's (500, 380), at 20 px a degree
            tx, ty = 1.25 * r["truth"]["x"], 1.25 * r["truth"]["y"]
            self.assertAlmostEqual(q["own"]["deg"], math.hypot(510 - tx, 400 - ty) / 20, places=1)
            self.assertAlmostEqual(q["mmap1"]["deg"], math.hypot(500 - tx, 380 - ty) / 20, places=1)
        self.assertGreater(len((run / "gaze.jsonl").read_text().splitlines()), 50)

    def test_qml_loads(self):
        bench = gazebench.Bench(SCENARIOS, str(Path(self.tmp.name) / "runs"), images=False, gaze_prog=FAKE_GAZE)
        warnings = []
        try:
            engine = QQmlApplicationEngine()
            engine.warnings.connect(lambda ws: warnings.extend(w.toString() for w in ws))
            engine.rootContext().setContextProperty("bench", bench)
            engine.load(QUrl.fromLocalFile(str(BENCH / "bench.qml")))
            self.assertTrue(engine.rootObjects())
            self.wait(lambda: False, 0.3)
            self.assertGreater(bench.screen.get("w", 0), 0)
            bench.start("small")
            self.wait(lambda: False, 0.2)
            bench.click(1, 1, "left")
            self.wait(lambda: False, 0.2)
            bench.stop()
            self.wait(lambda: bench.page == "done", 1)
            self.wait(lambda: False, 0.5)
            self.assertEqual(warnings, [])
            del engine
        finally:
            bench.shutdown()


if __name__ == "__main__":
    unittest.main()
