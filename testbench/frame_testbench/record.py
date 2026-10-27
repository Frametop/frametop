"""Recordings of the trackers' output, for the fake trackers to play back (frame_testbench.fakes).

Only what the trackers publish is recorded, never a camera image, and only by reading:

- gaze.jsonl: SteamVR's eye-server.mmap and our tracker's frametop-eyes-gaze, a line per new
  sample. The first line says what it is ({"format": "frame-testbench-gaze", "version": 1});
  each after it has "t" (seconds since the start) and "mmap" (left1, right1, fix1, left2,
  right2, var1, var2, open, meas, as ft-gaze names them) or "own" (yaw, pitch, flags, eyes,
  slip). null is NaN, which our tracker writes for an eye it can't see.
- hands.fhdump: ft-hands' hands and gestures files after each new set, in ft-handreplay
  --publish-dump's format: "FHDUMP02", the two files' sizes (u32 each), then per set both
  files' bytes and an int64 time (ns, CLOCK_MONOTONIC). So a dump made offline from a camera
  recording plays back the same way.

Gaze is how someone's eyes moved: recordings stay on this machine, in the testbench's ignored
artifacts/ folder, and never go into the repo or anywhere else.
"""
import json
import math
import mmap
import os
import shutil
import struct
import time

from . import fakes

EYE_SERVER = '/dev/shm/eye-server.mmap'
OWN_GAZE = '/dev/shm/frametop-eyes-gaze'


def read_only(path, size=None):
    fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC)
    try:
        length = os.fstat(fd).st_size if size is None else size
        return mmap.mmap(fd, length, mmap.MAP_SHARED, mmap.PROT_READ)
    finally:
        os.close(fd)


class EyeServer:
    """SteamVR's eye-server.mmap, read the way ft-gaze reads it (gaze/ft-gaze.cpp: EyeFile)."""

    def __init__(self, path=None):
        path = path or EYE_SERVER
        self.m = read_only(path)
        if len(self.m) < 0x1f8:
            raise ValueError(f'{path} is too small for an eye tracker file')
        self.shift = None

    def get(self, off, fmt):
        return struct.unpack_from('<' + fmt, self.m, off)

    def fits(self, shift):
        t = self.get(0x157 + shift, 'd')[0]
        unit = lambda off: 0.81 < sum(v * v for v in self.get(off + shift, '3f')) < 1.21
        return abs(t - fakes.raw_now()) < 2 and unit(0x15f) and unit(0x16b), t

    def detect(self, wait=1.0):
        """Which layout is live: the one whose time is near the clock, whose eyes are unit
        vectors, and whose time moves on."""
        first = {s: self.fits(s) for s in fakes.SHIFTS}
        end = time.monotonic() + wait
        while time.monotonic() < end:
            time.sleep(0.004)
            for s, (ok, t) in first.items():
                now_ok, now_t = self.fits(s)
                if ok and now_ok and now_t > t:
                    self.shift = s
                    return s
        raise RuntimeError("SteamVR's eye tracker isn't writing samples in a layout ft-gaze knows")

    def sample(self):
        """A consistent copy of the latest sample, or None if it kept changing under us."""
        for _ in range(4):
            n0, t0 = self.get(0x38, 'I')[0], self.get(0x157 + self.shift, 'd')[0]
            values = {name: list(self.get(off + self.shift, f'{count}f')) for name, off, count in fakes.MMAP_FIELDS}
            if self.get(0x38, 'I')[0] == n0 and self.get(0x157 + self.shift, 'd')[0] == t0:
                return n0, values
        return None


class OwnGaze:
    """Our tracker's frametop-eyes-gaze (gaze/ft-gaze.cpp: OwnFile)."""

    def __init__(self, path=None):
        self.m = read_only(path or OWN_GAZE, 128)

    def sample(self):
        for _ in range(4):
            s0 = self.m[0:4]
            if struct.unpack('<I', s0)[0] & 1:
                continue
            version, _, yaw, pitch, flags, n = struct.unpack_from('<IdffII', self.m, 4)
            values = {name: list(struct.unpack_from(f'<{count}f', self.m, off)) for name, off, count in fakes.OWN_FIELDS}
            if self.m[0:4] == s0 and version == 1:
                return n, dict(values, yaw=yaw, pitch=pitch, flags=flags)
        return None


