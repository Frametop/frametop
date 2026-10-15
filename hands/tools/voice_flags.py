#!/usr/bin/env python3
"""A testing session take's flags: the headset button's marks (prompts.jsonl "mark") and what the
person said (voice.bin, hands/rec/voicelog.py), each with what Frametop did with the hands around
it (events.jsonl, hands/rec/eventlog.py), in time order.

  voice_flags.py TAKE_DIR [--out FLAGS.jsonl] [--server URL] [--before S] [--after S]

The voice is cut into bursts of speech by its level (100 ms windows over the noise floor, gaps
under GAP_S joined), and each burst is transcribed on its own by a whisper.cpp server on this
machine (default frame-voice's, http://127.0.0.1:8178): its times stay exact, and whisper never
hears the silence it makes words up for ("Thank you. Bye."). Only a loopback address is accepted,
so the voice never leaves the machine. A note within PAIR_S of a mark says which mark it's about
("mark": n).

Each flag, one JSON line in --out (and a line each on stdout):
  {"t": ns (CLOCK_MONOTONIC, the take's clock), "s": seconds into the take, "source": "mark" | "voice",
   "n": the mark's number | "text", "end_t", "mark": the nearest mark's number or null,
   "events": [events from `before` s ahead of it to `after` s after it (a note: after its end)]}

Standard library only.
"""
import argparse
import array
import ipaddress
import json
import math
import os
import sys
import tempfile
import urllib.parse
import urllib.request
import uuid
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "rec"))
import eventlog  # noqa: E402
import voicelog  # noqa: E402

SERVER = "http://127.0.0.1:8178"
GAP_S = 1.0       # a pause shorter than this stays in the burst
PAD_S = 0.3       # kept around each burst, so its first and last words aren't cut
MIN_RMS = 150     # s16; a burst's windows are over this and over 4x the noise floor
PAIR_S = 5.0
# whisper's words for silence and noise, not speech
NOT_SPEECH = ("[blank_audio]", "(silence)", "[silence]", "[music]", "(music)", "[noise]", "(noise)",
              "[inaudible]", "(inaudible)", "[ silence ]")


def loopback(url):
    """True for an http URL whose host is this machine."""
    u = urllib.parse.urlsplit(url)
    if u.scheme != "http" or not u.hostname:
        return False
    if u.hostname == "localhost":
        return True
    try:
        return ipaddress.ip_address(u.hostname).is_loopback
    except ValueError:
        return False


