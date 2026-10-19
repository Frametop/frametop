#!/usr/bin/env python3
"""A stand-in for ft-gaze in the bench's tests: 90 lines a second, the head facing the middle of
a 1000 x 800 screen (number 2) at 20 px per degree, our tracker 0.5 degrees right of straight
ahead and SteamVR 1 degree up. Quits when stdin closes, as ft-gaze --watch-stdin, or after
$FAKE_GAZE_QUIT_AFTER seconds."""
import json
import os
import sys
import threading
import time

PPD = 20.0


def hit(hy, hp):
    return {"s": 2, "x": 500 - PPD * hy, "y": 400 - PPD * hp, "j": [-PPD, 0.0, 0.0, -PPD], "dpp": 1 / PPD}


def watch():
    while sys.stdin.buffer.read(4096):
        pass
    os._exit(0)


threading.Thread(target=watch, daemon=True).start()
quit_after = float(os.environ.get("FAKE_GAZE_QUIT_AFTER", "inf"))   # s: then exit, as when SteamVR quits
start = time.monotonic()
n = 0
while time.monotonic() - start < quit_after:
    t = time.clock_gettime(time.CLOCK_MONOTONIC_RAW)
    own = {"hy": -0.5, "hp": 0.0, "hit": hit(-0.5, 0.0), "eyes": [[-0.5, 0.0], [-0.5, 0.0]],
           "ehit": [hit(-0.5, 0.0), hit(-0.5, 0.0)]}
    s = {"t": t, "age": 5, "n": n, "head": {"yaw": 0, "pitch": 0, "hit": hit(0, 0)},
         "src": {"action": {"ok": 0}, "mmap1": {"hy": 0.0, "hp": 1.0, "hit": hit(0.0, 1.0)}, "mmap2": {"ok": 0},
                 "left": {"hy": 0.0, "hp": 1.0, "hit": hit(0.0, 1.0)}, "right": {"hy": 0.0, "hp": 1.0, "hit": hit(0.0, 1.0)},
                 "own": own}, "eye": None}
    try:
        sys.stdout.write(json.dumps(s) + "\n")
        sys.stdout.flush()
    except BrokenPipeError:
        break
    n += 1
    time.sleep(1 / 90)
