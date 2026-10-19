#!/usr/bin/env python3
"""Offline test of the fusion (gazecal.Fusion, ft-gazed's GAZE_FUSION): SteamVR's affine from
calibration dots, the clicks' offset per wear, which tracker carries the gaze when, saving and
loading fusion.json, the setting, and ft-gazed's live gaze with it on. ft-gazed's Service is
built without its sockets, and HOME is a temp folder, so nothing reaches the live gaze service.

  gaze/test/fusion-test.py
"""
import os
import tempfile

HOME = tempfile.mkdtemp(prefix="ft-fusion-test-")
os.environ["HOME"] = HOME  # before gazecal: its STATE, and frametop.conf, follow HOME

import importlib.machinery  # noqa: E402
import importlib.util  # noqa: E402
import math  # noqa: E402
import shutil  # noqa: E402
import sys  # noqa: E402
from collections import deque  # noqa: E402
from pathlib import Path  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
from gazecal import EyeWeights, Fusion, GazeLock  # noqa: E402

loader = importlib.machinery.SourceFileLoader("ftgazed", os.path.join(HERE, "..", "ft-gazed"))
gazed = importlib.util.module_from_spec(importlib.util.spec_from_loader("ftgazed", loader))
loader.exec_module(gazed)

failures = []


def check(what, got, want):
    if got != want:
        failures.append(what)
        print(f"FAIL {what}: got {got!r}, want {want!r}", flush=True)
    else:
        print(f"ok   {what}", flush=True)


def near(a, b, tol=1e-6):
    return a is not None and b is not None and all(abs(x - y) < tol for x, y in zip(a, b))


# --- gazecal.Fusion ---
f = Fusion()
check("not fitted: not ready, no SteamVR gaze", (f.ready(), f.steam_gaze((0, 0), (0, 0))), (False, None))
# SteamVR's eyes' mean reads 1 degree high everywhere: the affine's offset takes it out
f.fit([(y, p + 1, y, p) for y in (-20, -10, 0, 10, 20) for p in (-10, 0, 10)], {"how": "test"})
check("fitted on 15 dots: ready", (f.ready(), f.info["dots"], f.info["how"]), (True, 15, "test"))
check("an offset: taken out", near(f.steam_gaze((14.0, 6.0), (16.0, 6.0)), (15.0, 5.0)), True)
check("...leaving nothing on its dots", max(f.misses()) < 1e-6, True)
# ...and 10 % wide too: the gain is taken out as well without the ridge (with it, a little short:
# on 2026-10-17's runs it fitted other sittings better)
wide = Fusion()
wide.RIDGE = 0.0
wide.fit([(1.1 * y, 1.1 * p + 1, y, p) for y in (-20, -10, 0, 10, 20) for p in (-10, 0, 10)])
check("a gain, no ridge: taken out", near(wide.steam_gaze((1.1 * 15 - 1, 1.1 * 5 + 1), (1.1 * 15 + 1, 1.1 * 5 + 1)),
                                         (15.0, 5.0), 1e-6), True)
wide.RIDGE = Fusion.RIDGE
wide.fit(wide.points)
check("with the ridge: most of it", 0.0 < max(wide.misses()) < 0.6, True)
check("one SteamVR eye: no gaze from it", f.steam_gaze((1.0, 2.0), None), None)
check("the mean of the eyes", Fusion.mean((0.0, 2.0), (2.0, 4.0)), (1.0, 3.0))

# Who carries the gaze
ours, steam = (2.0, 0.0), (0.0, 2.0)
check("both our eyes and SteamVR's: half and half", f.combine(ours, 2, steam), (1.0, 1.0))
check("one of our eyes: SteamVR's", f.combine(ours, 1, steam), steam)
check("none of ours: SteamVR's", f.combine(None, 0, steam), steam)
check("no SteamVR gaze: ours", f.combine(ours, 1, None), ours)
check("a share of 1: ours where both are found", f.combine(ours, 2, steam, share=1.0), ours)

# The clicks' offset: per wear, a median, stale clicks left out
g = Fusion()
g.fit([(y, p, y, p - 1) for y in (-10, 0, 10) for p in (-10, 0, 10)])   # 1 degree low... corrected
g.wear = 100.0
check("no clicks: no offset", g.offset(), (0.0, 0.0))
check("a click 0.5 off: its miss", round(g.click((0.0, 0.0), (0.0, 0.0), 0.5, -1.0), 3), 0.5)
check("...and it's the offset", near(g.offset(), (0.5, 0.0)), True)
check("a click 20 degrees off (a look elsewhere): not taken", (round(g.click((0, 0), (0, 0), 20.0, -1.0), 1),
                                                              near(g.offset(), (0.5, 0.0))), (20.0, True))
g.wear = 200.0
check("the headset on again: the old wear's clicks don't count", g.offset(), (0.0, 0.0))
check("steam_gaze without the clicks", near(g.steam_gaze((3.0, 3.0), (3.0, 3.0), clicks=False), (3.0, 2.0)), True)

# Saving and loading
path = Path(HOME) / "fusion.json"
f.save(path)
h = Fusion.load(path)
check("saved and loaded: the same correction", near(h.steam_gaze((5.0, 5.0), (7.0, 5.0)), f.steam_gaze((5.0, 5.0), (7.0, 5.0))),
      True)
