#!/usr/bin/env python3
"""The eye recorder's sound cues: plain beeps for steps done with the eyes shut, where the panel's
words can't be seen, and for moving the headset by hand, where the timing matters. A round script names one as a cue's third field (session.py: "cues":
[[word, seconds, sound]...]); it plays as the cue starts, and prompts.jsonl's "cue" line says which.

  blink   a short high beep: blink once
  close   a beep: close your eyes
  open    the same beep again: open them
  go      a higher beep: move the headset now (the cue word says how)
  done    a low, longer one: let go of it

No voice, no sound files: the beeps are made here (a sine with a short fade, so it doesn't click)
into the run folder, and played with PipeWire's pw-play on the default output (the headset's
speakers). A beep starts some tens of ms after its cue line (pw-play's start and its 20 ms
latency); a person's reaction to it takes several times that.

  sounds.py              play each one, a second apart
  sounds.py NAME...      play those
"""
import math
import os
import shutil
import struct
import subprocess
import sys
import time
import wave

RATE = 48000
VOLUME = 0.6        # pw-play's stream volume; the beep itself is at half scale
# name: (pitch Hz, seconds)
SOUNDS = {
    "blink": (1000.0, 0.09),
    "close": (660.0, 0.18),
    "open": (660.0, 0.18),
    "go": (880.0, 0.14),
    "done": (440.0, 0.3),
}
FADE_S = 0.005


def _dir():
    d = os.path.join(os.environ.get("XDG_RUNTIME_DIR") or "/run/user/%d" % os.getuid(), "frametop", "eyerec-sounds")
    os.makedirs(d, exist_ok=True)
    return d


def wav_path(name):
    """The sound's WAV file, made the first time (and again when its pitch or length changed)."""
    hz, secs = SOUNDS[name]
    path = os.path.join(_dir(), "%s-%d-%d.wav" % (name, hz, secs * 1000))
    if not os.path.exists(path):
        n, fade = int(RATE * secs), int(RATE * FADE_S)
        frames = bytearray()
        for i in range(n):
            env = min(1.0, i / fade, (n - 1 - i) / fade)
            frames += struct.pack("<h", int(0.5 * 32767 * env * math.sin(2 * math.pi * hz * i / RATE)))
        with wave.open(path + ".tmp", "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(RATE)
            w.writeframes(bytes(frames))
        os.replace(path + ".tmp", path)
    return path


class Player:
    """Plays sounds without waiting for them. Off (dry runs, tests): it only says what it would play.
    Without pw-play, or when a sound fails, it logs that once and the round goes on silent."""

    def __init__(self, on=True, log=None):
        self.log = log or (lambda text: None)
        self.pw_play = shutil.which("pw-play") if on else None
        self.procs = []
        self.warned = False
        if on and not self.pw_play:
            self._warn("no pw-play: the sound cues are off")
        if self.pw_play:
            for name in SOUNDS:     # made now, not while a step runs
                wav_path(name)

    def _warn(self, text):
        if not self.warned:
            self.warned = True
            self.log(text)

    def play(self, name):
        if name not in SOUNDS:
            raise ValueError("no sound %r" % name)
        if not self.pw_play:
            return False
        self.procs = [p for p in self.procs if p.poll() is None]
        env = dict(os.environ, XDG_RUNTIME_DIR=os.environ.get("XDG_RUNTIME_DIR") or "/run/user/%d" % os.getuid())
        try:
            self.procs.append(subprocess.Popen(
                [self.pw_play, "--latency", "20ms", "--volume", str(VOLUME), "--media-role", "Notification",
                 wav_path(name)], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                env=env))
            return True
        except OSError as e:
            self._warn("can't play the sound cues: %s" % e)
            return False

    def close(self):
        for p in self.procs:
            try:
                p.wait(timeout=1)
            except subprocess.TimeoutExpired:
                p.kill()
        self.procs = []


def main(argv):
    names = argv or list(SOUNDS)
    bad = [n for n in names if n not in SOUNDS]
    if bad:
        sys.exit("no sound %s; there are: %s" % (", ".join(bad), ", ".join(SOUNDS)))
    p = Player(log=lambda text: print(text, file=sys.stderr))
    for i, name in enumerate(names):
        if i:
            time.sleep(1.0)
        print(name)
        p.play(name)
    p.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
