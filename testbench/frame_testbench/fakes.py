"""Fake eye and hand trackers at Frametop's boundary.

One small process (the CLI starts it as the transient user unit frame-testbench-fakes) stands in
for the trackers whose output Frametop reads:

- Eyes: it writes stand-ins for SteamVR's /dev/shm/eye-server.mmap (SteamOS 0.4's layout) and
  our tracker's /dev/shm/frametop-eyes-gaze to $XDG_RUNTIME_DIR/frametop-testbench/, 90 times a
  second. ft-gaze reads a stand-in instead of the real file while it was written in the last
  2 s (gaze/ft-gaze.cpp), so everything from ft-gaze on runs as with real eyes. It never
  touches the real files.
- Hands: it touches $XDG_RUNTIME_DIR/frametop-testbench/hands twice a second, which parks ft-hands
  (hands/track/main.cpp), and publishes the hands and gestures files ft-hands would, 30 times a
  second, in their layouts (hands/include/fh_hands.h, fh_gestures.h).

Commands come one line at a time on a user-only socket, like the input proxy's, and every reply
is one JSON line. Faked state stays until released. Releasing everything ends the process, and
so does SIGTERM, an hour without a command, or a hand or gaze file it can't write: each takes its
files back out, so the real trackers take over again within a few seconds.

  status
  gaze LEFT_YAW LEFT_PITCH RIGHT_YAW RIGHT_PITCH   head-relative degrees, yaw +left, pitch +up
  eyes LEFT RIGHT                                   each open, closed or lost
  gaze-release
  hand left|right X Y Z QW QX QY QZ SHAPE           wrist in the head frame (m), SHAPE from handshape.SHAPES
  hand-release left|right|all
  replay-gaze PATH once|loop                        a recording (frame_testbench.record) in place of the
  replay-hands PATH once|loop                       scripted fake, by its absolute path; once: released at its end
  release
"""
import json
import math
import os
import select
import signal
import socket
import stat
import struct
import sys
import time

from . import handshape, pose

RUN = os.environ.get('XDG_RUNTIME_DIR') or f'/run/user/{os.getuid()}'
FAKE_DIR = os.path.join(RUN, 'frametop-testbench')   # where ft-gaze and ft-hands look
HANDS_DIR = os.environ.get('FRAME_TESTBENCH_HANDS_DIR', f'/run/user/{os.getuid()}/frametop-hands')
SOCKET = os.environ.get('FRAME_TESTBENCH_FAKES_SOCKET', os.path.join(RUN, 'frame-testbench-fakes.sock'))
IDLE_EXIT = float(os.environ.get('FRAME_TESTBENCH_FAKES_IDLE', 3600))
# ft-hands checks its park files every 0.25 s and, parking, publishes no hands once: start after that.
HANDS_SETTLE = float(os.environ.get('FRAME_TESTBENCH_HANDS_SETTLE', 0.6))
EYE_STATES = ('open', 'closed', 'lost')


def raw_now():
    return time.clock_gettime(time.CLOCK_MONOTONIC_RAW)


def mono_ns():
    return time.clock_gettime_ns(time.CLOCK_MONOTONIC)


def direction(yaw, pitch):
    """md::Direction (pointer/common/vrmath.h): head-relative degrees to a unit vector."""
    y, p = math.radians(yaw), math.radians(pitch)
    return [-math.sin(y) * math.cos(p), math.sin(p), -math.cos(y) * math.cos(p)]


def fresh(path, limit):
    """Whether the file was written in the last `limit` seconds."""
    try:
        return abs(time.time() - os.stat(path).st_mtime) < limit
    except OSError:
        return False


def open_private(path, size):
    fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
    if os.fstat(fd).st_size < size:
        os.ftruncate(fd, size)
    return fd


# --- eyes ------------------------------------------------------------------------------------

# eye-server.mmap, SteamOS 0.4: ft-gaze's offsets (gaze/ft-gaze.cpp) plus 5 from the timestamp on.
SHIFT = 5
COUNTER, TIME = 0x38, 0x157 + SHIFT
LEFT1, RIGHT1, VAR1, FIX1 = 0x15f + SHIFT, 0x16b + SHIFT, 0x177 + SHIFT, 0x18f + SHIFT
LEFT2, RIGHT2, VAR2 = 0x19b + SHIFT, 0x1a7 + SHIFT, 0x1b3 + SHIFT
OPEN, MEAS = 0x1cb + SHIFT, 0x1d3 + SHIFT
EYE_FILE_SIZE = 0x200
SHIFTS = (SHIFT, 0)   # the layouts ft-gaze knows: SteamOS 0.4, 0.3
SEEN_VAR, LOST_VAR = 0.001, 0.02   # SteamVR's filter variance with an eye seen, and lost
OWN_SIZE = 128
# What one eye-server.mmap sample holds (offsets for SteamOS 0.3; 0.4 adds SHIFT from the time on).
MMAP_FIELDS = (('left1', 0x15f, 3), ('right1', 0x16b, 3), ('fix1', 0x18f, 3), ('left2', 0x19b, 3),
               ('right2', 0x1a7, 3), ('var1', 0x177, 6), ('var2', 0x1b3, 6), ('open', 0x1cb, 2),
               ('meas', 0x1d3, 8))
