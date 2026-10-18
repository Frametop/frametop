#!/usr/bin/env python3
"""Offline test of ft-eyes' size guard (eyes_pupil.SizeGuard, EYES_SIZE_GUARD): synthetic frames
fed to its eyes as the live loop does. A find two thirds of the pupil's usual size in one eye,
while the other's is as usual, counts as not found: no gaze from it, the search stays on the
last pupil, the finds log marks it "guard". With the guard off, it's taken. Needs the
tracker's Python (numpy, OpenCV); no cameras or SteamVR.

  scripts/in-box /opt/frametop/venv/bin/python gaze/test/size-guard-test.py
"""
import importlib.machinery
import importlib.util
import json
import os
import sys
import tempfile
from pathlib import Path

import cv2
import numpy as np

HOME = tempfile.mkdtemp(prefix="ft-eyes-size-guard-test-")
TRACKER = Path(__file__).resolve().parents[1] / "tracker"


def load(rule):
    os.environ.update(HOME=HOME, FT_EYES_CAMS="/nonexistent", FT_EYES_STATE=os.path.join(HOME, "eyes"),
                      FT_EYES_MODEL="classic", FT_EYES_SIZE_GUARD=rule)
    loader = importlib.machinery.SourceFileLoader(f"fteyes_{rule}", str(TRACKER / "ft-eyes"))
    mod = importlib.util.module_from_spec(importlib.util.spec_from_loader(loader.name, loader))
    loader.exec_module(mod)
    return mod


def frame(a, b):
    img = np.full((400, 512), 100, np.uint8)
    cv2.ellipse(img, (256, 200), (a, b), 0, 0, 360, 0, -1)
    return img


failed = 0


def check(ok, what):
    global failed
    print(("ok   " if ok else "FAIL ") + what)
    failed += not ok


for rule, refused in (("both", True), ("off", False)):
    fe = load(rule)
    tracker = fe.Tracker()
    log = os.path.join(HOME, f"finds-{rule}.jsonl")
    tracker.command(f"finds start {log}")
    right, left = tracker.eyes
    t = 100.0
    for i in range(40):           # both pupils as usual
        right.feed(t, frame(30, 24))
        left.feed(t + 0.001, frame(30, 24))
        t += 1 / 90
    last = right.last
    left.feed(t, frame(30, 24))
    right.feed(t + 0.001, frame(20, 15))   # 0.67 of usual, the left eye as usual
    tracker.command("finds stop")
    rows = [json.loads(x) for x in open(log)]
    check(tracker.status()["size_guard"] == rule, f"{rule}: status says the rule")
    check((right.refused == 1) == refused, f"{rule}: the small find refused: {right.refused}")
    check((right.last is last) == refused, f"{rule}: the search stays on the last pupil")
    check(bool(rows[-1].get("guard")) == refused, f"{rule}: the finds log marks it: {rows[-1]}")
    check(abs(rows[-1]["a"] - 20) < 1.5, f"{rule}: ...with its size")
    check(left.refused == 0 and sum(1 for r in rows if r.get("guard")) == (1 if refused else 0),
          f"{rule}: nothing else refused")

print(f"{'FAILED' if failed else 'ok'}: size guard")
sys.exit(1 if failed else 0)
