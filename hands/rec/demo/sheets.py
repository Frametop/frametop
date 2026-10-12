#!/usr/bin/env python3
"""Contact sheets and the spoken part number for each take in a demo session, for trimming by hand.

  frame-job --local -- python3 hands/rec/demo/sheets.py SESSION [--part ID] [--every S] [--no-speech]

SESSION/sheets/<take>.jpg: one frame every S seconds (default 0.5), 6 across, each stamped with its
time in the take, so a trim's in and out points can be read off it. SESSION/heard.json: what
whisper.cpp heard in each take's first 8 s, beside the part the app recorded (a mismatch is printed).
The trims go into SESSION/trims.json for gifs.py (README.md).
"""
import argparse
import json
import os
import re
import subprocess
import tempfile

WHISPER = os.environ.get("HANDDEMO_WHISPER") or os.path.expanduser(
    "~/Desktop/Projects/frame-voice/vendor/whisper.cpp/build/bin/whisper-cli")
WHISPER_MODEL = os.environ.get("HANDDEMO_WHISPER_MODEL") or os.path.expanduser(
    "~/Desktop/Projects/frame-voice/models/ggml-small.en-q5_1.bin")
WORDS = {"one": "1", "two": "2", "three": "3", "four": "4", "five": "5", "six": "6", "seven": "7",
         "eight": "8", "nine": "9", "ten": "10", "eleven": "11", "twelve": "12", "thirteen": "13"}


def takes(session):
    with open(os.path.join(session, "takes.jsonl")) as f:
        return [json.loads(line) for line in f if line.strip()]


def duration(video):
    r = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", video],
                       capture_output=True, text=True)
    try:
        return float(r.stdout)
    except ValueError:
        return 60.0


def sheet(video, out, every):
    rows = max(1, min(20, int(duration(video) / every + 5) // 6))
    font = "/usr/share/fonts/noto/NotoSans-Regular.ttf"
    stamp = ("drawtext=fontfile=%s:" % font if os.path.exists(font) else "drawtext=") + \
        "text='%{pts\\:hms}':x=8:y=8:fontsize=28:fontcolor=yellow:box=1:boxcolor=black@0.6"
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", video, "-vf",
                    "fps=1/%g,scale=480:-2,%s,tile=6x%d:padding=4" % (every, stamp, rows),
                    "-q:v", "4", "-update", "1", "-frames:v", "1", out], check=True)


def spoken(video):
    """The words in the take's first 8 s (the mic), or None without whisper.cpp."""
    if not (os.access(WHISPER, os.X_OK) and os.path.exists(WHISPER_MODEL)):
        return None
    with tempfile.TemporaryDirectory() as tmp:
        wav = os.path.join(tmp, "a.wav")
        r = subprocess.run(["ffmpeg", "-v", "error", "-y", "-t", "8", "-i", video, "-vn", "-af", "loudnorm",
                            "-ar", "16000", "-ac", "1", wav])
        if r.returncode:
            return None
        env = dict(os.environ, LD_LIBRARY_PATH=os.path.dirname(WHISPER))   # its libwhisper sits beside it
        r = subprocess.run([WHISPER, "-ng", "-t", "2", "-nt", "-np", "-m", WHISPER_MODEL, "-f", wav],
                           capture_output=True, text=True, env=env)
        if r.returncode:
            print("whisper-cli: %s" % r.stderr.strip()[-200:])
            return None
        return " ".join(r.stdout.split())


def matches(part, text):
    t = text.lower()
    for word, digit in WORDS.items():
        t = re.sub(r"\b%s\b" % word, digit, t)
    t = re.sub(r"[^0-9a-z]", "", t)
    return part.lower() in t


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("session")
    ap.add_argument("--part")
    ap.add_argument("--every", type=float, default=0.5)
    ap.add_argument("--no-speech", action="store_true")
    a = ap.parse_args()
    os.makedirs(os.path.join(a.session, "sheets"), exist_ok=True)
    heard_path = os.path.join(a.session, "heard.json")
    heard = json.load(open(heard_path)) if os.path.exists(heard_path) else {}
    for t in takes(a.session):
        if not t.get("file") or (a.part and t["part"] != a.part):
            continue
        video = os.path.join(a.session, t["file"])
        name = os.path.splitext(t["file"])[0]
        if not os.path.exists(video):
            print("%s: missing" % t["file"])
            continue
        sheet(video, os.path.join(a.session, "sheets", name + ".jpg"), a.every)
        line = "%-12s %-6s %s" % (name, t["result"], "sheets/%s.jpg" % name)
        if not a.no_speech:
            text = spoken(video)
            if text is not None:
                heard[name] = {"part": t["part"], "heard": text, "ok": matches(t["part"], text)}
                line += "   heard: %r%s" % (text, "" if heard[name]["ok"] else "   <- NOT %s" % t["part"])
        print(line, flush=True)
    with open(heard_path, "w") as f:
        json.dump(heard, f, indent=1)


if __name__ == "__main__":
    main()