OWN_FIELDS = (('eyes', 32, 4), ('slip', 48, 4))
# Samples for a tracker a gaze recording doesn't have: both eyes lost.
LOST_SAMPLE = {'left1': direction(0, 0), 'right1': direction(0, 0), 'fix1': direction(0, 0),
               'left2': direction(0, 0), 'right2': direction(0, 0), 'var1': [LOST_VAR] * 6,
               'var2': [LOST_VAR] * 6, 'open': [0.0, 0.0], 'meas': [0.0] * 8}
NO_OWN_SAMPLE = {'yaw': 0.0, 'pitch': 0.0, 'flags': 0, 'eyes': [0.0] * 4, 'slip': [0.0] * 4}


class Playback:
    """A recording's clock: which of each stream's samples (times in seconds from the start)
    is due, once each, looping or not."""

    def __init__(self, streams, loop):
        self.streams, self.loop = streams, loop
        last = [times[-1] for times in streams if times]
        # A loop's length: the last sample plus a typical gap, so it doesn't repeat at once.
        self.length = max(last) + 1 / 30
        self.restart(time.monotonic())
        self.laps = 0

    def restart(self, now):
        """From the top at now."""
        self.start = now
        self.next = [0] * len(self.streams)

    def advance(self, now):
        """([the newest due sample's index or None, per stream], done, wrapped)."""
        wrapped = False
        if now - self.start >= self.length:
            if not self.loop:
                return [None] * len(self.streams), True, False
            self.start += self.length * ((now - self.start) // self.length)
            self.next = [0] * len(self.streams)
            self.laps += 1
            wrapped = True
        elapsed = now - self.start
        due = []
        for k, times in enumerate(self.streams):
            i = self.next[k]
            while i < len(times) and times[i] <= elapsed:
                i += 1
            due.append(i - 1 if i > self.next[k] else None)
            self.next[k] = i
        return due, False, wrapped

    def status(self, path):
        return {'path': path, 'loop': self.loop, 'laps': self.laps, 'length': round(self.length, 3),
                'position': round(min(time.monotonic() - self.start, self.length), 3)}


class Eyes:
    """SteamVR's tracker and ours, both seeing the same two eyes."""

    def __init__(self, fake_dir=FAKE_DIR):
        os.makedirs(fake_dir, mode=0o700, exist_ok=True)
        self.eye_path = os.path.join(fake_dir, 'eye-server.mmap')
        self.own_path = os.path.join(fake_dir, 'frametop-eyes-gaze')
        self.eye_fd = open_private(self.eye_path, EYE_FILE_SIZE)
        self.own_fd = open_private(self.own_path, OWN_SIZE)
        self.look = [0.0, 0.0, 0.0, 0.0]   # left yaw, pitch, right yaw, pitch
        self.state = ['open', 'open']
        self.held = [direction(0, 0), direction(0, 0)]   # a lost eye's last direction
        self.meas = [0.0] * 8
        self.n = self.own_n = 0
        self.replay = None   # (Playback, mmap samples, own samples, path) while playing a recording
        self.written = 0.0

    def status(self):
        if self.replay:
            return {'replay': self.replay[0].status(self.replay[3])}
        return {'left': {'yaw': self.look[0], 'pitch': self.look[1], 'state': self.state[0]},
                'right': {'yaw': self.look[2], 'pitch': self.look[3], 'state': self.state[1]}}

    def play(self, path, loop):
        mmap_samples, own_samples = load_gaze(path)
        playback = Playback([[t for t, _ in mmap_samples], [t for t, _ in own_samples]], loop)
        self.replay = (playback, mmap_samples, own_samples, path)

    def write(self):
        """The next sample; False once a recording played to its end."""
        if not self.replay:
            self.publish(*self.scripted())
            return True
        playback, mmap_samples, own_samples, _ = self.replay
        (m, o), done, _ = playback.advance(time.monotonic())
        if done:
            return False
        # A tracker the recording doesn't have says it sees no eyes, so ft-gaze doesn't read the
        # real one beside the recorded other.
        if m is not None or o is not None:
            self.publish(mmap_samples[m][1] if m is not None else None if mmap_samples else LOST_SAMPLE,
                         own_samples[o][1] if o is not None else None if own_samples else NO_OWN_SAMPLE)
        elif time.monotonic() - self.written > 0.5:   # a pause in the recording: stay fresh
            for path in (self.eye_path, self.own_path):
                os.utime(path)
            self.written = time.monotonic()
        return True

    def scripted(self):
        """The two trackers' samples for the scripted look and eye states."""
        dirs = []
        for k in range(2):
            if self.state[k] != 'lost':
                self.held[k] = direction(self.look[2 * k], self.look[2 * k + 1])
            dirs.append(self.held[k])
        seen = [s != 'lost' for s in self.state]
        # Set 1 is filtered and its eyes share one pitch; set 2 is each eye's own reading.
        pitch = (self.look[1] + self.look[3]) / 2
        set1 = [direction(self.look[0], pitch) if seen[0] else dirs[0],
                direction(self.look[2], pitch) if seen[1] else dirs[1]]
        var = []
        for k in range(2):
            v = SEEN_VAR if seen[k] else LOST_VAR
            var += [v, v / 2, v]
        mean = pose.normalize([0.0] + [a + b for a, b in zip(set1[0], set1[1])])[1:]
        # The raw measurements move with each new frame of a seen eye and freeze while it's lost.
        for k in range(2):
            if seen[k]:
                self.meas[2 * k] = dirs[k][0] + (self.n % 2) * 1e-5
                self.meas[2 * k + 1] = dirs[k][1]
                self.meas[4 + 2 * k] = self.meas[5 + 2 * k] = 2e-5
        openness = [0.9 if s == 'open' else 0.0 for s in self.state]
        sample = {'left1': set1[0], 'right1': set1[1], 'fix1': mean,   # the fixation point: 1 m out
                  'left2': dirs[0], 'right2': dirs[1], 'var1': var, 'var2': var, 'open': openness,
                  'meas': list(self.meas)}
        n = max(1, sum(seen))
        own = {'yaw': sum(self.look[2 * k] for k in range(2) if seen[k]) / n,
               'pitch': sum(self.look[2 * k + 1] for k in range(2) if seen[k]) / n,
               'flags': (1 if seen[1] else 0) | (2 if seen[0] else 0),
               'eyes': [self.look[2], self.look[3], self.look[0], self.look[1]], 'slip': [0.0] * 4}
        return sample, own

    def publish(self, sample, own):
        """One sample each to the files that have one (None: that file stays as it is)."""
        t = raw_now() - 0.017   # a real sample is about this old when it appears
        self.written = time.monotonic()
        if sample is not None:
            self.n = (self.n + 1) & 0xffffffff
            body = bytearray(EYE_FILE_SIZE - TIME)
            struct.pack_into('<d', body, 0, t)
            for name, off, count in MMAP_FIELDS:
                struct.pack_into(f'<{count}f', body, off + SHIFT - TIME, *sample[name])
            # The eye server has no lock: ft-gaze reads until the counter and time hold still
            # across a copy. Writing the counter last makes a torn copy show as a counter that moved.
            os.pwrite(self.eye_fd, bytes(body[8:]), TIME + 8)
            os.pwrite(self.eye_fd, bytes(body[:8]), TIME)
            os.pwrite(self.eye_fd, struct.pack('<I', self.n), COUNTER)
        if own is not None:
            # Ours (ft-eyes): u32 seq (odd while written), u32 version, f64 t, f32 yaw, pitch, u32
            # flags (bit 0 right eye seen, 1 left), u32 n, f32 right yaw, pitch, left yaw, pitch, slip.
            self.own_n = (self.own_n + 1) & 0x7fffffff
            body = struct.pack('<Id2fII4f4f', 1, t + 0.017, own['yaw'], own['pitch'], own['flags'], self.own_n,
                               *own['eyes'], *own['slip'])
            os.pwrite(self.own_fd, struct.pack('<I', 2 * self.own_n - 1), 0)
            os.pwrite(self.own_fd, body, 4)
            os.pwrite(self.own_fd, struct.pack('<I', 2 * self.own_n), 0)

    def close(self):
        for fd, path in ((self.eye_fd, self.eye_path), (self.own_fd, self.own_path)):
            os.close(fd)
            try:
                os.unlink(path)
            except FileNotFoundError:
                pass


# --- hands -----------------------------------------------------------------------------------

HANDS_HEADER, HAND, CAPSULE, FIT = 64, 272, 32, 1024
HANDS_V1 = HANDS_HEADER + 2 * HAND + 64 * CAPSULE
HANDS_SIZE = HANDS_V1 + 2 * FIT
GESTURES_SIZE = 64 + 6 * 64
FH_HAND_RIGHT, FH_HAND_STEREO = 1, 2
FH_FIT_VALID, FH_FIT_RIGHT, FH_FIT_SETTLED, FH_FIT_ELBOW_HEAD_UP = 1, 2, 4, 16
FH_PINCH_TRACKED, FH_PINCH_DOWN, FH_PINCH_LOST = 1, 2, 4
HAND_IDS = {'left': 9001, 'right': 9002}
SIDES = ('left', 'right')


class Gesture:
    """One fh_pinch_t slot: begins and ends count every gesture, so readers miss none."""

    def __init__(self, raw, keep=False):
        (self.flags, self.hand_id, self.begins, self.ends, self.begin_ns, self.end_ns, self.distance,
         self.strength, *rest) = struct.unpack('<4I2Q2f6f', raw)
        self.point, self.begin_point = list(rest[:3]), list(rest[3:])
        if not keep:
            self.hand_id = 0
            if self.begins != self.ends:   # one a previous writer left down is over, as lost
                self.end(mono_ns(), lost=True)

    def update(self, hand_id, down, now, distance, strength, point):
        self.hand_id = hand_id
        self.flags = (self.flags & FH_PINCH_LOST) | (FH_PINCH_TRACKED if hand_id else 0)
        if hand_id:
            self.distance, self.strength, self.point = distance, strength, point
        if down and self.begins == self.ends:
            self.begins += 1
            self.begin_ns, self.begin_point = now, point
            self.flags &= ~FH_PINCH_LOST
        elif not down and self.begins != self.ends:
            self.end(now, lost=not hand_id)
        if self.begins != self.ends:
            self.flags |= FH_PINCH_DOWN

    def end(self, now, lost):
        self.ends = self.begins
        self.end_ns = now
        self.flags = (self.flags & ~FH_PINCH_DOWN) | (FH_PINCH_LOST if lost else 0)

    def pack(self):
        return struct.pack('<4I2Q2f6f', self.flags, self.hand_id, self.begins, self.ends, self.begin_ns,
                           self.end_ns, self.distance, self.strength, *self.point, *self.begin_point)


class Hands:
    """Both hands, published as ft-hands would while it's parked."""

    def __init__(self, fake_dir=FAKE_DIR, hands_dir=HANDS_DIR):
        os.makedirs(fake_dir, mode=0o700, exist_ok=True)
        os.makedirs(hands_dir, mode=0o700, exist_ok=True)
        self.hands_dir = hands_dir
        self.marker = os.path.join(fake_dir, 'hands')
        self.touched = 0.0
        self.touch()
        self.started = time.monotonic()
        self.poses = {}   # side -> (position, quaternion, shape)
        self.hands_fd = self.gestures_fd = None
        self.hands_seq = self.gestures_seq = 0
        self.thresholds = (0.020, 0.045, 1.2, 1.45)   # ft-hands' defaults (hands/track/pinch.h)
        self.slots = None
        self.replay = None   # (Playback, HandsDump, path) while playing a recording
        self.rebase = None   # per gesture slot: what to add to the recording's counters

    def touch(self):
        """Keep ft-hands parked: the marker is fresh for 3 s (hands/track/main.cpp)."""
        if time.monotonic() - self.touched < 0.5:
            return
        with open(self.marker, 'a'):
            pass
        os.utime(self.marker)
        self.touched = time.monotonic()

    def ready(self):
        return time.monotonic() - self.started >= HANDS_SETTLE

    def open_files(self):
        """After ft-hands parked: take its files over, keeping the gesture counters it left."""
        self.hands_fd = open_private(os.path.join(self.hands_dir, 'hands'), HANDS_SIZE)
        self.gestures_fd = open_private(os.path.join(self.hands_dir, 'gestures'), GESTURES_SIZE)
        old = os.pread(self.gestures_fd, GESTURES_SIZE, 0)
        ours = old[:8] == b'FHGEST01' and struct.unpack_from('<I', old, 8)[0] == 3
        self.gestures_seq = struct.unpack_from('<Q', old, 16)[0] // 2 + 1 if ours else 1
        if ours:
            self.thresholds = struct.unpack_from('<4f', old, 40)
        slot = lambda k: old[64 + 64 * k:128 + 64 * k] if ours else bytes(64)
        # pinch[left, right], grip[left, right], pinch2[left, right]
        self.slots = [[Gesture(slot(2 * kind + s)) for s in range(2)] for kind in range(3)]
        self.hands_seq = struct.unpack_from('<Q', os.pread(self.hands_fd, 8, 16), 0)[0] // 2 + 1

    def status(self):
        if self.replay:
            return {'replay': self.replay[0].status(self.replay[2])}
        return {side: {'position': p, 'quaternion': q, 'shape': s} for side, (p, q, s) in self.poses.items()}

    def play(self, path, loop):
        dump = HandsDump(path)
        self.stop_replay()
        self.poses.clear()
        self.replay = (Playback([dump.times], loop), dump, path)
        self.rebase = None

    def stop_replay(self):
        if self.replay:
            self.replay[1].close()
        self.replay = None

    def write(self):
        """The next set; False once a recording played to its end."""
        self.touch()
        if not self.ready():
            return True
        if self.hands_fd is None:
            self.open_files()
        now = mono_ns()
        if not self.replay:
            built = {side: handshape.hand(side, *self.poses[side]) for side in SIDES if side in self.poses}
            self.write_hands(built, now)
            self.write_gestures(built, now)
            return True
        playback, dump, _ = self.replay
        (i,), done, wrapped = playback.advance(time.monotonic())
        if wrapped or done:
            # Between laps, and at the end: whatever was down ends, as lost, like a lost hand.
            self.write_gestures({}, now)
            self.rebase = None
        if done:
            return False
        if i is not None:
            self.publish_recorded(*dump.read(i)[:2], now)
        return True

    def publish_recorded(self, hands, gestures, now):
        """A recorded set, as now: its times moved to now and its gesture counters carrying on
        from ours, so readers see new gestures and never a counter going back."""
        h, g = bytearray(hands), bytearray(gestures)
        delta = now - struct.unpack_from('<Q', h, 32)[0]   # its publish_ns becomes now
        shift = lambda buf, off: struct.pack_into('<Q', buf, off, max(0, struct.unpack_from('<Q', buf, off)[0] + delta))
        shift(h, 24)
        shift(h, 32)
        for k in range(2):
            if struct.unpack_from('<Q', h, HANDS_V1 + k * FIT + 8)[0]:
                shift(h, HANDS_V1 + k * FIT + 8)
        self.hands_seq += 1
        os.pwrite(self.hands_fd, bytes(h[:16]) + struct.pack('<Q', 2 * self.hands_seq - 1), 0)
        os.pwrite(self.hands_fd, bytes(h[24:]), 24)
        os.pwrite(self.hands_fd, struct.pack('<Q', 2 * self.hands_seq), 16)
        shift(g, 24)
        shift(g, 32)
        slots = [slot for kind in self.slots for slot in kind]
        if self.rebase is None:
            self.rebase = [mine.ends - struct.unpack_from('<I', g, 64 + 64 * k + 12)[0]
                           for k, mine in enumerate(slots)]
        for k, mine in enumerate(slots):
            off = 64 + 64 * k
            for field in (16, 24):
                if struct.unpack_from('<Q', g, off + field)[0]:
                    shift(g, off + field)
            begins, ends = (v + self.rebase[k] for v in struct.unpack_from('<2I', g, off + 8))
            struct.pack_into('<2I', g, off + 8, begins & 0xffffffff, ends & 0xffffffff)
            # Ours from now on, so a release or the next lap ends what's down.
            mine.__init__(bytes(g[off:off + 64]), keep=True)
            mine.hand_id = struct.unpack_from('<I', g, off + 4)[0]
        self.thresholds = struct.unpack_from('<4f', g, 40)
        self.gestures_seq += 1
        os.pwrite(self.gestures_fd, bytes(g[:16]) + struct.pack('<Q', 2 * self.gestures_seq - 1), 0)
        os.pwrite(self.gestures_fd, bytes(g[24:]), 24)
        os.pwrite(self.gestures_fd, struct.pack('<Q', 2 * self.gestures_seq), 16)

    def write_hands(self, built, now):
        hands, caps, fits = bytearray(), [], bytearray()
        for side, h in built.items():
            right = side == 'right'
            mine = handshape.capsules(h['pts'])
            caps += mine
            hands += struct.pack('<2I2f63fI', HAND_IDS[side], (FH_HAND_RIGHT if right else 0) | FH_HAND_STEREO,
                                 1.0, 0.0, *[c for p in h['pts'] for c in p], len(mine))
            joints = [c for p, r, q in zip(h['pts'], h['radius'], h['quats']) for c in (*p, r, *q)]
            fits += struct.pack('<2IQ168f20f26f3ff3f3f2fff3f3f18f', HAND_IDS[side],
                                FH_FIT_VALID | FH_FIT_SETTLED | FH_FIT_ELBOW_HEAD_UP | (FH_FIT_RIGHT if right else 0),
                                now, *joints, *h['bones'], *h['angles'], *h['elbow'], 0.0, *h['pinch_point'],
                                *h['contact'], *h['pinch_d'], 0.003, 1.0, 0, 0, 0, 0, 0, 0, *[0.0] * 18)
        caps = caps[:64]
        body = struct.pack('<QQII16x', now, mono_ns(), len(built), len(caps))
        body += hands.ljust(2 * HAND, b'\0')
        body += b''.join(struct.pack('<8f', *a, *b, ra, rb) for a, b, ra, rb in caps).ljust(64 * CAPSULE, b'\0')
        body += fits.ljust(2 * FIT, b'\0')
        self.hands_seq += 1
        fd = self.hands_fd
        os.pwrite(fd, b'FHHANDS1' + struct.pack('<IIQ', 2, HANDS_SIZE, 2 * self.hands_seq - 1), 0)
        os.pwrite(fd, body, 24)
        os.pwrite(fd, struct.pack('<Q', 2 * self.hands_seq), 16)

    def write_gestures(self, built, now):
        begin_m, end_m, grip_begin, grip_end = self.thresholds
        for s, side in enumerate(SIDES):
            h = built.get(side)
            hid = HAND_IDS[side] if h else 0
            shape = h['shape'] if h else None
            pinch, grip, middle = (self.slots[kind][s] for kind in range(3))
            if h:
                d = h['pinch_d']
                pinch_strength = min(1.0, max(0.0, (end_m - d[0]) / (end_m - begin_m)))
                grip_strength = min(1.0, max(0.0, (2.0 - h['curl']) / (2.0 - grip_begin)))
                grip.update(hid, shape == 'fist', now, h['curl'], grip_strength, h['palm'])
                pinch.update(hid, shape == 'pinch', now, d[0], pinch_strength, h['pinch_point'])
                middle.update(hid, False, now, d[1], 0.0, h['pinch_point'])
            else:
                for g in (pinch, grip, middle):
                    g.update(0, False, now, 0.0, 0.0, [0.0] * 3)
        body = struct.pack('<QQ4f8x', now, mono_ns(), *self.thresholds)
        body += b''.join(g.pack() for kind in self.slots for g in kind)
        self.gestures_seq += 1
        fd = self.gestures_fd
        os.pwrite(fd, b'FHGEST01' + struct.pack('<IIQ', 3, GESTURES_SIZE, 2 * self.gestures_seq - 1), 0)
        os.pwrite(fd, body, 24)
        os.pwrite(fd, struct.pack('<Q', 2 * self.gestures_seq), 16)

    def close(self):
        """No hands and every gesture over, as lost; then ft-hands takes the files back."""
        self.stop_replay()
        if self.hands_fd is not None:
            self.poses.clear()
            self.write_hands({}, mono_ns())
            self.write_gestures({}, mono_ns())
            os.close(self.hands_fd)
            os.close(self.gestures_fd)
        try:
            os.unlink(self.marker)
        except FileNotFoundError:
            pass


# --- recordings (frame_testbench.record makes them) ------------------------------------------

DUMP_MAGIC = b'FHDUMP02'
GAZE_FORMAT = {'format': 'frame-testbench-gaze', 'version': 1}
MAX_GAZE_BYTES = 64 << 20


def open_recording(path, limit):
    """A recording the user owns, by an absolute path, without following a link."""
    if not os.path.isabs(path):
        raise ValueError('a recording is given by its absolute path')
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
    st = os.fstat(fd)
    if not stat.S_ISREG(st.st_mode) or st.st_uid != os.getuid() or st.st_size > limit:
        os.close(fd)
        raise ValueError(f'{path}: not a recording of yours (a regular file under {limit >> 20} MiB)')
    return os.fdopen(fd, 'rb')


def finite_list(value, count):
    if (not isinstance(value, list) or len(value) != count
            or not all(isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v) for v in value)):
        raise ValueError(f'expected {count} finite numbers')
    return [float(v) for v in value]