def transcribe(wav, server):
    """[(start_s, end_s, text)] of the WAV, from the whisper.cpp server's verbose_json."""
    boundary = uuid.uuid4().hex
    with open(wav, "rb") as f:
        audio = f.read()
    parts = []
    for name, value in (("response_format", "verbose_json"), ("temperature", "0")):
        parts.append(f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"\r\n\r\n{value}\r\n'.encode())
    parts.append(f'--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="voice.wav"\r\n'
                 f'Content-Type: audio/wav\r\n\r\n'.encode() + audio + b"\r\n")
    parts.append(f"--{boundary}--\r\n".encode())
    req = urllib.request.Request(server.rstrip("/") + "/inference", data=b"".join(parts),
                                 headers={"Content-Type": f"multipart/form-data; boundary={boundary}"})
    with urllib.request.urlopen(req, timeout=600) as r:
        out = json.load(r)
    segs = []
    for s in out.get("segments", []):
        text = (s.get("text") or "").strip()
        if text and text.lower() not in NOT_SPEECH:
            segs.append((float(s["start"]), float(s["end"]), text))
    return segs


def bursts(samples, rate, gap=GAP_S):
    """[(start, end)] sample ranges of speech in samples (an array of s16)."""
    win = rate // 10
    rms = [math.sqrt(sum(v * v for v in samples[i:i + win]) / win) for i in range(0, len(samples) - win + 1, win)]
    quiet = sorted(rms)
    floor = quiet[len(quiet) // 10] if quiet else 0   # the quietest tenth: silence between notes
    loud = max(MIN_RMS, 4 * floor)
    out = []
    for k, r in enumerate(rms):
        if r < loud:
            continue
        if out and k - out[-1][1] <= gap * 10:
            out[-1][1] = k + 1
        else:
            out.append([k, k + 1])
    pad = int(PAD_S * rate)
    return [(max(0, a * win - pad), min(len(samples), b * win + pad)) for a, b in out]


def notes(wav, server, tmp):
    """[(start_s, end_s, text)] of the speech in wav, a burst at a time."""
    with wave.open(wav) as w:
        rate = w.getframerate()
        samples = array.array("h", w.readframes(w.getnframes()))
    out = []
    for a, b in bursts(samples, rate):
        part = os.path.join(tmp, "burst.wav")
        with wave.open(part, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(rate)
            w.writeframes(samples[a:b].tobytes())
        text = " ".join(t for _, _, t in transcribe(part, server))
        if text:
            out.append((a / rate, b / rate, text))
    return out


def read_jsonl(path):
    out = []
    try:
        with open(path) as f:
            for line in f:
                try:
                    e = json.loads(line)
                except ValueError:
                    continue
                if isinstance(e, dict) and isinstance(e.get("t"), int):
                    out.append(e)
    except OSError:
        pass
    return out


def flags(take, server=SERVER, before=5.0, after=2.0):
    """The take's flags (see the top), in time order."""
    with open(os.path.join(take, "take.json")) as f:
        start = json.load(f).get("started_ns", 0)
    marks = [e for e in read_jsonl(os.path.join(take, "prompts.jsonl")) if e.get("event") == "mark"]
    events = sorted(read_jsonl(os.path.join(take, "events.jsonl")), key=lambda e: e["t"])
    out = [{"t": m["t"], "source": "mark", "n": m.get("n")} for m in marks]
    voice = os.path.join(take, "voice.bin")
    if os.path.exists(voice):
        with tempfile.TemporaryDirectory(prefix="voice-flags-") as tmp:
            wav = os.path.join(tmp, "voice.wav")
            try:
                first = voicelog.to_wav(voice, wav)
            except ValueError:
                first = None   # no sound recorded
            if first is not None:
                for a, b, text in notes(wav, server, tmp):
                    out.append({"t": first + int(a * 1e9), "end_t": first + int(b * 1e9),
                                "source": "voice", "text": text})
    for f in out:
        f["s"] = round((f["t"] - start) / 1e9, 2) if start else None
        if f["source"] == "voice":
            near = [m for m in marks if abs(m["t"] - f["t"]) <= PAIR_S * 1e9]
            f["mark"] = min(near, key=lambda m: abs(m["t"] - f["t"])).get("n") if near else None
        lo, hi = f["t"] - int(before * 1e9), f.get("end_t", f["t"]) + int(after * 1e9)
        f["events"] = [e for e in events if lo <= e["t"] <= hi]
    out.sort(key=lambda f: f["t"])
    return out


def line(f):
    """'  34.1 s  mark 1: poke missed, grab-that done' or '  36.0 s  voice (mark 1): "..."'."""
    what = eventlog.describe(eventlog.summary(f["events"]))
    s = "%7.1f s" % f["s"] if f["s"] is not None else "      ? s"
    if f["source"] == "mark":
        return "%s  mark %s: %s" % (s, f.get("n"), what)
    pair = " (mark %s)" % f["mark"] if f.get("mark") is not None else ""
    return '%s  voice%s: "%s"  [%s]' % (s, pair, f["text"], what)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("take", help="a testing session's take directory (voice.bin, prompts.jsonl, events.jsonl)")
    ap.add_argument("--out", help="write the flags here, one JSON line each")
    ap.add_argument("--server", default=SERVER, help="whisper.cpp server on this machine (default %(default)s)")
    ap.add_argument("--before", type=float, default=5.0, help="events this many seconds ahead of a flag (default 5)")
    ap.add_argument("--after", type=float, default=2.0, help="and this many after it (default 2)")
    a = ap.parse_args()
    if not loopback(a.server):
        sys.exit("--server must be on this machine (127.0.0.1, ::1 or localhost): the voice stays here")
    out = flags(a.take, a.server, a.before, a.after)
    for f in out:
        print(line(f))
    if a.out:
        with open(a.out, "w") as fh:
            for f in out:
                fh.write(json.dumps(f, separators=(", ", ": ")) + "\n")
    print("%d marks, %d spoken notes" % (sum(f["source"] == "mark" for f in out),
                                         sum(f["source"] == "voice" for f in out)), file=sys.stderr)


if __name__ == "__main__":
    main()
