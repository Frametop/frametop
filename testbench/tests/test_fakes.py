"""The fake eye and hand trackers (frame_testbench.fakes and handshape): their files read back
through the real layouts (hands/include's structs, ft-gaze's own reader), the gesture counters'
handover, and the daemon's commands and exits. No SteamVR, cameras or trackers needed."""
import json
import os
from pathlib import Path
import shlex
import signal
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

from frame_testbench import cli, fakes, handshape, pose, transport

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
BUILD = ROOT / 'build' / 'fake-tests'


def close(a, b, tol=1e-5):
    return all(abs(x - y) <= tol for x, y in zip(a, b)) and len(a) == len(b)


class HandShapeTests(unittest.TestCase):
    def test_shapes_read_as_the_gestures_they_stand_for(self):
        for side in fakes.SIDES:
            for shape in handshape.SHAPES:
                with self.subTest(side=side, shape=shape):
                    h = handshape.hand(side, [0, -0.2, -0.35], [1, 0, 0, 0], shape)
                    p = h['pts']
                    # ft-hands' default thresholds: pinch begins under 20 mm, ends over 45 mm;
                    # a grip begins with every finger's curl under 1.2, ends over 1.45.
                    if shape == 'pinch':
                        self.assertLess(h['pinch_d'][0], 0.020)
                    else:
                        self.assertGreater(h['pinch_d'][0], 0.045)
                    curls = [handshape.norm(handshape.sub(p[t], p[0])) / handshape.norm(handshape.sub(p[k], p[0]))
                             for k, t in ((5, 8), (9, 12), (13, 16), (17, 20))]
                    if shape == 'fist':
                        self.assertLess(max(curls), 1.2)
                    if shape in ('open', 'pinch'):
                        self.assertGreater(min(curls[1:]), 1.45)
                    self.assertTrue(all(abs(handshape.norm(q) - 1) < 1e-6 for q in h['quats']))
                    self.assertEqual((len(p), len(h['bones']), len(h['angles'])), (21, 20, 26))

    def test_joint_frames_follow_openxr(self):
        for side in fakes.SIDES:
            h = handshape.hand(side, [0, 0, -0.3], [1, 0, 0, 0], 'open')
            p = h['pts']
            for j in (5, 6, 9, 13, 17, 1, 2):
                x, y, z, w = h['quats'][j]
                q = [w, x, y, z]
                bone = handshape.unit(handshape.sub(p[j + 1], p[j]))
                with self.subTest(side=side, joint=j):
                    # -Z along the bone toward the tip, +Y out of the back of the (palm-down) hand.
                    self.assertGreater(handshape.dot(pose.rotate(q, [0, 0, -1]), bone), 0.999)
                    if j >= 5:
                        self.assertGreater(pose.rotate(q, [0, 1, 0])[1], 0.9)
                        # +X to the little finger on a right hand, to the thumb on a left one:
                        # with the palm down, both are +X in the head frame.
                        self.assertGreater(pose.rotate(q, [1, 0, 0])[0], 0.9)
            self.assertLess(p[4][0] * (1 if side == 'right' else -1), 0)   # thumb on the inside

    def test_left_hand_mirrors_the_right_and_bones_keep_their_length(self):
        right = handshape.hand('right', [0, 0, -0.3], [1, 0, 0, 0], 'point')
        left = handshape.hand('left', [0, 0, -0.3], [1, 0, 0, 0], 'point')
        for a, b in zip(right['pts'], left['pts']):
            self.assertTrue(close([-a[0], a[1], a[2]], b))
        self.assertTrue(close(right['bones'], left['bones']))
        self.assertTrue(close(right['bones'][5:8], handshape.FINGERS[0][1]))

    def test_rotation_and_position_place_the_hand_in_the_head_frame(self):
        q = pose.from_euler([0, 0, 0], [90, 0, 0])['quaternion']   # yaw 90: fingers to the left
        h = handshape.hand('right', [0.1, -0.2, -0.4], q, 'open')
        self.assertTrue(close(h['pts'][0], [0.1, -0.2, -0.4]))
        fingers = handshape.unit(handshape.sub(h['pts'][12], h['pts'][0]))
        self.assertLess(fingers[0], -0.95)

    def test_capsules_clip_at_the_near_plane_like_ft_hands(self):
        far = handshape.capsules(handshape.hand('right', [0, 0, -0.4], [1, 0, 0, 0], 'open')['pts'])
        self.assertEqual(len(far), 4 + 12 + 8 + 1)
        a, b, ra, rb = far[0]
        self.assertAlmostEqual(ra, handshape.THUMB_R + handshape.MARGIN)
        # Wrist at 0.15 m, fingers toward the eyes: everything nearer than 0.12 m is cut off.
        q = pose.from_euler([0, 0, 0], [180, 0, 0])['quaternion']
        near = handshape.capsules(handshape.hand('right', [0, 0, -0.15], q, 'open')['pts'])
        self.assertLess(len(near), len(far))
        self.assertTrue(all(-p[2] >= handshape.NEAR - 1e-6 for c in near for p in c[:2]))


