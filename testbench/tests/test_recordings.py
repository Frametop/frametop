"""Recording the trackers' output (frame_testbench.record) and playing it back through the fake
trackers: a scripted fake stands in for the real trackers being recorded. No SteamVR or cameras."""
import json
import math
import os
from pathlib import Path
import struct
import tempfile
import threading
import time
import unittest
from unittest import mock

from frame_testbench import cli, fakes, record

ROOT = Path(__file__).resolve().parents[1]


def close(a, b, tol=1e-5):
    return len(a) == len(b) and all(abs(x - y) <= tol for x, y in zip(a, b))


class Base(unittest.TestCase):
    def setUp(self):
        scratch = os.environ.get('TMPDIR', str(ROOT / 'build'))
        Path(scratch).mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix='fr-', dir=scratch)
        self.addCleanup(self.temp.cleanup)
        self.dir = Path(self.temp.name)
        for name, value in (('HANDS_SETTLE', 0), ('FAKE_DIR', str(self.dir / 'fake')),
                            ('HANDS_DIR', str(self.dir / 'hands'))):
            patch = mock.patch.object(fakes, name, value)
            patch.start()
            self.addCleanup(patch.stop)

    def player(self):
        player = fakes.Fakes(str(self.dir / 'fake'), str(self.dir / 'hands'))
        self.addCleanup(player.close)
        return player

    def run_for(self, player, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end and player.active():
            player.tick(time.monotonic())
            time.sleep(0.002)

    def gaze_recording(self, samples, own=()):
        path = self.dir / 'gaze.jsonl'
        lines = [json.dumps(fakes.GAZE_FORMAT)]
        lines += [json.dumps({'t': t, 'mmap': m}) for t, m in samples]
        lines += [json.dumps({'t': t, 'own': o}) for t, o in own]
        path.write_text('\n'.join(lines) + '\n')
        return str(path)

    def eye_file(self):
        data = (self.dir / 'fake/eye-server.mmap').read_bytes()
        counter = struct.unpack_from('<I', data, fakes.COUNTER)[0]
        return counter, {name: list(struct.unpack_from(f'<{n}f', data, off + fakes.SHIFT))
                         for name, off, n in fakes.MMAP_FIELDS}

    def gestures(self):
        data = (self.dir / 'hands/gestures').read_bytes()
        return [struct.unpack_from('<4I', data, 64 + 64 * k) for k in range(6)]


def sample(yaw, var=0.001):
    d = fakes.direction(yaw, 0)
    return {'left1': d, 'right1': d, 'fix1': d, 'left2': d, 'right2': d, 'var1': [var] * 6,
            'var2': [var] * 6, 'open': [0.8, 0.7], 'meas': [yaw / 100] * 4 + [2e-5] * 4}


class GazeReplayTests(Base):
    def test_samples_play_at_their_times_then_it_lets_go(self):
        path = self.gaze_recording([(0.0, sample(1)), (0.15, sample(2)), (0.3, sample(3))])
        player = self.player()
        reply = player.command(f'replay-gaze {path} once')
        self.assertTrue(reply['ok'], reply)
        self.assertEqual(reply['gaze']['replay']['path'], path)
        seen = []
        end = time.monotonic() + 2
        while player.active() and time.monotonic() < end:
            player.tick(time.monotonic())
            if (self.dir / 'fake/eye-server.mmap').exists():
                counter, fields = self.eye_file()
                if counter and (not seen or seen[-1][0] != counter):
                    seen.append((counter, fields, time.monotonic()))
            time.sleep(0.002)
        self.assertEqual([c for c, _, _ in seen], [1, 2, 3])   # each sample once
        for (_, fields, _), yaw in zip(seen, (1, 2, 3)):
            self.assertTrue(all(close(fields[k], v) for k, v in sample(yaw).items()))
        self.assertAlmostEqual(seen[2][2] - seen[0][2], 0.3, delta=0.1)
        self.assertFalse(player.active())
        self.assertFalse((self.dir / 'fake/eye-server.mmap').exists())

    def test_a_tracker_missing_from_the_recording_sees_no_eyes(self):
        own = {'yaw': 4.0, 'pitch': -2.0, 'flags': 3, 'eyes': [1.0, 2.0, 3.0, 4.0], 'slip': [0.0] * 4}
        path = self.gaze_recording([], own=[(0.0, own)])
        player = self.player()
        self.assertTrue(player.command(f'replay-gaze {path} loop')['ok'])
        self.run_for(player, 0.05)
        _, fields = self.eye_file()
        self.assertTrue(close(fields['var1'], [fakes.LOST_VAR] * 6))
        data = (self.dir / 'fake/frametop-eyes-gaze').read_bytes()
        self.assertEqual(struct.unpack_from('<ffI', data, 16), (4.0, -2.0, 3))

    def test_a_loop_repeats_and_a_scripted_look_takes_over(self):
        path = self.gaze_recording([(0.0, sample(1)), (0.05, sample(2))])
        player = self.player()
        player.command(f'replay-gaze {path} loop')
        self.run_for(player, 0.4)
        self.assertGreaterEqual(player.status()['gaze']['replay']['laps'], 3)
        self.assertGreater(self.eye_file()[0], 6)
        reply = player.command('gaze 5 0 5 0')
        self.assertEqual(reply['gaze']['left']['yaw'], 5.0)
        self.run_for(player, 0.05)
        self.assertTrue(close(self.eye_file()[1]['left2'], fakes.direction(5, 0)))

    def test_bad_recordings_are_refused_and_change_nothing(self):
        good = sample(1)
        bad = dict(good, left1=[0.0, 'x', 1.0])
        cases = {'missing': str(self.dir / 'nope.jsonl'), 'relative': 'gaze.jsonl',
                 'empty': self.gaze_recording([]), }
        cases['bad sample'] = self.gaze_recording([(0.0, bad)])
        (self.dir / 'other.jsonl').write_text('{"format": "something else", "version": 1}\n')
        cases['other format'] = str(self.dir / 'other.jsonl')
        os.symlink(self.dir / 'gaze.jsonl', self.dir / 'link.jsonl')
        cases['symlink'] = str(self.dir / 'link.jsonl')
        player = self.player()
        for name, path in cases.items():
            with self.subTest(name):
                reply = player.command(f'replay-gaze {path} once')
                self.assertFalse(reply['ok'], reply)
                self.assertIsNone(reply['gaze'])
        self.assertFalse((self.dir / 'fake/eye-server.mmap').exists())
        self.assertFalse(player.command(f'replay-gaze {cases["empty"]} twice')['ok'])
        os.mkfifo(self.dir / 'fifo')   # refused at once, not waited on
        self.assertFalse(player.command(f'replay-gaze {self.dir / "fifo"} once')['ok'])

    def test_an_unseen_eye_records_as_null_and_plays_back_as_nan(self):
        own = {'yaw': float('nan'), 'pitch': 2.0, 'flags': 2, 'eyes': [float('nan'), float('nan'), 3.0, 2.0],
               'slip': [0.0] * 4}
        line = record.unseen_as_null(own)
        self.assertEqual((line['yaw'], line['eyes'][:2], line['flags']), (None, [None, None], 2))
        path = self.dir / 'gaze.jsonl'
        path.write_text(json.dumps(fakes.GAZE_FORMAT) + '\n' + json.dumps({'t': 0, 'own': line}, allow_nan=False) + '\n')
        player = self.player()
        self.assertTrue(player.command(f'replay-gaze {path} loop')['ok'])
        self.run_for(player, 0.05)
        data = (self.dir / 'fake/frametop-eyes-gaze').read_bytes()
        yaw, pitch = struct.unpack_from('<ff', data, 16)
        self.assertTrue(math.isnan(yaw))
        self.assertEqual(pitch, 2.0)


def hands_set(t_ns, pinch_right, seq=2):
    """One recorded set: a right hand whose pinch slot has these counters."""
    hands = bytearray(fakes.HANDS_SIZE)
    struct.pack_into('<8sIIQQQII', hands, 0, b'FHHANDS1', 2, fakes.HANDS_SIZE, seq, t_ns - 20_000_000, t_ns, 1, 0)
    struct.pack_into('<2I', hands, 64, 7, fakes.FH_HAND_RIGHT | fakes.FH_HAND_STEREO)
    struct.pack_into('<IIQ', hands, fakes.HANDS_V1, 7, fakes.FH_FIT_VALID | fakes.FH_FIT_RIGHT, t_ns - 20_000_000)
    gestures = bytearray(fakes.GESTURES_SIZE)
    struct.pack_into('<8sIIQQQ4f', gestures, 0, b'FHGEST01', 3, fakes.GESTURES_SIZE, seq, t_ns - 20_000_000, t_ns,
                     0.02, 0.045, 1.2, 1.45)
    begins, ends = pinch_right
    flags = fakes.FH_PINCH_TRACKED | (fakes.FH_PINCH_DOWN if begins != ends else 0)
    struct.pack_into('<4IQ', gestures, 64 + 64, flags, 7, begins, ends, t_ns - 50_000_000)
    return bytes(hands) + bytes(gestures) + struct.pack('<q', t_ns)


class HandsReplayTests(Base):
    def dump(self, sets):
        path = self.dir / 'hands.fhdump'
        path.write_bytes(fakes.DUMP_MAGIC + struct.pack('<II', fakes.HANDS_SIZE, fakes.GESTURES_SIZE) + b''.join(sets))
        return str(path)

    def test_sets_play_with_times_moved_to_now_and_counters_carrying_on(self):
        # What ft-hands left: 10 pinches on the right, all ended.
        (self.dir / 'hands').mkdir(mode=0o700)
        old = bytearray(fakes.GESTURES_SIZE)
        struct.pack_into('<8sIIQ', old, 0, b'FHGEST01', 3, fakes.GESTURES_SIZE, 40)
        struct.pack_into('<4I', old, 64 + 64, fakes.FH_PINCH_TRACKED, 7, 10, 10)
        (self.dir / 'hands/gestures').write_bytes(old)
        base = 5_000_000_000_000
        # The recording starts mid-pinch (its 3rd), ends it, starts another.
        path = self.dump([hands_set(base, (3, 2)), hands_set(base + 100_000_000, (3, 3)),
                          hands_set(base + 200_000_000, (4, 3))])
        player = self.player()
        self.assertTrue(player.command(f'replay-hands {path} once')['ok'])
        pinches = []
        end = time.monotonic() + 2
        while player.active() and time.monotonic() < end:
            player.tick(time.monotonic())
            if (self.dir / 'hands/hands').exists():
                g = self.gestures()[1]
                if not pinches or pinches[-1] != g:
                    pinches.append(g)
                    if len(pinches) == 1:
                        hands = (self.dir / 'hands/hands').read_bytes()
                        capture, publish = struct.unpack_from('<QQ', hands, 24)
                        self.assertLess(abs(publish - fakes.mono_ns()), 100_000_000)
                        self.assertEqual(publish - capture, 20_000_000)
                        self.assertEqual(struct.unpack_from('<I', hands, 64)[0], 7)
            time.sleep(0.002)
        down, tracked, lost = fakes.FH_PINCH_DOWN, fakes.FH_PINCH_TRACKED, fakes.FH_PINCH_LOST
        self.assertEqual([p[2:] for p in pinches], [(11, 10), (11, 11), (12, 11), (12, 12)])
        self.assertEqual(pinches[0][0], tracked | down)
        self.assertEqual(pinches[-1][0], lost)   # at the end: ended, as lost
        self.assertFalse(player.active())
        self.assertFalse((self.dir / 'fake/hands').exists())

    def test_a_loop_ends_whats_down_between_laps_and_counts_on(self):
        base = 7_000_000_000_000
        path = self.dump([hands_set(base, (1, 0)), hands_set(base + 40_000_000, (1, 0))])
        player = self.player()
        player.command(f'replay-hands {path} loop')
        self.run_for(player, 0.5)
        begins, ends = self.gestures()[1][2:]
        self.assertGreaterEqual(player.status()['hands']['replay']['laps'], 3)
        self.assertGreaterEqual(ends, 3)
        self.assertIn(begins - ends, (0, 1))
        player.command('hand-release all')
        self.assertFalse(player.active())
        self.assertEqual(self.gestures()[1][2] - self.gestures()[1][3], 0)

    def test_bad_dumps_are_refused(self):
        base = 1_000_000_000
        good = hands_set(base, (0, 0))
        cases = {
            'wrong magic': b'FHDUMP01' + struct.pack('<II', fakes.HANDS_SIZE, fakes.GESTURES_SIZE) + good,
            'wrong sizes': fakes.DUMP_MAGIC + struct.pack('<II', 4000, fakes.GESTURES_SIZE) + good,
            'cut off': fakes.DUMP_MAGIC + struct.pack('<II', fakes.HANDS_SIZE, fakes.GESTURES_SIZE) + good[:-1],
            'no sets': fakes.DUMP_MAGIC + struct.pack('<II', fakes.HANDS_SIZE, fakes.GESTURES_SIZE),
            'backwards': fakes.DUMP_MAGIC + struct.pack('<II', fakes.HANDS_SIZE, fakes.GESTURES_SIZE)
            + hands_set(base, (0, 0)) + hands_set(base - 1, (0, 0)),
        }
        player = self.player()
        for name, data in cases.items():
            with self.subTest(name):
                path = self.dir / 'bad.fhdump'
                path.write_bytes(data)
                reply = player.command(f'replay-hands {path} once')
                self.assertFalse(reply['ok'], reply)
                self.assertIsNone(reply['hands'])
                self.assertFalse((self.dir / 'fake/hands').exists())

    def test_a_replay_waits_for_ft_hands_to_park_and_the_gaze_waits_with_it(self):
        # A pinch in the first 0.1 s, on a fresh daemon that waits 0.3 s for ft-hands to park.
        base = 3_000_000_000_000
        hands = self.dump([hands_set(base, (1, 0)), hands_set(base + 100_000_000, (1, 1))])
        gaze = self.gaze_recording([(0.0, sample(1)), (0.1, sample(2))])
        with mock.patch.object(fakes, 'HANDS_SETTLE', 0.3):
            player = self.player()
            self.assertTrue(player.command(f'replay-gaze {gaze} once')['ok'])
            self.assertTrue(player.command(f'replay-hands {hands} once')['ok'])
            pinches, eyes = [], []
            end = time.monotonic() + 2
            while player.active() and time.monotonic() < end:
                player.tick(time.monotonic())
                if (self.dir / 'hands/gestures').exists() and (not pinches or pinches[-1][0] != self.gestures()[1]):
                    pinches.append((self.gestures()[1], time.monotonic()))
                if (self.dir / 'fake/eye-server.mmap').exists():
                    left = self.eye_file()[1]['left2']
                    if not eyes or not close(eyes[-1][0], left):
                        eyes.append((left, time.monotonic()))
                time.sleep(0.002)
        self.assertEqual([p[0][2:] for p in pinches[:2]], [(1, 0), (1, 1)])   # nothing skipped
        self.assertTrue(close(eyes[0][0], fakes.direction(1, 0)))   # held while waiting
        second = next(t for d, t in eyes if close(d, fakes.direction(2, 0)))
        self.assertLess(abs(second - pinches[1][1]), 0.05)   # in step with the hands

    def test_a_scripted_pinch_down_ends_before_a_replay_so_counters_never_go_back(self):
        player = self.player()
        self.assertTrue(player.command('hand right 0.1 -0.2 -0.35 1 0 0 0 pinch')['ok'])
        player.tick(time.monotonic())
        begins, ends = self.gestures()[1][2:]
        self.assertEqual(begins, ends + 1)
        base = 4_000_000_000_000
        path = self.dump([hands_set(base, (0, 0)), hands_set(base + 50_000_000, (0, 0))])
        self.assertTrue(player.command(f'replay-hands {path} once')['ok'])
        seen = []
        end = time.monotonic() + 1
        while player.active() and time.monotonic() < end:
            player.tick(time.monotonic())
            seen.append(self.gestures()[1][2:])
            time.sleep(0.002)
        self.assertTrue(all(b >= begins and e >= ends for b, e in seen), seen)
        self.assertEqual(seen[0], (begins, begins))   # the scripted pinch ended, nothing began

    def test_a_damaged_set_stops_the_hands_alone(self):
        base = 6_000_000_000_000
        bad = bytearray(hands_set(base + 50_000_000, (0, 0)))
        bad[:8] = b'XXXXXXXX'
        path = self.dump([hands_set(base, (0, 0)), bytes(bad)])
        player = self.player()
        self.assertTrue(player.command('gaze 0 0 0 0')['ok'])
        self.assertTrue(player.command(f'replay-hands {path} loop')['ok'])
        self.run_for(player, 0.3)
        self.assertIsNone(player.status()['hands'])
        self.assertIsNotNone(player.status()['gaze'])

    def test_scripted_commands_and_a_replay_take_turns(self):
        base = 2_000_000_000_000
        hands = self.dump([hands_set(base, (0, 0)), hands_set(base + 50_000_000, (0, 0))])
        gaze = self.gaze_recording([(0.0, sample(1))])
        player = self.player()
        self.assertFalse(player.command(f'replay-hands {hands} twice')['ok'])
        self.assertTrue(player.command(f'replay-gaze {gaze} loop')['ok'])
        self.assertFalse(player.command('eyes open lost')['ok'])   # the recording says how the eyes are
        self.assertTrue(player.command(f'replay-hands {hands} loop')['ok'])
        reply = player.command('hand left 0 -0.2 -0.3 1 0 0 0 open')
        self.assertEqual(list(reply['hands']), ['left'])   # the replay stopped
        self.assertTrue(player.command(f'replay-hands {hands} loop')['ok'])
        self.assertTrue(player.command('hand-release left')['ok'])
        self.assertIsNone(player.status()['hands'])


class RecorderTests(Base):
    """A scripted fake plays the real trackers here: record reads its files as it would theirs."""

    def setUp(self):
        super().setUp()
        source = fakes.Fakes(str(self.dir / 'source'), str(self.dir / 'source-hands'))
        source.command('gaze 8 -4 6 -4')
        source.command('hand right 0.1 -0.2 -0.35 1 0 0 0 pinch')
        self.stop = threading.Event()

        def run():
            while not self.stop.is_set():
                source.tick(time.monotonic())
                time.sleep(0.002)
            source.close()
        thread = threading.Thread(target=run)
        thread.start()
        self.addCleanup(thread.join)
        self.addCleanup(self.stop.set)
        for name, value in (('EYE_SERVER', str(self.dir / 'source/eye-server.mmap')),
                            ('OWN_GAZE', str(self.dir / 'source/frametop-eyes-gaze'))):
            patch = mock.patch.object(record, name, value)
            patch.start()
            self.addCleanup(patch.stop)
        patch = mock.patch.object(fakes, 'HANDS_DIR', str(self.dir / 'source-hands'))
        patch.start()
        self.addCleanup(patch.stop)
        time.sleep(0.2)

    def test_record_then_replay_gives_back_what_the_trackers_published(self):
        out = self.dir / 'rec'
        result = record.record(str(out), 0.5)
        self.assertEqual(result['files'], ['gaze.jsonl', 'hands.fhdump'])
        self.assertGreater(result['recorded']['mmap'], 10)
        self.assertGreater(result['recorded']['own'], 10)
        self.assertGreater(result['recorded']['sets'], 4)
        self.assertEqual(oct(out.stat().st_mode & 0o777), '0o700')
        self.assertEqual(oct((out / 'gaze.jsonl').stat().st_mode & 0o777), '0o600')
        mmap_samples, own_samples = fakes.load_gaze(str(out / 'gaze.jsonl'))
        self.assertTrue(close(mmap_samples[0][1]['left2'], fakes.direction(8, -4)))
        self.assertTrue(close(own_samples[0][1]['eyes'], [6, -4, 8, -4]))
        dump = fakes.HandsDump(str(out / 'hands.fhdump'))
        self.addCleanup(dump.close)
        hands, gestures, _ = dump.read(0)
        self.assertEqual(struct.unpack_from('<I', hands, 64)[0], fakes.HAND_IDS['right'])
        self.assertEqual(struct.unpack_from('<4I', gestures, 64 + 64)[:2],
                         (fakes.FH_PINCH_TRACKED | fakes.FH_PINCH_DOWN, fakes.HAND_IDS['right']))
        self.stop.set()
        # Played back into the real places' stand-ins: the same samples.
        player = self.player()
        self.assertTrue(player.command(f'replay-gaze {out / "gaze.jsonl"} loop')['ok'])
        self.run_for(player, 0.1)
        self.assertTrue(close(self.eye_file()[1]['right2'], fakes.direction(6, -4)))

    def test_it_refuses_to_record_the_testbench_itself(self):
        Path(fakes.FAKE_DIR).mkdir(mode=0o700)
        (Path(fakes.FAKE_DIR) / 'hands').touch()
        with self.assertRaisesRegex(RuntimeError, 'faking hands'):
            record.record(str(self.dir / 'r1'), 0.1)
        self.assertFalse((self.dir / 'r1').exists())
        result = record.record(str(self.dir / 'r2'), 0.1, hands=False)
        self.assertEqual(result['files'], ['gaze.jsonl'])
        for seconds in (0, -1, 301):
            with self.assertRaises(ValueError):
                record.record(str(self.dir / 'r3'), seconds)
        self.assertFalse((self.dir / 'r3').exists())

    def test_nothing_to_record_leaves_nothing_behind(self):
        self.stop.set()
        with mock.patch.object(record, 'EYE_SERVER', str(self.dir / 'none')), \
                mock.patch.object(record, 'OWN_GAZE', str(self.dir / 'none')):
            with self.assertRaisesRegex(RuntimeError, 'no eye tracker'):
                record.record(str(self.dir / 'r4'), 0.1)
        with mock.patch.object(fakes, 'HANDS_DIR', str(self.dir / 'none')):
            with self.assertRaisesRegex(RuntimeError, "isn't publishing"):
                record.record(str(self.dir / 'r4'), 0.1, gaze=False)
        with mock.patch.object(record, 'seqlock_copy', side_effect=OSError('gone')):
            with self.assertRaises(OSError):
                record.record(str(self.dir / 'r4'), 0.1, gaze=False)
        self.assertFalse((self.dir / 'r4').exists())


class CliTests(Base):
    def setUp(self):
        super().setUp()
        patch = mock.patch.object(cli, 'RECORDINGS', self.dir / 'recordings')
        patch.start()
        self.addCleanup(patch.stop)
        self.sent = []

        class Bench(cli.Bench):
            def fakes(bench, text, start=False):
                self.sent.append((text, start))
                return {'ok': True}
        self.bench = Bench()

    def run_cli(self, *argv):
        return cli.dispatch(cli.parser().parse_args(list(argv)), self.bench)

    def test_replay_sends_each_file_of_a_recording(self):
        rec = self.dir / 'recordings/look-left'
        rec.mkdir(parents=True)
        (rec / 'gaze.jsonl').touch()
        (rec / 'hands.fhdump').touch()
        self.run_cli('replay', 'look-left')
        self.assertEqual(self.sent, [(f'replay-gaze {rec}/gaze.jsonl once', True),
                                     (f'replay-hands {rec}/hands.fhdump once', True)])
        self.sent.clear()
        self.run_cli('replay', 'look-left', '--loop', '--only', 'hands')
        self.assertEqual(self.sent, [(f'replay-hands {rec}/hands.fhdump loop', True)])
        with self.assertRaisesRegex(ValueError, 'recordings: look-left'):
            self.run_cli('replay', 'nope')
        (rec / 'hands.fhdump').unlink()
        with self.assertRaisesRegex(ValueError, 'no hands'):
            self.run_cli('replay', 'look-left', '--only', 'hands')

    def test_names_are_plain_and_recordings_are_never_overwritten(self):
        for bad in ('../x', '.hidden', 'a b', 'a/b', '', 'x' * 65):
            with self.subTest(bad=bad), self.assertRaises(SystemExit), mock.patch('sys.stderr'):
                cli.parser().parse_args(['record', bad, '--seconds', '1'])
        (self.dir / 'recordings/taken').mkdir(parents=True)
        with self.assertRaisesRegex(ValueError, 'already exists'):
            self.run_cli('record', 'taken', '--seconds', '1')


if __name__ == '__main__':
    unittest.main()