def load_gaze(path):
    """gaze.jsonl's samples: ([(t, mmap fields)], [(t, own fields)])."""
    with open_recording(path, MAX_GAZE_BYTES) as f:
        lines = f.read().splitlines()
    try:
        if not lines or json.loads(lines[0]) != GAZE_FORMAT:
            raise ValueError
        mmap_frames, own_frames = [], []
        for raw in lines[1:]:
            line = json.loads(raw)
            t = finite_list([line['t']], 1)[0]
            if 'mmap' in line:
                m = line['mmap']
                mmap_frames.append((t, {name: finite_list(m[name], count) for name, _, count in MMAP_FIELDS}))
            elif 'own' in line:
                o = line['own']
                own = {name: finite_list(o[name], count) for name, _, count in OWN_FIELDS}
                own['yaw'], own['pitch'] = finite_list([o['yaw'], o['pitch']], 2)
                if not isinstance(o['flags'], int) or not 0 <= o['flags'] < 1 << 32:
                    raise ValueError
                own['flags'] = o['flags']
                own_frames.append((t, own))
            else:
                raise ValueError
    except (ValueError, KeyError, TypeError):
        raise ValueError(f'{path}: not a gaze recording (frame-testbench-gaze version 1)') from None
    if not mmap_frames and not own_frames:
        raise ValueError(f'{path}: the recording has no samples')
    return mmap_frames, own_frames