class Scratch(unittest.TestCase):
    def setUp(self):
        scratch = os.environ.get('TMPDIR', str(ROOT / 'build'))
        Path(scratch).mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix='ff-', dir=scratch)
        self.dir = Path(self.temp.name)
        self.fake_dir = self.dir / 'frametop-testbench'
        self.hands_dir = self.dir / 'frametop-hands'
        settle = mock.patch.object(fakes, 'HANDS_SETTLE', 0)
        settle.start()
        self.addCleanup(settle.stop)
        self.fakes = fakes.Fakes(str(self.fake_dir), str(self.hands_dir))
        self.addCleanup(self.fakes.close)
        self.addCleanup(self.temp.cleanup)

    def ok(self, command):
        reply = self.fakes.command(command)
        self.assertTrue(reply['ok'], reply)
        return reply

    def tick(self, n=1):
        for _ in range(n):
            self.fakes.next_eyes = self.fakes.next_hands = 0
            self.fakes.tick(time.monotonic())


class EyeFileTests(Scratch):
    def eye(self):
        data = (self.fake_dir / 'eye-server.mmap').read_bytes()
        get = lambda off, fmt: struct.unpack_from('<' + fmt, data, off)
        return data, get

    def own(self):
        data = (self.fake_dir / 'frametop-eyes-gaze').read_bytes()
        seq, version, t, yaw, pitch, flags, n = struct.unpack_from('<IIdffII', data, 0)
        return dict(seq=seq, version=version, t=t, yaw=yaw, pitch=pitch, flags=flags, n=n,
                    eyes=struct.unpack_from('<4f', data, 32))

    def test_both_files_in_their_layouts(self):
        self.ok('gaze 10 6 -4 2')
        self.tick(2)
        data, get = self.eye()
        self.assertEqual(len(data), fakes.EYE_FILE_SIZE)
        self.assertEqual(get(0x38, 'I'), (2,))
        t = get(0x157 + 5, 'd')[0]
        self.assertAlmostEqual(t, fakes.raw_now() - 0.017, delta=0.05)
        # Set 1 shares one pitch, set 2 is each eye's own.
        self.assertTrue(close(get(0x15f + 5, '3f'), fakes.direction(10, 4)))
        self.assertTrue(close(get(0x16b + 5, '3f'), fakes.direction(-4, 4)))
        self.assertTrue(close(get(0x19b + 5, '3f'), fakes.direction(10, 6)))
        self.assertTrue(close(get(0x1a7 + 5, '3f'), fakes.direction(-4, 2)))
        self.assertTrue(close(get(0x177 + 5, '6f'), [0.001, 0.0005, 0.001] * 2))
        self.assertTrue(close(get(0x1cb + 5, '2f'), [0.9, 0.9]))
        self.assertAlmostEqual(handshape.norm(get(0x18f + 5, '3f')), 1, places=5)
        own = self.own()
        self.assertEqual((own['seq'] % 2, own['version'], own['flags']), (0, 1, 3))
        self.assertAlmostEqual(own['t'], t + 0.017, places=6)
        self.assertAlmostEqual(own['yaw'], 3, places=5)
        self.assertAlmostEqual(own['pitch'], 4, places=5)
        self.assertTrue(close(own['eyes'], [-4, 2, 10, 6]))   # right yaw, pitch, then left

    def test_a_lost_eye_holds_still_and_a_closed_one_is_still_seen(self):
        self.ok('gaze 5 0 5 0')
        self.tick()
        self.ok('eyes lost closed')
        self.ok('gaze 20 10 20 10')
        self.tick()
        _, get = self.eye()
        meas = get(0x1d3 + 5, '8f')
        self.tick()
        _, get = self.eye()
        self.assertTrue(close(get(0x19b + 5, '3f'), fakes.direction(5, 0)))     # left: where it was
        self.assertTrue(close(get(0x1a7 + 5, '3f'), fakes.direction(20, 10)))   # right: closed, followed
        self.assertTrue(close(get(0x177 + 5, '6f'), [0.02, 0.01, 0.02, 0.001, 0.0005, 0.001]))
        self.assertTrue(close(get(0x1cb + 5, '2f'), [0.0, 0.0]))
        later = get(0x1d3 + 5, '8f')
        self.assertEqual(later[:2], meas[:2])       # the lost eye's measurement froze
        self.assertNotEqual(later[2], meas[2])      # the seen one's keeps moving
        own = self.own()
        self.assertEqual(own['flags'], 1)           # bit 0 right seen, bit 1 left
        self.assertAlmostEqual(own['yaw'], 20, places=5)

    def test_release_removes_the_files(self):
        self.ok('gaze 0 0 0 0')
        self.tick()
        self.ok('gaze-release')
        self.assertEqual(list(self.fake_dir.iterdir()), [])
        self.assertFalse(self.fakes.active())


