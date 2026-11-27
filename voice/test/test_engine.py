#!/usr/bin/env python3
"""voice_engine.py offline: how a recording is cut down to its speech, where a long one is cut
into pieces and how their texts join, and when the recognizer's model loads and leaves memory.
No model: a stand-in is "loaded". Needs numpy (the image has it).
Run: python3 voice/test/test_engine.py"""
import sys
import threading
import unittest
from pathlib import Path
from unittest import mock

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import voice_engine as ve  # noqa: E402


class Squeeze(unittest.TestCase):
    def test_silence_is_cut_short(self):
        samples = np.arange(1000, dtype=np.float32)
        out = ve.squeeze(samples, [(100, 200), (600, 700)], 50)
        self.assertEqual(out.tolist(), [*range(50, 250), *range(550, 750)])  # 300 of silence, now 100

    def test_close_spans_and_the_ends(self):
        samples = np.arange(1000, dtype=np.float32)
        # The pads of two spans 60 apart overlap: nothing is there twice. The ends stop at the audio's.
        out = ve.squeeze(samples, [(10, 200), (260, 990)], 50)
        self.assertEqual(out.tolist(), list(range(0, 1000)))

    def test_no_speech(self):
        self.assertIsNone(ve.squeeze(np.zeros(1000, np.float32), [], 50))


class PieceCut(unittest.TestCase):
    def test_longest_pause_in_the_second_half(self):
        # Speech, a 4 chunk pause, speech, a 6 chunk pause, speech: cut in the middle of the 6.
        speech = [True] * 40 + [False] * 4 + [True] * 20 + [False] * 6 + [True] * 10
        level = [0.1 if s else 0.001 for s in speech]
        self.assertEqual(ve.piece_cut(speech, level, 0, len(speech), 3), 67)
        # A longer pause in the first half doesn't count: a piece that short wastes the cut.
        speech = [True] * 10 + [False] * 12 + [True] * 30 + [False] * 4 + [True] * 24
        level = [0.1 if s else 0.001 for s in speech]
        self.assertEqual(ve.piece_cut(speech, level, 0, len(speech), 3), 54)

    def test_of_two_as_long_the_later(self):
        speech = [True] * 40 + [False] * 4 + [True] * 10 + [False] * 4 + [True] * 10
        self.assertEqual(ve.piece_cut(speech, [0.1] * len(speech), 0, len(speech), 3), 56)

    def test_a_pause_still_going_on(self):
        speech = [True] * 40 + [False] * 8
        self.assertEqual(ve.piece_cut(speech, [0.1] * 48, 0, 48, 3), 44)

    def test_no_pause_cuts_after_the_quietest_moment(self):
        speech = [True] * 60 + [False] * 2 + [True] * 20
        level = [0.1] * 82
        level[70] = 0.0001  # quieter than the two silent chunks, but the VAD hears speech there
        level[60], level[61] = 0.02, 0.01
        self.assertEqual(ve.piece_cut(speech, level, 0, 82, 3), 62)  # after the quieter silent chunk
        speech = [True] * 82  # no silence at all: after the quietest chunk of the second half
        level[10] = 0.00001
        self.assertEqual(ve.piece_cut(speech, level, 0, 82, 3), 71)

    def test_starts_where_the_last_cut_was(self):
        speech = [True] * 100 + [True] * 30 + [False] * 5 + [True] * 5
        self.assertEqual(ve.piece_cut(speech, [0.1] * 140, 100, 140, 3), 133)