class HandsDump:
    """hands.fhdump (ft-handreplay --publish-dump's format), read a set at a time."""

    def __init__(self, path):
        self.f = open_recording(path, 1 << 34)
        header = self.f.read(16)
        if len(header) != 16 or header[:8] != DUMP_MAGIC or struct.unpack_from('<II', header, 8) != (
                HANDS_SIZE, GESTURES_SIZE):
            self.f.close()
            raise ValueError(f'{path}: not a hands recording (FHDUMP02 of hands v2 and gestures v3)')
        self.record = HANDS_SIZE + GESTURES_SIZE + 8
        size = os.fstat(self.f.fileno()).st_size - 16
        self.count = size // self.record
        if not self.count or size % self.record:
            self.f.close()
            raise ValueError(f'{path}: the recording has no sets, or a cut-off one')
        times = []
        for i in range(self.count):
            self.f.seek(16 + i * self.record + self.record - 8)
            times.append(struct.unpack('<q', self.f.read(8))[0])
        if any(b < a for a, b in zip(times, times[1:])):
            self.f.close()
            raise ValueError(f'{path}: its sets go back in time')
        self.times = [(t - times[0]) / 1e9 for t in times]

    def read(self, i):
        self.f.seek(16 + i * self.record)
        data = self.f.read(self.record)
        hands, gestures = data[:HANDS_SIZE], data[HANDS_SIZE:-8]
        if hands[:8] != b'FHHANDS1' or gestures[:8] != b'FHGEST01':
            raise ValueError(f'set {i} of the hands recording is damaged')
        return hands, gestures, struct.unpack('<q', data[-8:])[0]

    def close(self):
        self.f.close()