def compile_readers():
    """The C reader against hands/include, and ft-gaze's own reader around gaze/ft-gaze.cpp
    (None when there's no OpenVR library to link it with)."""
    BUILD.mkdir(parents=True, exist_ok=True)
    cc = os.environ.get('CC', 'gcc')
    hands = BUILD / 'fake_hands_reader'
    subprocess.run([cc, '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', '-I', str(REPO / 'hands/include'),
                    str(ROOT / 'tests/fake_hands_reader.c'), '-o', str(hands)], check=True)
    flags = subprocess.run(['bash', '-c', '. "$1" && printf "%s\\n%s\\n" "$OPENVR_CFLAGS" "$OPENVR_LIBS"', '_',
                            str(REPO / 'scripts/openvr.sh')], cwd=ROOT, check=True, capture_output=True, text=True)
    cflags, libs = flags.stdout.splitlines()
    libdir = next((a[2:] for a in shlex.split(libs) if a.startswith('-L')), '')
    if not (Path(libdir) / 'libopenvr_api.so').exists():
        return hands, None
    eyes = BUILD / 'fake_eye_reader'
    subprocess.run([os.environ.get('CXX', 'g++'), '-std=c++17', '-O1', '-Wall', '-Wno-unused-parameter',
                    '-Wno-missing-field-initializers', *shlex.split(cflags), '-I', str(REPO / 'pointer/common'),
                    str(ROOT / 'tests/fake_eye_reader.cpp'), '-o', str(eyes), *shlex.split(libs), '-lpthread'],
                   cwd=ROOT, check=True)
    return hands, eyes