check("...and its info", (h.info["how"], h.info["dots"], len(h.points)), ("test", 15, 15))
check("a missing file: None", Fusion.load(Path(HOME) / "none.json"), None)
path.write_text("{ broken")
check("a broken file: None", Fusion.load(path), None)

# --- GAZE_FUSION ---
conf = Path(HOME) / ".config" / "frametop.conf"
conf.parent.mkdir(parents=True, exist_ok=True)
check("no frametop.conf: off", gazed.read_fusion(), False)
for text, want in (("GAZE_FUSION=on\n", True), ("GAZE_FUSION = ON  # yes\n", True), ("GAZE_FUSION=off\n", False),
                   ("GAZE_FUSION=yes\n", False), ("# GAZE_FUSION=on\n", False), ("GAZE_TRACKER=own\n", False)):
    conf.write_text(text)
    check(f"frametop.conf {text.strip()!r}", gazed.read_fusion(), want)


# --- ft-gazed's live gaze: the parts of Service the samples go through, no sockets ---
def service(fusion_on=True, one=None):
    svc = gazed.Service.__new__(gazed.Service)
    svc.override, svc.tracker, svc.source, svc.one_eye = None, "own", "mmap1", one
    svc.counts = dict.fromkeys(("samples", "sent", "blinks", "one_eye", "one_eye_used", "lost_left", "lost_right",
                                "looking_down", "dropped", "fused", "steam_only"), 0)
    svc.opens, svc.lost = (deque(maxlen=90), deque(maxlen=90)), [False, False]
    svc.fix, svc.last_sample, svc.low_until = GazeLock(radius=1.0), 0.0, [0.0, 0.0]
    svc.bridge = gazed.EyeBridge()
    svc.weights = {"own": EyeWeights()}
    svc.fusion_on = fusion_on
    svc.fusion = Fusion()
    svc.fusion.fit([(y, p + 1, y, p) for y in (-10, 0, 10) for p in (-10, 0, 10)])   # SteamVR 1 degree high
    svc.checks = type("Checks", (), {"active": False, "pending": None})()
    svc.sent = []
    svc.send = lambda t, hy, hp, rhy, rhp, eyes: svc.sent.append((round(hy, 3), round(hp, 3), eyes))
    return svc


def feed(svc, n=30, own_eyes=((4.0, 2.0), (4.0, 2.0)), steam=True, opens=(0.8, 0.8), unc=(0.001, 0.001)):
    for i in range(n):
        src = {"mmap1": {"unc": list(unc), "open": list(opens)},
               "own": {"hy": 4.0, "hp": 2.0, "eyes": [list(e) if e else None for e in own_eyes]}}
        if steam:
            src["left"], src["right"] = {"hy": 1.0, "hp": 3.0}, {"hy": 3.0, "hp": 3.0}   # mean (2, 3): corrected (2, 2)
        svc.on_eyes_sample({"t": i / 90, "src": src}, True)


svc = service()
check("fusing with our tracker, on, and fitted", svc.fusing, True)
check("ft-gaze prints SteamVR's eyes too", svc.wanted_sources(), "own,mmap1,left,right")
feed(svc)
check("both our eyes: half ours (4, 2), half SteamVR's corrected (2, 2)", svc.sent[-1][:2], (3.0, 2.0))
check("...counted as fused", (svc.counts["fused"], svc.counts["steam_only"]), (30, 0))
check("...and the eyes sent with it are ours (for lessons)", svc.sent[-1][2], [(4.0, 2.0), (4.0, 2.0)])
svc = service()
feed(svc, own_eyes=((4.0, 2.0), None))
check("one of our eyes: SteamVR's", svc.sent[-1][:2], (2.0, 2.0))
check("...counted", (svc.counts["steam_only"], svc.counts["one_eye"]), (30, 30))
svc = service()
feed(svc, own_eyes=(None, None))
check("neither of ours: SteamVR's carries it", (len(svc.sent), svc.sent[-1][:2] if svc.sent else None), (30, (2.0, 2.0)))
svc = service()
feed(svc, steam=False)
check("SteamVR's eyes missing: ours", svc.sent[-1][:2], (4.0, 2.0))
svc = service()
feed(svc, n=60, unc=(0.02, 0.001))
check("SteamVR lost its left eye: ours", svc.sent[-1][:2], (4.0, 2.0))
svc = service()
feed(svc, n=60, opens=(0.0, 0.0))
check("a blink (both lids down): nothing", (svc.sent, svc.counts["blinks"]), ([], 60))
svc = service(one=1)
feed(svc)
check("SteamVR tracking one eye: no fusion", (svc.sent[-1][:2], svc.counts["fused"]), ((4.0, 2.0), 0))
svc = service(fusion_on=False)
check("GAZE_FUSION off: not fusing, SteamVR's eyes not asked for", (svc.fusing, svc.wanted_sources()),
      (False, "own,mmap1"))
feed(svc)
check("...ours alone", svc.sent[-1][:2], (4.0, 2.0))
svc = service()
svc.fusion = Fusion()
check("no fitted fusion: not fusing", svc.fusing, False)
svc = service()
svc.tracker = "steam"
svc.models = {name: gazed.Correction() for name in gazed.SOURCES}
check("SteamVR's tracker: no fusion", svc.fusing, False)

shutil.rmtree(HOME, ignore_errors=True)
print("FAILED: " + ", ".join(failures) if failures else "all passed")
sys.exit(1 if failures else 0)
