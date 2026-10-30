"""The verify-frametop skill's mic-latency and mic-bursts, on synthetic recordings: no host needed."""
import importlib.machinery
import importlib.util
import json
import math
import random
import shutil
import subprocess
import sys
import tempfile
import unittest
import wave
from pathlib import Path
from unittest import mock

SCRIPTS = Path(__file__).resolve().parents[2] / '.claude/skills/verify-frametop/scripts'


def load(name):
    loader = importlib.machinery.SourceFileLoader(name.replace('-', '_'), str(SCRIPTS / name))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    with mock.patch.object(sys, 'dont_write_bytecode', True):   # no __pycache__ in the skill
        loader.exec_module(module)
    return module


def recording(offsets, seconds, noise=200, rate=48000):
    """Samples with a 1 kHz 30 ms burst at each offset (s) over noise."""
    rng = random.Random(4)
    samples = [int(rng.gauss(0, noise / 3)) for _ in range(int(seconds * rate))]
    for t in offsets:
        at = int(t * rate)
        for i in range(int(0.03 * rate)):
            if at + i < len(samples):
                samples[at + i] += int(9000 * math.sin(2 * math.pi * 1000 * i / rate))
    return samples


@unittest.skipUnless((SCRIPTS / 'mic-latency').exists(), 'the verify-frametop skill is not in this checkout')
class MicLatencyTests(unittest.TestCase):
    def setUp(self):
        self.m = load('mic-latency')

    def test_onsets_find_each_burst_once(self):
        found = self.m.onsets(recording([1.0, 2.0, 3.25], 4.0))
        self.assertEqual(len(found), 3)
        for got, want in zip(found, [1.0, 2.0, 3.25]):
            self.assertAlmostEqual(got, want, delta=0.002)
        self.assertEqual(self.m.onsets(recording([], 2.0)), [])
        self.assertEqual(self.m.onsets([]), [])

    def test_report_matches_bursts_and_counts_the_missed(self):
        played = [100.0 + i for i in range(30)]
        heard = [p + 0.150 + 0.002 * i for i, p in enumerate(played) if i != 7]   # one lost, 2 ms/s drift
        heard.append(50.0)   # a noise before any burst
        result = self.m.report(played, heard)
        self.assertEqual((result['matched'], result['missed'], result['unmatched_heard']), (29, 1, 1))
        self.assertAlmostEqual(result['latency_ms']['median'], 180, delta=3)
        self.assertAlmostEqual(result['latency_ms']['drift_per_min'], 120, delta=5)
        self.assertNotIn('latency_ms', self.m.report(played, []))

    def test_a_late_burst_goes_with_the_one_played_before_it(self):
        # 1.2 s late: past the next burst, still matched to its own (the latest before it is taken)
        pairs = self.m.match([10.0, 11.0], [11.2, 12.2])
        self.assertEqual(pairs, [(11.0, 11.2)])

    @unittest.skipUnless(shutil.which('ffmpeg') and shutil.which('ffprobe'), 'needs ffmpeg')
    def test_end_to_end_with_a_host_clock_ahead(self):
        tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, tmp)
        frame_start, offset, latency = 1_793_000_000.0, 1080.0, 0.120
        played = [frame_start + 2 + i for i in range(10)]
        (tmp / 'bursts.txt').write_text(''.join(f'{p:.4f}\n' for p in played))
        rec_start_frame = frame_start + 1.0   # the recording began 1 s in, on the Frame's clock
        samples = recording([p + latency - rec_start_frame for p in played], 13.0)
        with wave.open(str(tmp / 'rec.wav'), 'wb') as w:
            w.setnchannels(1), w.setsampwidth(2), w.setframerate(48000)
            w.writeframes(b''.join(max(-32768, min(32767, s)).to_bytes(2, 'little', signed=True) for s in samples))
        out = subprocess.run([sys.executable, str(SCRIPTS / 'mic-latency'), str(tmp / 'bursts.txt'), str(tmp / 'rec.wav'),
                              '--offset-ms', str(offset), '--start', str(rec_start_frame + offset / 1000)],
                             capture_output=True, text=True, check=True).stdout
        result = json.loads(out)
        self.assertEqual(result['matched'], 10)
        self.assertAlmostEqual(result['latency_ms']['median'], 120, delta=2)
        no_start = subprocess.run([sys.executable, str(SCRIPTS / 'mic-latency'), str(tmp / 'bursts.txt'),
                                   str(tmp / 'rec.wav'), '--offset-ms', '0'], capture_output=True, text=True)
        self.assertIn('--start', no_start.stderr)


@unittest.skipUnless((SCRIPTS / 'mic-bursts').exists(), 'the verify-frametop skill is not in this checkout')
class MicBurstsTests(unittest.TestCase):
    def test_bursts_are_logged_where_they_are_in_the_audio(self):
        tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, tmp)
        done = subprocess.run([sys.executable, str(SCRIPTS / 'mic-bursts'), '2.5', str(tmp / 'log')],
                              capture_output=True, check=True, timeout=10)
        log = [float(x) for x in (tmp / 'log').read_text().split()]
        self.assertEqual(len(log), 3)
        self.assertAlmostEqual(log[1] - log[0], 1.0, places=3)
        samples = [int.from_bytes(done.stdout[i:i + 2], 'little', signed=True) for i in range(0, len(done.stdout), 2)]
        self.assertGreaterEqual(len(samples), int(2.5 * 48000))
        self.assertTrue(any(samples[48000:48000 + 1440]))   # burst 2 at its second
        self.assertFalse(any(samples[48000 + 1440:96000]))   # quiet till the next


if __name__ == '__main__':
    unittest.main()