# --- the server ------------------------------------------------------------------------------

def finite(values):
    out = [float(v) for v in values]
    if not all(math.isfinite(v) for v in out):
        raise ValueError('expected finite numbers')
    return out


class Fakes:
    def __init__(self, fake_dir=FAKE_DIR, hands_dir=HANDS_DIR):
        self.fake_dir, self.hands_dir = fake_dir, hands_dir
        self.eyes = None
        self.hands = None
        self.next_eyes = self.next_hands = 0.0
        self.together = False   # a gaze replay started with a hands replay waits with it

    def status(self, ok=True, error=None):
        reply = {'ok': ok, 'pid': os.getpid(),
                 'gaze': self.eyes.status() if self.eyes else None,
                 'hands': self.hands.status() if self.hands else None,
                 'hands_publishing': bool(self.hands and self.hands.hands_fd is not None)}
        if error:
            reply['error'] = error
        return reply

    def command(self, line):
        words = line.split()
        if not words:
            return self.status(False, 'empty command')
        op, args = words[0], words[1:]
        try:
            if op == 'status' and not args:
                pass
            elif op == 'gaze' and len(args) == 4:
                look = finite(args)
                if not all(-90 <= v <= 90 for v in look):
                    raise ValueError('gaze angles must be within 90 degrees of straight ahead')
                self.eyes = self.eyes or Eyes(self.fake_dir)
                self.eyes.look = look
                self.eyes.replay = None
            elif op == 'eyes' and len(args) == 2:
                if not all(a in EYE_STATES for a in args):
                    raise ValueError('each eye is open, closed or lost')
                if not self.eyes or self.eyes.replay:
                    raise ValueError('set a gaze first')
                self.eyes.state = list(args)
            elif op == 'gaze-release' and not args:
                if self.eyes:
                    self.eyes.close()
                self.eyes = None
            elif op == 'hand' and len(args) == 9 and args[0] in SIDES:
                position, quaternion = finite(args[1:4]), pose.normalize(finite(args[4:8]))
                if not all(abs(v) <= 2 for v in position):
                    raise ValueError('a hand must be within 2 m of the head')
                if args[8] not in handshape.SHAPES:
                    raise ValueError(f'shape must be one of {", ".join(handshape.SHAPES)}')
                self.hands = self.hands or Hands(self.fake_dir, self.hands_dir)
                self.hands.stop_replay()
                self.hands.poses[args[0]] = (position, quaternion, args[8])
            elif op == 'hand-release' and len(args) == 1 and args[0] in SIDES + ('all',):
                if self.hands:
                    self.hands.stop_replay()
                    for side in SIDES if args[0] == 'all' else args:
                        self.hands.poses.pop(side, None)
                    if not self.hands.poses:
                        self.hands.close()
                        self.hands = None
            elif op in ('replay-gaze', 'replay-hands') and len(args) == 2 and args[1] in ('once', 'loop'):
                if op == 'replay-gaze':
                    eyes = self.eyes or Eyes(self.fake_dir)
                    try:
                        eyes.play(args[0], args[1] == 'loop')
                    finally:
                        if not self.eyes and not eyes.replay:
                            eyes.close()
                    self.eyes = eyes
                    self.next_eyes = 0.0
                    self.together = bool(self.hands and self.hands.replay and not self.hands.ready())
                else:
                    hands = self.hands or Hands(self.fake_dir, self.hands_dir)
                    try:
                        hands.play(args[0], args[1] == 'loop')
                    finally:
                        if not self.hands and not hands.replay:
                            hands.close()
                    self.hands = hands
                    self.next_hands = 0.0
                    # `replay NAME` sends the gaze first: it starts over with the hands.
                    self.together = bool(self.eyes and self.eyes.replay
                                         and time.monotonic() - self.eyes.replay[0].start < 1)
            elif op == 'release' and not args:
                self.close()
            else:
                return self.status(False, f'unknown or malformed command: {op}')
        except ValueError as error:
            return self.status(False, str(error))
        except OSError as error:
            return self.status(False, f'{error.filename or "recording"}: {error.strerror}')
        return self.status()

    def tick(self, now):
        # A hands recording starts once ft-hands has parked (HANDS_SETTLE), not before, so its
        # first sets aren't skipped; a gaze recording started with it holds its first sample.
        if self.hands and self.hands.replay and not self.hands.ready():
            self.hands.replay[0].restart(now)
            if self.together and self.eyes and self.eyes.replay:
                self.eyes.replay[0].restart(now)
        else:
            self.together = False
        # A recording plays at its own times, so it's looked at more often than a scripted fake
        # is written; nothing is written between its samples.
        if self.eyes and now >= self.next_eyes:
            if not self.eyes.write():
                self.eyes.close()
                self.eyes = None
            self.next_eyes = max(self.next_eyes + (1 / 250 if self.eyes and self.eyes.replay else 1 / 90), now)
        if self.hands and now >= self.next_hands:
            if not self.hands.write():
                self.hands.close()
                self.hands = None
            self.next_hands = max(self.next_hands + (1 / 250 if self.hands and self.hands.replay else 1 / 30), now)

    def next_due(self):
        due = [t for t, on in ((self.next_eyes, self.eyes), (self.next_hands, self.hands)) if on]
        return min(due) if due else None

    def active(self):
        return bool(self.eyes or self.hands)

    def close(self):
        eyes, hands, self.eyes, self.hands = self.eyes, self.hands, None, None
        for fake in (hands, eyes):
            if fake:
                try:
                    fake.close()
                except OSError as error:
                    print(f'frame-testbench fakes: cleanup failed: {error}', file=sys.stderr)


