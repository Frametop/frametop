#!/usr/bin/env python3
"""sounds.py: the beeps' WAV files, and a player that never stops a round."""
import os
import shutil
import sys
import tempfile
import unittest
import wave
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import sounds  # noqa: E402


class SoundsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="eyerec-sounds-")
        self.env = mock.patch.dict(os.environ, {"XDG_RUNTIME_DIR": self.tmp})
        self.env.start()

    def tearDown(self):
        self.env.stop()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_wavs(self):
        self.assertEqual(set(sounds.SOUNDS), {"blink", "close", "open", "go", "done"})
        self.assertEqual(len({sounds.SOUNDS[n] for n in ("blink", "close", "go", "done")}), 4, "told apart by ear")
        self.assertEqual(sounds.SOUNDS["close"], sounds.SOUNDS["open"], "open is the same beep again")
        for name, (hz, secs) in sounds.SOUNDS.items():
            path = sounds.wav_path(name)
            self.assertTrue(path.startswith(self.tmp))
            with wave.open(path) as w:
                self.assertEqual((w.getnchannels(), w.getsampwidth(), w.getframerate()), (1, 2, sounds.RATE))
                self.assertEqual(w.getnframes(), int(sounds.RATE * secs))
                frames = w.readframes(w.getnframes())
            first, last = int.from_bytes(frames[:2], "little", signed=True), int.from_bytes(frames[-2:], "little", signed=True)
            self.assertEqual((first, last), (0, 0), "faded in and out: no click")

    def test_player_off_and_missing(self):
        logged = []
        self.assertFalse(sounds.Player(on=False, log=logged.append).play("blink"))
        self.assertEqual(logged, [])
        with mock.patch.object(sounds.shutil, "which", return_value=None):
            p = sounds.Player(log=logged.append)
            self.assertFalse(p.play("close"))
        self.assertEqual(logged, ["no pw-play: the sound cues are off"])
        with self.assertRaises(ValueError):
            p.play("honk")


if __name__ == "__main__":
    unittest.main()