class ReaderTests(Scratch):
    @classmethod
    def setUpClass(cls):
        cls.hands_reader, cls.eye_reader = compile_readers()

    def hands(self):
        out = subprocess.run([str(self.hands_reader), str(self.hands_dir)], check=True, capture_output=True,
                             text=True, timeout=10)
        return json.loads(out.stdout)

    def test_hands_and_gestures_through_the_real_structs(self):
        self.ok('hand right 0.1 -0.2 -0.35 1 0 0 0 pinch')
        self.ok('hand left -0.1 -0.2 -0.35 1 0 0 0 open')
        self.tick(3)
        read = self.hands()
        h, g = read['hands'], read['gestures']
        self.assertEqual((h['magic_ok'], h['version'], h['size'], h['seq'] % 2, h['nhands']), (1, 2, 4704, 0, 2))
        built = {s: handshape.hand(s, *self.fakes.hands.poses[s]) for s in fakes.SIDES}
        byid = {hand['id']: hand for hand in h['list']}
        for side, flags, fit_flags in (('left', 2, 1 | 4 | 16), ('right', 3, 1 | 2 | 4 | 16)):
            with self.subTest(side=side):
                hand, b = byid[fakes.HAND_IDS[side]], built[side]
                self.assertEqual((hand['flags'], hand['fit']['flags'], hand['fit']['id']),
                                 (flags, fit_flags, fakes.HAND_IDS[side]))
                self.assertTrue(close(hand['pts'], [c for p in b['pts'] for c in p]))
                self.assertTrue(close(hand['fit']['joint8_p'], b['pts'][8]))
                self.assertTrue(close(hand['fit']['joint8_q'], b['quats'][8]))
                self.assertAlmostEqual(hand['fit']['joint8_radius'], handshape.RADIUS[8], places=6)
                self.assertTrue(close(hand['fit']['bone_len'], b['bones']))
                self.assertTrue(close(hand['fit']['pinch_point'], b['pinch_point']))
                self.assertTrue(close(hand['fit']['pinch_d'], b['pinch_d']))
                self.assertEqual(hand['ncapsules'], len(handshape.capsules(b['pts'])))
        self.assertEqual(h['ncapsules'], sum(hand['ncapsules'] for hand in h['list']))
        self.assertEqual(h['list'][0]['id'], fakes.HAND_IDS['left'])   # hands in side order
        first = handshape.capsules(built['left']['pts'])[0]
        self.assertTrue(close(h['capsule0'], [*first[0], *first[1], first[2], first[3]]))
        self.assertEqual((g['magic_ok'], g['version'], g['size'], g['seq'] % 2), (1, 3, 448, 0))
        self.assertTrue(close([g['begin_m'], g['end_m'], g['grip_begin'], g['grip_end']], [0.020, 0.045, 1.2, 1.45]))
        down = fakes.FH_PINCH_TRACKED | fakes.FH_PINCH_DOWN
        self.assertEqual([g['pinch_right'][k] for k in ('flags', 'hand_id', 'begins', 'ends')], [down, 9002, 1, 0])
        self.assertEqual([g['pinch_left'][k] for k in ('flags', 'begins', 'ends')], [fakes.FH_PINCH_TRACKED, 0, 0])
        self.assertEqual(g['pinch_right']['strength'], 1)
        self.assertTrue(close(g['pinch_right']['point'], built['right']['pinch_point']))
        self.assertEqual([g['grip_right'][k] for k in ('begins', 'ends')], [0, 0])

    def test_a_shape_change_is_one_click_and_a_fist_one_grip(self):
        self.ok('hand right 0 -0.2 -0.35 1 0 0 0 pinch')
        self.tick(3)
        self.ok('hand right 0 -0.2 -0.35 1 0 0 0 open')
        self.tick()
        g = self.hands()['gestures']
        self.assertEqual([g['pinch_right'][k] for k in ('flags', 'begins', 'ends')], [fakes.FH_PINCH_TRACKED, 1, 1])
        self.ok('hand right 0 -0.2 -0.35 1 0 0 0 fist')
        self.tick(2)
        g = self.hands()['gestures']
        self.assertEqual([g['grip_right'][k] for k in ('flags', 'begins', 'ends')],
                         [fakes.FH_PINCH_TRACKED | fakes.FH_PINCH_DOWN, 1, 0])
        self.assertEqual(g['pinch_right']['begins'], 1)

    def test_counters_continue_from_ft_hands_and_a_held_one_ends_as_lost(self):
        # What a parked ft-hands might leave: its own thresholds, a right pinch it never ended.
        self.hands_dir.mkdir(mode=0o700)
        old = bytearray(fakes.GESTURES_SIZE)
        struct.pack_into('<8sIIQQQ4f', old, 0, b'FHGEST01', 3, fakes.GESTURES_SIZE, 80, 1, 2, 0.018, 0.05, 1.1, 1.5)
        struct.pack_into('<4I', old, 64 + 64, fakes.FH_PINCH_TRACKED | fakes.FH_PINCH_DOWN, 4, 7, 6)
        struct.pack_into('<4I', old, 64 + 2 * 64, 0, 0, 3, 3)
        (self.hands_dir / 'gestures').write_bytes(old)
        self.ok('hand right 0 -0.2 -0.35 1 0 0 0 open')
        self.tick()
        g = self.hands()['gestures']
        self.assertGreater(g['seq'], 80)
        self.assertTrue(close([g['begin_m'], g['end_m'], g['grip_begin'], g['grip_end']], [0.018, 0.05, 1.1, 1.5]))
        self.assertEqual([g['pinch_right'][k] for k in ('flags', 'begins', 'ends')],
                         [fakes.FH_PINCH_TRACKED | fakes.FH_PINCH_LOST, 7, 7])
        self.assertEqual([g['grip_left'][k] for k in ('begins', 'ends')], [3, 3])
        self.ok('hand right 0 -0.2 -0.35 1 0 0 0 pinch')
        self.tick()
        g = self.hands()['gestures']
        self.assertEqual([g['pinch_right'][k] for k in ('flags', 'begins', 'ends')],
                         [fakes.FH_PINCH_TRACKED | fakes.FH_PINCH_DOWN, 8, 7])

    def test_release_ends_gestures_as_lost_and_lets_ft_hands_go(self):
        self.ok('hand right 0 -0.2 -0.35 1 0 0 0 pinch')
        self.tick(2)
        self.assertTrue((self.fake_dir / 'hands').exists())
        self.ok('hand-release all')
        read = self.hands()
        self.assertEqual((read['hands']['nhands'], read['hands']['ncapsules']), (0, 0))
        g = read['gestures']
        self.assertEqual([g['pinch_right'][k] for k in ('flags', 'hand_id', 'begins', 'ends')],
                         [fakes.FH_PINCH_LOST, 0, 1, 1])
        self.assertFalse((self.fake_dir / 'hands').exists())
        self.assertFalse(self.fakes.active())

    def test_nothing_is_published_until_ft_hands_parked(self):
        with mock.patch.object(fakes, 'HANDS_SETTLE', 30):
            self.ok('hand left 0 0 -0.3 1 0 0 0 open')
            self.tick(2)
        self.assertTrue((self.fake_dir / 'hands').exists())
        self.assertFalse((self.hands_dir / 'hands').exists())
        self.assertFalse(self.fakes.status()['hands_publishing'])

    def test_ft_gaze_reads_the_fake_eye_files(self):
        if self.eye_reader is None:
            self.skipTest("no OpenVR library to link ft-gaze's reader with")
        self.ok('gaze 12 -3 8 -3')
        self.ok('eyes open lost')
        self.tick()
        env = dict(os.environ, XDG_RUNTIME_DIR=str(self.dir))
        reader = subprocess.Popen([str(self.eye_reader)], env=env, stdout=subprocess.PIPE, text=True)
        end = time.monotonic() + 10
        while reader.poll() is None and time.monotonic() < end:
            self.tick()
            time.sleep(1 / 90)
        out, _ = reader.communicate(timeout=5)
        read = json.loads(out)
        self.assertEqual((read['fresh'], read['opened'], read['found'], read['shift'], read['read']),
                         (1, 1, 1, 5, 1), read)
        self.assertLess(abs(read['age'] - 0.017), 0.1)
        self.assertTrue(close(read['left1'], fakes.direction(12, -3)))
        self.assertTrue(close(read['left2'], fakes.direction(12, -3)))
        self.assertTrue(close(read['var1'], [0.001, 0.0005, 0.001, 0.02, 0.01, 0.02]))
        self.assertTrue(close(read['open'], [0.9, 0.0]))
        own = read['own']
        self.assertEqual((own['read'], own['flags']), (1, 2))   # only the left eye seen
        self.assertLess(abs(own['age']), 0.1)
        self.assertTrue(close([own['yaw'], own['pitch']], [12, -3]))
        self.assertTrue(close(own['eyes'], [8, -3, 12, -3]))


