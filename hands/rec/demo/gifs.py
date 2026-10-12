#!/usr/bin/env python3
"""The demo GIFs from a session's hand-picked trims: one per part.

  frame-job --local -- python3 hands/rec/demo/gifs.py SESSION [--part ID] [--width PX] [--fps N] [--lossy N]

SESSION/trims.json, written by hand from the contact sheets (sheets.py):
  {"7a": {"take": 2, "in": 1.4, "out": 9.6}, "2b": {"take": 1, "in": 0.8, "out": 10.2, "crop": [x, y, w, h]}, ...}
"in"/"out" are seconds in the take; "crop" (optional) is in the 1920x1080 headset view.
Writes SESSION/gifs/<part>.gif: 640 px wide, 10 fps, one palette per GIF, then gifsicle's lossy pass
when it's there (HANDDEMO_GIFSICLE or ~/.cache/gifwork/gifsicle-src/src/gifsicle), and prints sizes.
"""
import argparse
import json
import os
import shutil
import subprocess
import tempfile

GIFSICLE = os.environ.get("HANDDEMO_GIFSICLE") or shutil.which("gifsicle") or os.path.expanduser(
    "~/.cache/gifwork/gifsicle-src/src/gifsicle")
MAX_S = 15.0   # 10 s for most parts; 2b, 7a and 11a run longer


def make(video, out, t_in, t_out, crop, width, fps, lossy):
    filt = []
    if crop:
        filt.append("crop=%d:%d:%d:%d" % (crop[2], crop[3], crop[0], crop[1]))
    filt += ["fps=%d" % fps, "scale=%d:-2:flags=lanczos" % width]
    chain = ",".join(filt)
    with tempfile.TemporaryDirectory() as tmp:
        pal = os.path.join(tmp, "pal.png")
        raw = os.path.join(tmp, "raw.gif")
        common = ["-ss", "%.3f" % t_in, "-to", "%.3f" % t_out, "-i", video]
        subprocess.run(["ffmpeg", "-v", "error", "-y", *common, "-vf", chain + ",palettegen=stats_mode=full", pal],
                       check=True)
        subprocess.run(["ffmpeg", "-v", "error", "-y", *common, "-i", pal, "-lavfi",
                        chain + "[v];[v][1:v]paletteuse=dither=bayer:bayer_scale=4:diff_mode=rectangle",
                        "-loop", "0", raw], check=True)
        if os.access(GIFSICLE, os.X_OK):
            subprocess.run([GIFSICLE, "-O3", "--lossy=%d" % lossy, raw, "-o", out], check=True)
        else:
            shutil.copyfile(raw, out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("session")
    ap.add_argument("--part")
    ap.add_argument("--width", type=int, default=640)
    ap.add_argument("--fps", type=int, default=10)
    ap.add_argument("--lossy", type=int, default=60, help="gifsicle's lossy level (80 for smaller files)")
    a = ap.parse_args()
    with open(os.path.join(a.session, "trims.json")) as f:
        trims = json.load(f)
    os.makedirs(os.path.join(a.session, "gifs"), exist_ok=True)
    for part, t in trims.items():
        if a.part and part != a.part:
            continue
        video = os.path.join(a.session, "%s-take%d.mp4" % (part, t["take"]))
        length = t["out"] - t["in"]
        if length > MAX_S:
            print("%s: %.1f s, longer than %g s" % (part, length, MAX_S))
        out = os.path.join(a.session, "gifs", part + ".gif")
        make(video, out, t["in"], t["out"], t.get("crop"), a.width, a.fps, a.lossy)
        print("%-4s take %d  %5.1f-%5.1f s  %.2f MB" % (part, t["take"], t["in"], t["out"],
                                                       os.path.getsize(out) / 1e6), flush=True)


if __name__ == "__main__":
    main()
