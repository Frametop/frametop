#!/usr/bin/env python3
"""The gaze bench without a headset: the scenarios' targets, bench.qml against gazebench.Bench,
and whole runs recorded with stand-ins for ft-gaze (fake_ft_gaze.py), the panel
(fake_ft_gazepanel.py), the headset button (a FIFO), our tracker and the gaze service (their
sockets), into a temporary folder. Needs PySide6 (the dev container); the parts that need it are
skipped without it.

  scripts/in-box python3 gaze/bench/tests/test_bench.py
"""
import json
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
sys.path.insert(0, str(BENCH.parent.parent))
os.environ["FT_BENCH_EYES_SOCKET"] = f"ft_eyes_benchtest_{os.getpid()}"
os.environ["FT_BENCH_GAZED_SOCKET"] = f"ft_gazed_benchtest_{os.getpid()}"
os.environ["FT_BENCH_PANEL_SOCKET"] = f"ft_benchpanel_test_{os.getpid()}"
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
import benchrec  # noqa: E402
from recorder.button import EV_KEY, INPUT_EVENT, KEY_SELECT  # noqa: E402

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
FAKE_PANEL = HERE / "fake_ft_gazepanel.py"
OWN, STEAMVR = (-0.5, 0.0), (0.0, 1.0)   # fake_ft_gaze.py's gaze, head-relative degrees


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


class GazeStreamTest(unittest.TestCase):
    def test_restarts(self):
        """ft-gaze quits with SteamVR: it's started again, and the samples go on."""
        saved = benchrec.RESTART
        benchrec.RESTART = 0.2
        os.environ["FAKE_GAZE_QUIT_AFTER"] = "0.3"
        logs = []
        g = benchrec.GazeStream(prog=FAKE_GAZE, log=logs.append).start()
        try:
            end = time.monotonic() + 5
            while g.restarts < 2 and time.monotonic() < end:
                time.sleep(0.05)
            self.assertGreaterEqual(g.restarts, 2, logs)
            lines = g.lines
            time.sleep(0.2)
            self.assertGreater(g.lines, lines)
            self.assertIn("starting it again", logs[0])
        finally:
            del os.environ["FAKE_GAZE_QUIT_AFTER"]
            benchrec.RESTART = saved
            g.stop()
        self.assertFalse(g.reader.is_alive())