def serve():
    fakes = Fakes()
    stop = []
    signal.signal(signal.SIGTERM, lambda *_: stop.append(1))
    signal.signal(signal.SIGINT, lambda *_: stop.append(1))
    try:
        os.unlink(SOCKET)
    except FileNotFoundError:
        pass
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    old = os.umask(0o177)
    try:
        server.bind(SOCKET)
    finally:
        os.umask(old)
    server.listen(4)
    last_command = time.monotonic()
    started = False
    try:
        while not stop:
            now = time.monotonic()
            # Done once nothing is faked any more (after the first command), or left alone too long.
            if (started and not fakes.active()) or now - last_command > IDLE_EXIT:
                break
            due = fakes.next_due()
            timeout = 0.5 if due is None else max(0.0, min(0.5, due - now))
            try:
                ready, _, _ = select.select([server], [], [], timeout)
            except InterruptedError:
                continue
            if ready:
                client, _ = server.accept()
                with client:
                    client.settimeout(2)
                    try:
                        data = b''
                        while b'\n' not in data and len(data) <= 2048:
                            part = client.recv(4096)
                            if not part:
                                break
                            data += part
                        line = data.split(b'\n', 1)[0].decode('ascii')
                        reply = fakes.command(line) if len(data) <= 2048 else fakes.status(False, 'oversized command')
                    except (OSError, UnicodeDecodeError):
                        continue
                    started = True
                    last_command = time.monotonic()
                    try:
                        client.sendall(json.dumps(reply).encode() + b'\n')
                    except OSError:
                        pass
            fakes.tick(time.monotonic())
    finally:
        fakes.close()
        server.close()
        try:
            os.unlink(SOCKET)
        except FileNotFoundError:
            pass


if __name__ == '__main__':
    sys.exit(serve())
