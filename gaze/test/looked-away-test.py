#!/usr/bin/env python3
"""Offline test of the full calibration's looked-away check (gaze/gazecheck.py, Checks.looked_away):
a dot for our tracker where SteamVR's two eyes were more than LOOKED_AWAY from it, past their
usual offset in that calibration, isn't taken, and ft-eyes isn't asked. Live 2026-10-18: the
palm menu opened the Steam menu mid-calibration, and three dots were taken 17-35 degrees from
where the user looked (both trackers agreed). A SteamVR that is off by itself (its eyes in one
place whatever the dot) mustn't block the calibration: past AWAY_GIVE_UP dots in a row, the
check stops.

Drives Checks.capture with synthetic looks and a fake service: nothing reaches the live gaze
service, the pointer helper, ft-eyes, or SteamVR. HOME is a temp folder (checks.jsonl goes there).

  gaze/test/looked-away-test.py
"""
import os
import tempfile

HOME = tempfile.mkdtemp(prefix="ft-gaze-looked-away-test-")
os.environ["HOME"] = HOME  # before gazecal: its STATE follows HOME

import selectors  # noqa: E402
import shutil  # noqa: E402
import sys  # noqa: E402
import types  # noqa: E402

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import gazecheck  # noqa: E402

tag = f"ft_gaze_looked_away_test_{os.getpid()}"
gazecheck.PANEL = f"\0{tag}_panel"
gazecheck.POINTER = f"\0{tag}_helper"
gazecheck.EYES = f"\0{tag}_eyes"
logs = []
gazecheck.log = lambda msg: logs.append(msg)

failures = []


def check(label, got, want):
    ok = got == want
    print(f"{'ok  ' if ok else 'FAIL'}  {label}" + ("" if ok else f": got {got!r}, want {want!r}"), flush=True)
    if not ok:
        failures.append(label)


class Weights:
    def add(self, miss):
        pass


svc = types.SimpleNamespace(one_eye=None, weights={"own": Weights(), "steam": Weights()})
checks = gazecheck.Checks(svc, selectors.DefaultSelector())
asked = []
checks.ask_eyes = lambda command, timeout, done: asked.append(command)
DOTS = gazecheck.check_dots("full", True)


def new_check():
    checks.check = {"kind": "full", "own": True, "dots": DOTS, "i": 0, "tries": 0, "skipped": 0, "captured": 0,
                    "points": {}, "reasons": {}, "fit_reasons": set(), "note": "", "blind": False, "fusion": [],
                    "away_dots": [], "away_off": False, "steam_offsets": []}


def look(at, steam_off=(0.0, 0.0), n=30):
    """A look at `at` (yaw, pitch) by SteamVR's eyes (each this far from it, plus steam_off), our
    tracker's at the dot: the capture window as on_sample keeps it."""
    y, p = at[0] + steam_off[0], at[1] + steam_off[1]
    src = {"mmap1": {"unc": [0.001, 0.001], "open": [0.8, 0.8]}, "left": {"hy": y - 1.0, "hp": p},
           "right": {"hy": y + 1.0, "hp": p}, "own": {"ok": 1, "eyes": [list(at), list(at)]}}
    return [(0.0, {"t": 100.0 + k / 90, "src": src}, y, p) for k in range(n)]


def capture(i, at, steam_off=(0.0, 0.0)):
    """Dot i shown, a click while the eyes are at `at`: was ft-eyes asked (the check let it go on)?"""
    c = checks.check
    c["i"], c["done_at"] = i, None
    before = len(asked)
    checks.capture(look(at, steam_off))
    return len(asked) > before


def answer_ok():
    """ft-eyes takes the dot it was asked about (point_done's own path, with "ok")."""
    c = checks.check
    yaw, pitch, rnd = c["dots"][c["i"]]
    rec = {"true": [yaw, pitch], "steam": [(yaw, pitch), (yaw, pitch)], "own": True}
    checks.point_done(rec, [0.0, 0.0], "ok")


# Looking at the dots: each taken, as before.
new_check()
for i in range(3):
    check(f"dot {i + 1}: looking at it, ft-eyes is asked", capture(i, DOTS[i][:2]), True)
    answer_ok()
check("no notes for those", checks.check["note"], "")

# Looking 25 degrees off (the Steam menu, mid-calibration): not taken, and the panel says why.
dot = DOTS[3][:2]
check("dot 4: looked 25 degrees away: ft-eyes isn't asked", capture(3, dot, (25.0, 0.0)), False)
check("...the panel says why", "looked away" in checks.check["note"], True)
check("...and the dot stays for another try", (checks.check["i"], checks.check["tries"]), (3, 1))
check("...the reason is counted", checks.check["reasons"], {"you looked away from the dot (by SteamVR's eyes)": 1})
check("dot 4, second try, looking at it: taken", capture(3, dot), True)
answer_ok()

# 14 degrees off: under LOOKED_AWAY, taken (SteamVR's own error at the corners reached 9 live).
check("dot 5: 14 degrees off: taken", capture(4, DOTS[4][:2], (0.0, 14.0)), True)
answer_ok()

# A SteamVR that is off by itself, 20 degrees at every dot: the usual offset counts once it has
# AWAY_OFFSET_MIN dots, so every dot is taken.
new_check()
for i in range(gazecheck.AWAY_OFFSET_MIN):
    capture(i, DOTS[i][:2], (3.0, 0.0))   # a small offset first: these are taken
    answer_ok()
check("its usual offset in this calibration is learned", len(checks.check["steam_offsets"]), gazecheck.AWAY_OFFSET_MIN)
check("then 18.5 degrees the same way: 15.5 past the usual 3, refused",
      capture(3, DOTS[3][:2], (18.5, 0.0)), False)
check("and 14 degrees the same way: taken (11 past the usual 3)", capture(3, DOTS[3][:2], (14.0, 0.0)), True)
answer_ok()

# A SteamVR off by 20 degrees from the start: the first dots are refused, then the check stops
# (AWAY_GIVE_UP dots in a row) and the rest are taken.
new_check()
logs.clear()
check("SteamVR 20 degrees off, dot 1: refused", capture(0, DOTS[0][:2], (20.0, 0.0)), False)
check("dot 1, second try: refused, and skipped", (capture(0, DOTS[0][:2], (20.0, 0.0)), checks.check["skipped"]),
      (False, 1))
check("dot 2: refused", capture(1, DOTS[1][:2], (20.0, 0.0)), False)
check("dot 2 again: refused, and skipped", (capture(1, DOTS[1][:2], (20.0, 0.0)), checks.check["skipped"]), (False, 2))
check("dot 3: a third dot in a row: SteamVR is off, taken", capture(2, DOTS[2][:2], (20.0, 0.0)), True)
check("...and the log says the check is off", any("looked-away check is off" in m for m in logs), True)
answer_ok()
check("dot 4: taken (the check stays off)", capture(3, DOTS[3][:2], (20.0, 0.0)), True)

# Our tracker alone without SteamVR's eyes (one eye tracked, or SteamVR lost them): no check.
new_check()
svc.one_eye = 0
check("one eye tracked by SteamVR: no check, 30 degrees off is taken", capture(0, DOTS[0][:2], (30.0, 0.0)), True)
svc.one_eye = None

# The reason's words, for the note and a failed calibration's status.
check("reject_reason for it", gazecheck.reject_reason("looked away (25 deg by SteamVR)", None),
      ("looked away", "you looked away from the dot (by SteamVR's eyes)", False))

print("FAILED: " + ", ".join(failures) if failures else "all passed", flush=True)
shutil.rmtree(HOME, ignore_errors=True)
sys.exit(1 if failures else 0)