class ScenarioTest(unittest.TestCase):
    def setUp(self):
        if gazebench is None:
            self.skipTest(f"no PySide6: {NO_QT}")

    def test_targets(self):
        for sc in SCENARIOS["scenarios"]:
            ts = gazebench.make_trials(sc, random.Random(1))
            self.assertTrue(ts, sc["id"])
            for p in ts:
                self.assertTrue(-1e-9 <= p["u"] <= 1 + 1e-9 and -1e-9 <= p["v"] <= 1 + 1e-9, (sc["id"], p))
            if sc["id"] == "edges":
                self.assertEqual(len(ts), 16)
                self.assertTrue(all(p["u"] in (0, 1) or p["v"] in (0, 1) for p in ts))
            if sc["id"] == "hold":
                self.assertTrue(all(p["dwell"] == 3.0 for p in ts))
            if sc["id"] == "light":
                self.assertEqual({p["bg"] for p in ts}, set(sc["backgrounds"]))
            if sc["id"] == "steps":
                dirs = [(0.0, 0.0)] + [gazebench.panel_direction(p["u"], p["v"]) for p in ts]
                steps = [gazebench.angle(a, b) for a, b in zip(dirs, dirs[1:])]
                self.assertTrue(all(1.9 < d < 4.1 for d in steps), steps)
                self.assertTrue(all(abs(y) <= 12.01 and abs(p) <= 9.01 for y, p in dirs), dirs)
        rays = gazebench.make_trials(next(s for s in SCENARIOS["scenarios"] if s["id"] == "spokes"), random.Random(1))
        outer = [p for p in rays if p["u"] in (0, 1) or p["v"] in (0, 1)]
        self.assertEqual(len(outer), 8)  # the 1.0 ring reaches the edges and corners

    def test_panel_direction(self):
        d = gazebench.panel_direction
        self.assertEqual(d(0.5, 0.5), (0.0, 0.0))
        self.assertGreater(d(0, 0.5)[0], 30)    # u 0: left (+yaw)
        self.assertGreater(d(0.5, 0)[1], 20)    # v 0: up (+pitch)
        for u, v in ((0, 0), (1, 1), (0.2, 0.7), (0.9, 0.1)):
            back = gazebench.panel_place(*d(u, v))
            self.assertAlmostEqual(back[0], u, places=9)
            self.assertAlmostEqual(back[1], v, places=9)
        # Every scenario's dot shows whole: its ring (0.75 degrees out) is on the panel
        for sc in SCENARIOS["scenarios"]:
            for p in gazebench.make_trials(sc, random.Random(2)):
                yaw, pitch = d(p["u"], p["v"])
                for dy, dp in ((0.75, 0), (-0.75, 0), (0, 0.75), (0, -0.75)):
                    x, y = gazebench.panel_place(yaw + dy, pitch + dp, margin=0)
                    self.assertTrue(0 <= x <= 1 and 0 <= y <= 1, (sc["id"], p, dy, dp))


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
    """A warm-up played through with the button and the keys, recorded and summed up; a hold
    scenario ended by holding the button; bench.qml loaded on it."""

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
        self.panel_log = tmp / "panel.log"
        os.environ["FT_FAKE_PANEL_LOG"] = str(self.panel_log)
        self.fifo = tmp / "button"
        os.mkfifo(self.fifo)
        self.button = os.open(self.fifo, os.O_RDWR)   # (Linux: doesn't wait for the reader)

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
                                   "checks": {"gaze_mode": False, "headset_worn": True}})
            return "ok"

        self.eyes = FakeService(benchrec.EYES_SOCK, eyes)
        self.gazed = FakeService(benchrec.GAZED_SOCK, gazed)

    def tearDown(self):
        for k, v in self.saved.items():
            setattr(benchrec, k, v)
        self.eyes.stop()
        self.gazed.stop()
        os.close(self.button)
        self.tmp.cleanup()

    def wait(self, cond, timeout=5.0):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            APP.processEvents()
            if cond():
                return True
            time.sleep(0.02)
        return False

    def key(self, down):
        t = time.time()
        os.write(self.button, INPUT_EVENT.pack(int(t), int(t % 1 * 1e6), EV_KEY, KEY_SELECT, 1 if down else 0))

    def press(self, held=0.08):
        self.key(True)
        self.wait(lambda: False, held)
        self.key(False)

    def panel_said(self):
        return self.panel_log.read_text().splitlines() if self.panel_log.exists() else []

    def bench(self, base):
        bench = gazebench.Bench(SCENARIOS, str(base), images=False, gaze_prog=FAKE_GAZE, panel_prog=FAKE_PANEL,
                                button=str(self.fifo))
        self.assertTrue(self.wait(lambda: bench.gaze.latest() is not None), "no samples from the fake ft-gaze")
        self.assertTrue(self.wait(bench.panel.alive), bench.panel.error)
        return bench

    def test_run(self):
        base = Path(self.tmp.name) / "runs"
        bench = self.bench(base)
        try:
            self.assertTrue(self.wait(lambda: "Headset panel: ready" in bench.status), bench.status)
            self.assertIn(str(self.fifo), bench.status)
            bench.start("warmup")
            self.assertEqual((bench.page, bench.phase), ("run", "intro"))
            run = next(base.iterdir())
            self.assertTrue(self.wait(lambda: "title Warm-up" in self.panel_said()), self.panel_said())
            said = self.panel_said()
            self.assertIn("show rec", said)
            self.assertIn(f"log start {run}", said)
            self.press()                                # the intro: a press starts it
            self.assertTrue(self.wait(lambda: bench.phase == "target"))
            n = len(bench.trials)
            pressed_at = None
            for k in range(n):
                self.assertTrue(self.wait(lambda: bench.phase == "target"), (k, bench.phase))
                self.wait(lambda: False, 0.45)          # samples in the look window
                if k == 0:                               # the button marks it, at its down
                    pressed_at = benchrec.raw_now()
                    self.press()
                    self.assertTrue(self.wait(lambda: len(bench.results) == 1))
                    continue
                if k == 2:                               # two presses: back to 1, marked again
                    self.press()
                    self.wait(lambda: False, 0.1)
                    self.press()
                    self.assertTrue(self.wait(lambda: bench.i == 1 and bench.phase == "target"), (bench.i, bench.phase))
                    self.wait(lambda: False, 0.45)
                    bench.markKey()
                    self.assertTrue(self.wait(lambda: bench.phase == "target"))
                    self.wait(lambda: False, 0.45)
                if k == 4:
                    bench.skip()
                    continue
                bench.markKey()
            self.assertTrue(self.wait(lambda: bench.page == "done"))
            self.assertEqual(bench.phase, "end")
            self.assertIn("Our tracker", bench.summary)
            self.assertIn("log stop", self.panel_said())
            self.assertTrue(self.wait(lambda: "title Done" in self.panel_said()), self.panel_said()[-5:])
            self.press()                                 # closes the panel
            self.assertTrue(self.wait(lambda: self.panel_said()[-1] == "hide"))
            self.assertTrue(self.wait(lambda: "end" in json.loads((run / "run.json").read_text())))
        finally:
            bench.shutdown()
        info = json.loads((run / "run.json").read_text())
        self.assertEqual(info["scenario"]["id"], "warmup")
        self.assertEqual(info["display"], "panel")
        self.assertEqual(info["panel"]["log_rows"], "ok 3 100 1")
        self.assertEqual(info["button"], str(self.fifo))
        self.assertEqual(info["conf"], ["GAZE_TRACKER=own"])
        self.assertIsNotNone(info["last_lesson"])
        self.assertGreater(info["gaze_lines"], 50)
        self.assertEqual(info["finds_rows"], "ok 1")
        self.assertTrue((run / "start" / "eyes-calibration.json").exists())
        self.assertTrue((run / "targets.jsonl").exists())
        self.assertEqual(self.finds, [str(run / "own.jsonl")])
        rows = [json.loads(line) for line in (run / "trials.jsonl").read_text().splitlines()]
        kinds = [r["kind"] for r in rows]
        self.assertEqual(kinds[:2], ["message", "message-done"])
        self.assertIn("undo", kinds)
        trials = [r for r in rows if r["kind"] == "trial"]
        self.assertEqual(sum(1 for r in trials if r.get("skipped")), 1)
        by_button = [r for r in trials if r["click"]["by"] == "button"]
        self.assertEqual(len(by_button), 1)
        # The mark's time is the press's down, not when the press was sure (a double-press time later)
        self.assertLess(abs(by_button[0]["click"]["t"] - pressed_at), 0.2)
        for r in trials:
            if r.get("skipped"):
                continue
            target = gazebench.panel_direction(r["u"], r["v"])
            self.assertAlmostEqual(r["truth"]["yaw"], target[0], places=3)
            self.assertAlmostEqual(r["truth"]["pitch"], target[1], places=3)
            q = r["quick"]
            self.assertAlmostEqual(q["own"]["deg"], gazebench.angle(OWN, target), places=1)
            self.assertAlmostEqual(q["mmap1"]["deg"], gazebench.angle(STEAMVR, target), places=1)
        # The button's down showed the dot done before the press counted
        self.assertTrue(any(c.startswith("dot ") and c.endswith(" done") for c in self.panel_said()))
        self.assertGreater(len((run / "gaze.jsonl").read_text().splitlines()), 50)

    def test_hold(self):
        """Hold at the edges: a press before the ring fills doesn't count; holding the button ends it."""
        base = Path(self.tmp.name) / "runs"
        bench = self.bench(base)
        try:
            bench.start("hold")
            bench.markKey()
            self.assertEqual(bench.phase, "target")
            bench.markKey()                              # too soon
            self.assertTrue(self.wait(lambda: any(re.fullmatch(r"dot \S+ \S+ capture 0\.\d+", c) for c in self.panel_said())))
            self.key(True)
            self.assertTrue(self.wait(lambda: bench.page == "done", 3))
            self.key(False)
            self.assertIn("ended early", bench.summary)
            run = next(base.iterdir())
            self.assertTrue(self.wait(lambda: "end" in json.loads((run / "run.json").read_text())))
        finally:
            bench.shutdown()
        kinds = [json.loads(line)["kind"] for line in (run / "trials.jsonl").read_text().splitlines()]
        self.assertIn("early", kinds)
        self.assertNotIn("trial", kinds)

    def test_no_panel(self):
        bench = gazebench.Bench(SCENARIOS, str(Path(self.tmp.name) / "runs"), images=False, gaze_prog=FAKE_GAZE,
                                panel_prog="/nonexistent/ft-gazepanel", button=False)
        try:
            self.wait(lambda: bench.panel.error is not None, 2)
            bench.start("warmup")
            self.assertEqual(bench.page, "menu")
            self.assertIn("isn't running", bench.notice)
            self.assertTrue(self.wait(lambda: "Headset panel: not running" in bench.status), bench.status)
            self.assertIn("Headset button: not found", bench.status)
        finally:
            bench.shutdown()

    def test_qml_loads(self):
        bench = self.bench(Path(self.tmp.name) / "runs")
        warnings = []
        try:
            engine = QQmlApplicationEngine()
            engine.warnings.connect(lambda ws: warnings.extend(w.toString() for w in ws))
            engine.rootContext().setContextProperty("bench", bench)
            engine.load(QUrl.fromLocalFile(str(BENCH / "bench.qml")))
            self.assertTrue(engine.rootObjects())
            self.wait(lambda: False, 0.3)
            bench.start("steps")
            self.wait(lambda: False, 0.2)
            bench.markKey()
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
