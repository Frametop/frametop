#!/usr/bin/env python3
"""Offline test of a glance lesson ("lesson ... glance": a free mouse's click, ft-pointer.cpp's
"Glance") in ft-gazed: our tracker gets it as a weaker click, SteamVR's correction counts it for
GLANCE_WEIGHT, and it isn't one of the lessons after a quick check. ft-gazed's Service is built
without its sockets (a fake one records what would go to ft-eyes), and its lesson log is a
temporary file, so nothing reaches the live gaze service.

  gaze/test/glance-lesson-test.py
"""
import importlib.machinery
import importlib.util
import json
import os
import sys
import tempfile
from collections import deque
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
from gazecal import EyeWeights  # noqa: E402

loader = importlib.machinery.SourceFileLoader("ftgazed", os.path.join(HERE, "..", "ft-gazed"))
gazed = importlib.util.module_from_spec(importlib.util.spec_from_loader("ftgazed", loader))
loader.exec_module(gazed)

failures = []


def check(what, got, want):
    ok = got == want
    print(f"{'ok  ' if ok else 'FAIL'}  {what}" + ("" if ok else f": got {got!r}, want {want!r}"), flush=True)
    if not ok:
        failures.append(what)


tmp = Path(tempfile.mkdtemp(prefix="ft-glance-test-"))
gazed.LESSON_LOG = tmp / "pointer-lessons.jsonl"


class Sock:
    def __init__(self):
        self.sent = []

    def sendto(self, data, addr):
        self.sent.append(data.decode())


class Live:
    def __init__(self):
        self.added = []

    def add(self, s, model, mode):
        self.added.append(s)


class Steam:
    def worn(self):
        return True


def service(tracker):
    svc = gazed.Service.__new__(gazed.Service)
    svc.override, svc.tracker, svc.one_eye, svc.source, svc.mode = None, tracker, None, "mmap2", None
    svc.models = {name: type("M", (), {"samples": 5})() for name in gazed.SOURCES}
    svc.lives = {name: Live() for name in gazed.SOURCES}
    svc.correction = lambda name, hy, hp: (0.0, 0.0)
    svc.steam, svc.bias, svc.nudge_max = Steam(), "auto", gazed.NUDGE_MAX
    svc.weights = {t: EyeWeights(svc.bias) for t in gazed.TRACKERS}
    svc.eyes_sock = Sock()
    svc.counts = {"refused": 0, "lessons_taken": 0}
    svc.dirty = False
    # the gaze sent at t=100, raw (2, 1); each eye read about there
    svc.history = deque([(100.0, "2.000 1.000", [(2.5, 1.0), (1.5, 1.0)])])
    return svc


own = service("own")
rec = own.lesson(2.0, 1.0, 5.0, 2.0, glance=True)
check("our tracker: a glance goes to ft-eyes as a weaker click", own.eyes_sock.sent, ["click 100.000000 5.0000 2.0000 glance"])
check("...logged as a glance", (rec["how"], json.loads(gazed.LESSON_LOG.read_text().splitlines()[-1])["how"]), ("glance", "glance"))
own.lesson(2.0, 1.0, 5.0, 2.0)
check("...a nudge as a click, as before", own.eyes_sock.sent[-1], "click 100.000000 5.0000 2.0000")

steam = service("steam")
steam.lesson(2.0, 1.0, 5.0, 2.0, glance=True)
right = steam.lives[gazed.SIDES[0]].added
check("SteamVR's eyes: a glance counts for GLANCE_WEIGHT", (right[-1]["wy"], right[-1]["wp"]), (gazed.GLANCE_WEIGHT,) * 2)
steam.lesson(2.0, 1.0, 5.0, 2.0)
check("...a nudge for 1", right[-1]["wy"], 1.0)

# After a quick check: its next FIVE_COUNT lessons say whether it worked; a glance isn't one.
import gazecheck  # noqa: E402

checks = gazecheck.Checks.__new__(gazecheck.Checks)
checks.after_quick = []
checks.after_lesson({"how": "glance", "lesson_deg": 3.0})
check("a glance isn't one of the lessons after a quick check", checks.after_quick, [])
checks.after_lesson({"how": "nudge", "lesson_deg": 3.0})
check("...a nudge is", checks.after_quick, [3.0])

gazed.LESSON_LOG.unlink()
tmp.rmdir()
if failures:
    print("FAILED:", ", ".join(failures))
    sys.exit(1)
print("all ok")
