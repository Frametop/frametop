"""Fake eye and hand trackers at Frametop's boundary.

One small process (the CLI starts it as the transient user unit frame-testbench-fakes) stands in
for the trackers whose output Frametop reads:

- Eyes: it writes stand-ins for SteamVR's /dev/shm/eye-server.mmap (SteamOS 0.4's layout) and
  our tracker's /dev/shm/frametop-eyes-gaze to $XDG_RUNTIME_DIR/frametop-testbench/, 90 times a
  second. ft-gaze reads a stand-in instead of the real file while it was written in the last
  2 s (gaze/ft-gaze.cpp), so everything from ft-gaze on runs as with real eyes. It never
  touches the real files.
- Hands: it touches $XDG_RUNTIME_DIR/frametop-testbench/hands every second, which parks ft-hands
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
  release
"""
import json
import math
import os
import select
import signal
import socket
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
SEEN_VAR, LOST_VAR = 0.001, 0.02   # SteamVR's filter variance with an eye seen, and lost
OWN_SIZE = 128


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
        self.n = 0

    def status(self):
        return {'left': {'yaw': self.look[0], 'pitch': self.look[1], 'state': self.state[0]},
                'right': {'yaw': self.look[2], 'pitch': self.look[3], 'state': self.state[1]}}

    def write(self):
        self.n = (self.n + 1) & 0xffffffff
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
        body = bytearray(EYE_FILE_SIZE - TIME)

        def put(off, fmt, *values):
            struct.pack_into(fmt, body, off - TIME, *values)

        t = raw_now() - 0.017   # a real sample is about this old when it appears
        put(TIME, '<d', t)
        put(LEFT1, '<3f', *set1[0])
        put(RIGHT1, '<3f', *set1[1])
        put(VAR1, '<6f', *var)
        put(FIX1, '<3f', *mean)   # the fixation point: 1 m out
        put(LEFT2, '<3f', *dirs[0])
        put(RIGHT2, '<3f', *dirs[1])
        put(VAR2, '<6f', *var)
        put(OPEN, '<2f', *openness)
        put(MEAS, '<8f', *self.meas)
        # The eye server has no lock: ft-gaze reads until the counter and time hold still across
        # a copy. Writing the counter last makes a torn copy show as a counter that moved.
        os.pwrite(self.eye_fd, bytes(body[8:]), TIME + 8)
        os.pwrite(self.eye_fd, bytes(body[:8]), TIME)
        os.pwrite(self.eye_fd, struct.pack('<I', self.n), COUNTER)
        # Ours (ft-eyes): u32 seq (odd while written), u32 version, f64 t, f32 yaw, pitch, u32
        # flags (bit 0 right eye seen, 1 left), u32 n, f32 right yaw, pitch, left yaw, pitch, slip.
        flags = (1 if seen[1] else 0) | (2 if seen[0] else 0)
        yaw = sum(self.look[2 * k] for k in range(2) if seen[k]) / max(1, sum(seen))
        pit = sum(self.look[2 * k + 1] for k in range(2) if seen[k]) / max(1, sum(seen))
        own = struct.pack('<Id2fII4f4f', 1, t + 0.017, yaw, pit, flags, self.n,
                          self.look[2], self.look[3], self.look[0], self.look[1], 0, 0, 0, 0)
        seq = 2 * self.n
        os.pwrite(self.own_fd, struct.pack('<I', seq + 1), 0)
        os.pwrite(self.own_fd, own, 4)
        os.pwrite(self.own_fd, struct.pack('<I', seq + 2), 0)

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

    def __init__(self, raw):
        (self.flags, _, self.begins, self.ends, self.begin_ns, self.end_ns, self.distance, self.strength,
         *rest) = struct.unpack('<4I2Q2f6f', raw)
        self.point, self.begin_point = list(rest[:3]), list(rest[3:])
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
        self.touch()
        self.started = time.monotonic()
        self.poses = {}   # side -> (position, quaternion, shape)
        self.hands_fd = self.gestures_fd = None
        self.hands_seq = self.gestures_seq = 0
        self.thresholds = (0.020, 0.045, 1.2, 1.45)   # ft-hands' defaults (hands/track/pinch.h)
        self.slots = None

    def touch(self):
        with open(self.marker, 'a'):
            pass
        os.utime(self.marker)

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
        return {side: {'position': p, 'quaternion': q, 'shape': s} for side, (p, q, s) in self.poses.items()}

    def write(self):
        self.touch()
        if not self.ready():
            return
        if self.hands_fd is None:
            self.open_files()
        now = mono_ns()
        built = {side: handshape.hand(side, *self.poses[side]) for side in SIDES if side in self.poses}
        self.write_hands(built, now)
        self.write_gestures(built, now)

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
            elif op == 'eyes' and len(args) == 2:
                if not all(a in EYE_STATES for a in args):
                    raise ValueError('each eye is open, closed or lost')
                if not self.eyes:
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
                self.hands.poses[args[0]] = (position, quaternion, args[8])
            elif op == 'hand-release' and len(args) == 1 and args[0] in SIDES + ('all',):
                if self.hands:
                    for side in SIDES if args[0] == 'all' else args:
                        self.hands.poses.pop(side, None)
                    if not self.hands.poses:
                        self.hands.close()
                        self.hands = None
            elif op == 'release' and not args:
                self.close()
            else:
                return self.status(False, f'unknown or malformed command: {op}')
        except ValueError as error:
            return self.status(False, str(error))
        return self.status()

    def tick(self, now):
        if self.eyes and now >= self.next_eyes:
            self.eyes.write()
            self.next_eyes = max(self.next_eyes + 1 / 90, now)
        if self.hands and now >= self.next_hands:
            self.hands.write()
            self.next_hands = max(self.next_hands + 1 / 30, now)

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