def seqlock_copy(m, size):
    for _ in range(8):
        s0 = struct.unpack_from('<Q', m, 16)[0]
        if s0 & 1:
            time.sleep(0.0005)
            continue
        data = bytes(m[:size])
        if struct.unpack_from('<Q', m, 16)[0] == s0:
            return s0, data
    return None


def unseen_as_null(values):
    """NaN (an eye the tracker can't see) as null: JSON has no NaN. fakes.load_gaze reads it back."""
    clean = lambda v: None if isinstance(v, float) and not math.isfinite(v) else v
    return {k: [clean(x) for x in v] if isinstance(v, list) else clean(v) for k, v in values.items()}


def private_file(path):
    return os.fdopen(os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC, 0o600), 'wb')


def record(directory, seconds, gaze=True, hands=True):
    """Record for this long into the new folder `directory`; returns what was recorded."""
    if not 0 < seconds <= 300:
        raise ValueError('record between 0 and 300 seconds')
    if hands and fakes.fresh(os.path.join(fakes.FAKE_DIR, 'hands'), 3):
        raise RuntimeError('the testbench is faking hands: release them before recording')
    if gaze and any(fakes.fresh(os.path.join(fakes.FAKE_DIR, name), 2)
                    for name in ('eye-server.mmap', 'frametop-eyes-gaze')):
        raise RuntimeError('the testbench is faking eyes: release them before recording')
    eyes = own = hands_m = gestures_m = None
    if gaze:
        try:
            eyes = EyeServer()
            eyes.detect()
        except (OSError, RuntimeError, ValueError):
            eyes = None
        try:
            own = OwnGaze()
        except (OSError, ValueError):
            own = None
        if not eyes and not own:
            raise RuntimeError('no eye tracker is writing: wear the headset with the gaze in use')
    if hands:
        try:
            hands_m = read_only(os.path.join(fakes.HANDS_DIR, 'hands'), fakes.HANDS_SIZE)
            gestures_m = read_only(os.path.join(fakes.HANDS_DIR, 'gestures'), fakes.GESTURES_SIZE)
        except (OSError, ValueError):
            raise RuntimeError("ft-hands isn't publishing hands (version 2 with gestures)") from None
    os.makedirs(directory, mode=0o700)
    counts = {}
    gaze_out = hands_out = None
    done = False
    try:
        if gaze:
            gaze_out = private_file(os.path.join(directory, 'gaze.jsonl'))
            gaze_out.write(json.dumps(fakes.GAZE_FORMAT).encode() + b'\n')
            counts.update(mmap=0, own=0)
        if hands:
            hands_out = private_file(os.path.join(directory, 'hands.fhdump'))
            hands_out.write(fakes.DUMP_MAGIC + struct.pack('<II', fakes.HANDS_SIZE, fakes.GESTURES_SIZE))
            counts.update(sets=0)
        start = time.monotonic()
        last = {'mmap': None, 'own': None, 'hands': None}
        while (elapsed := time.monotonic() - start) < seconds:
            for name, source in (('mmap', eyes), ('own', own)):
                got = source and source.sample()
                if got and got[0] != last[name]:
                    first = last[name] is None
                    last[name] = got[0]
                    if not first:   # the one there when we started is old
                        line = {'t': round(elapsed, 6), name: unseen_as_null(got[1])}
                        gaze_out.write(json.dumps(line, allow_nan=False).encode() + b'\n')
                        counts[name] += 1
            if hands_m:
                h = seqlock_copy(hands_m, fakes.HANDS_SIZE)
                g = seqlock_copy(gestures_m, fakes.GESTURES_SIZE)
                if h and g and h[0] != last['hands']:
                    first = last['hands'] is None
                    last['hands'] = h[0]
                    if not first:
                        hands_out.write(h[1] + g[1] + struct.pack('<q', fakes.mono_ns()))
                        counts['sets'] += 1
            time.sleep(0.003)
        done = True
    finally:
        for f in (gaze_out, hands_out):
            if f:
                f.close()
        for m in (eyes and eyes.m, own and own.m, hands_m, gestures_m):
            if m:
                m.close()
        if not done:   # a broken recording would only keep its name taken
            shutil.rmtree(directory, ignore_errors=True)
    return {'ok': True, 'directory': directory, 'seconds': seconds, 'recorded': counts,
            'files': sorted(os.listdir(directory))}