class DaemonTests(unittest.TestCase):
    """The process the CLI starts: commands over its socket, and every way it ends."""

    def setUp(self):
        scratch = os.environ.get('TMPDIR', str(ROOT / 'build'))
        Path(scratch).mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix='fd-', dir=scratch)
        self.addCleanup(self.temp.cleanup)
        self.dir = Path(self.temp.name)
        self.socket = str(self.dir / 'fakes.sock')
        self.env = dict(os.environ, XDG_RUNTIME_DIR=str(self.dir), FRAME_TESTBENCH_FAKES_SOCKET=self.socket,
                        FRAME_TESTBENCH_HANDS_DIR=str(self.dir / 'frametop-hands'),
                        FRAME_TESTBENCH_HANDS_SETTLE='0', PYTHONPATH=str(ROOT))
        patch = mock.patch.object(fakes, 'SOCKET', self.socket)
        patch.start()
        self.addCleanup(patch.stop)
        self.process = None
        self.launches = []
        self.addCleanup(self.stop)

    def stop(self):
        if self.process and self.process.poll() is None:
            self.process.kill()
            self.process.wait(5)

    def launch(self, argv, **kwargs):
        """systemd-run, standing in: the same command, as a child with the test's environment."""
        self.launches.append(argv)
        self.assertEqual(argv[:4], ['systemd-run', '--user', '--quiet', '--collect'])
        self.assertIn('--unit=frame-testbench-fakes', argv)
        self.assertEqual(argv[-3:], [sys.executable, '-m', 'frame_testbench.fakes'])
        self.process = subprocess.Popen(argv[-3:], env=self.env, cwd=ROOT)
        return subprocess.CompletedProcess(argv, 0)

    def start(self, command):
        with mock.patch.object(cli.subprocess, 'run', side_effect=self.launch):
            return cli.Bench().fakes(command, start=True)

    def gone(self):
        self.assertEqual(self.process.wait(5), 0)
        self.assertFalse(os.path.exists(self.socket))
        self.assertEqual(list((self.dir / 'frametop-testbench').iterdir()), [])

    def test_started_on_demand_refuses_bad_commands_and_ends_on_release(self):
        self.assertIsNone(cli.Bench().fakes('status'))
        reply = self.start('gaze 1 2 3 4')
        self.assertEqual(reply['gaze']['right'], {'yaw': 3.0, 'pitch': 4.0, 'state': 'open'})
        self.assertEqual(len(self.launches), 1)
        self.assertEqual(self.start('status')['pid'], self.process.pid)
        self.assertEqual(len(self.launches), 1)
        for bad in ('gaze 1 2 3', 'gaze 1 2 3 nan', 'gaze 1 2 3 inf', 'gaze 100 0 0 0', 'eyes open blink',
                    'eyes open', 'hand middle 0 0 -0.3 1 0 0 0 open', 'hand right 0 0 -3 1 0 0 0 open',
                    'hand right 0 0 -0.3 0 0 0 0 open', 'hand right 0 0 -0.3 1 0 0 0 claw',
                    'hand right 0 0 -0.3 1 0 0 open', 'hand-release both', 'release now', 'status 1', 'bogus',
                    'gaze ' + '1' * 2100):
            with self.subTest(command=bad[:40]):
                with self.assertRaises((RuntimeError, ValueError)):
                    transport.request(self.socket, bad)
        time.sleep(0.1)
        files = sorted(p.name for p in (self.dir / 'frametop-testbench').iterdir())
        self.assertEqual(files, ['eye-server.mmap', 'frametop-eyes-gaze'])
        self.assertLess(time.time() - (self.dir / 'frametop-testbench/eye-server.mmap').stat().st_mtime, 1)
        reply = transport.request(self.socket, 'release')
        self.assertEqual((reply['gaze'], reply['hands']), (None, None))
        self.gone()
        self.assertIsNone(cli.Bench().fakes('status'))

    def test_sigterm_publishes_no_hands_and_cleans_up(self):
        self.start('hand right 0 -0.2 -0.35 1 0 0 0 pinch')
        time.sleep(0.3)
        self.assertTrue(transport.request(self.socket, 'status')['hands_publishing'])
        self.process.send_signal(signal.SIGTERM)
        self.gone()
        hands = (self.dir / 'frametop-hands/hands').read_bytes()
        self.assertEqual(struct.unpack_from('<II', hands, 40), (0, 0))   # nhands, ncapsules
        gestures = (self.dir / 'frametop-hands/gestures').read_bytes()
        self.assertEqual(struct.unpack_from('<4I', gestures, 64 + 64), (fakes.FH_PINCH_LOST, 0, 1, 1))

    def test_left_alone_it_lets_go_by_itself(self):
        self.env['FRAME_TESTBENCH_FAKES_IDLE'] = '0.5'
        self.start('gaze 0 0 0 0')
        self.gone()

    def test_releasing_the_last_fake_ends_it(self):
        self.start('hand left 0 -0.2 -0.35 1 0 0 0 open')
        transport.request(self.socket, 'hand right 0 -0.2 -0.35 1 0 0 0 open')
        transport.request(self.socket, 'hand-release left')
        self.assertIsNone(self.process.poll())
        transport.request(self.socket, 'hand-release right')
        self.gone()


if __name__ == '__main__':
    unittest.main()