class Join(unittest.TestCase):
    def test_pieces_are_one_text(self):
        self.assertEqual(ve.join_pieces(["Hello there.", "", "  How  are you? "]), "Hello there. How are you?")
        self.assertEqual(ve.join_pieces([]), "")
        self.assertEqual(ve.join_pieces(["", " "]), "")

    def test_a_seam_inside_a_sentence(self):
        # The model ends each piece like a sentence: "the." is a pause, not an end.
        self.assertEqual(ve.join_pieces(["We walked to the.", "Old bridge."]), "We walked to the old bridge.")
        self.assertEqual(ve.join_pieces(["I think that.", "I should go."]), "I think that I should go.")
        self.assertEqual(ve.join_pieces(["Because we're.", "Late"]), "Because we're late")
        # A word a sentence does end on keeps its period, and so does a question.
        self.assertEqual(ve.join_pieces(["We walked home.", "Then it rained."]), "We walked home. Then it rained.")
        self.assertEqual(ve.join_pieces(["Is it the?", "Old bridge."]), "Is it the? Old bridge.")

    def test_clean(self):
        self.assertEqual(ve.clean("  two\n words \t"), "two words")


class FakeRecognizer:
    """sherpa-onnx's OfflineRecognizer, as far as Parakeet.decode uses it."""
    def __init__(self, gate=None):
        self.gate = gate

    def create_stream(self):
        stream = type("S", (), {})()
        stream.accept_waveform = lambda rate, samples: setattr(stream, "n", (rate, len(samples)))
        stream.result = type("R", (), {"text": " heard "})()
        return stream

    def decode_stream(self, stream):
        if self.gate:
            self.gate.wait(5)


class Model(unittest.TestCase):
    def engine(self, **kw):
        self.made = []

        def make():
            self.made.append(1)
            return FakeRecognizer(**kw)
        return ve.Parakeet("/nowhere", make=make)

    def test_loads_when_first_wanted(self):
        p = self.engine()
        self.assertFalse(p.loaded)
        self.assertEqual(p.decode(np.zeros(ve.RATE, np.float32)), " heard ")
        self.assertEqual(p.decode(np.zeros(ve.RATE, np.float32)), " heard ")
        self.assertEqual(self.made, [1])
        self.assertTrue(p.loaded)

    def test_warm_loads_in_the_background(self):
        p = self.engine()
        p.warm()
        for t in threading.enumerate():
            if t.name == "parakeet-load":
                t.join(5)
        self.assertTrue(p.loaded)
        p.warm()  # there already: nothing loads twice
        p.decode(np.zeros(ve.RATE, np.float32))
        self.assertEqual(self.made, [1])

    def test_leaves_memory_only_when_idle(self):
        p = self.engine()
        self.assertTrue(p.unload_idle(300))  # never loaded: not there
        p.decode(np.zeros(ve.RATE, np.float32))
        self.assertFalse(p.unload_idle(300))  # used just now
        self.assertTrue(p.loaded)
        p.last_used -= 301
        p.warm()  # a recording started: that's a use
        self.assertFalse(p.unload_idle(300))
        p.last_used -= 301
        with mock.patch.object(ve, "give_back_memory") as given:
            self.assertTrue(p.unload_idle(300))
        self.assertFalse(p.loaded)
        given.assert_called_once()  # the memory goes back to the system, not only to malloc
        p.decode(np.zeros(ve.RATE, np.float32))  # and loads again when wanted
        self.assertEqual(self.made, [1, 1])

    def test_stays_while_it_decodes(self):
        gate = threading.Event()
        p = self.engine(gate=gate)
        t = threading.Thread(target=p.decode, args=(np.zeros(ve.RATE, np.float32),))
        t.start()
        while not p.loaded:
            pass
        p.last_used -= 1000
        self.assertFalse(p.unload_idle(300))  # mid-decode: not now, however long ago the last use
        gate.set()
        t.join(5)
        self.assertTrue(p.loaded)

    def test_a_model_that_cant_load(self):
        def make():
            raise RuntimeError("no such file")
        p = ve.Parakeet("/nowhere", make=make)
        with self.assertLogs("ft-voiced", "ERROR"):
            p._warm()  # the background load says so and goes
        with self.assertRaises(RuntimeError):  # and the decode that needed it fails, for ft-voiced to report
            p.decode(np.zeros(ve.RATE, np.float32))
        self.assertFalse(p.loaded)


if __name__ == "__main__":
    unittest.main()
