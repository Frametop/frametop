"""Frametop Voice's recognizer, and how a recording is prepared for it.

Parakeet TDT 0.6B v3 (NVIDIA, CC BY 4.0; sherpa-onnx's int8 export, fetched by voice/models.sh)
runs on the CPU and knows 25 European languages (voice_settings.PARAKEET_LANGUAGES). A recording
is decoded once, when it's over, not while it's spoken: the model hears whole sentences, which
is where its accuracy comes from. On the Frame (2026-11-27) two threads decode 17 s of speech
in 1.2 s and one thread in 1.9 s. A decode's memory grows with its audio (50 s took 2 GB),
which is why ft-voiced cuts a long recording into pieces (piece_cut) and joins their texts
(join_pieces).

It writes one voice. Of two speakers in a recording it often keeps one and drops the other's
sentences, sometimes all of them (two test clips spliced together came back empty).

Needs numpy and sherpa-onnx (the image has them). ft-voiced owns the mic, the VAD and when to
decode; nothing here touches asyncio.
"""
import ctypes
import logging
import re
import threading
import time
from pathlib import Path

import numpy as np

RATE = 16000
# Words a sentence rarely ends on, so a pause after one is a pause inside a sentence.
DANGLING = frozenset("""a an the to of for in on at by with from into onto about as than and or but nor
    because if when while where whether that which whose my our your their its i we they he she
    i'm we're you're they're it's are were be been being am have has had will would can
    could should shall may might must very""".split())

log = logging.getLogger("ft-voiced")


class Parakeet:
    """The model, loaded when it's first wanted (2 s) and dropped again once it's been idle:
    loaded, it holds 1 to 1.5 GB (the more the longer its decodes were). One load or decode at
    a time; safe to call from any thread."""

    def __init__(self, model_dir, threads=2, make=None):
        self.dir = Path(model_dir)
        self.threads = threads
        self._make = make or self._load
        self._lock = threading.Lock()
        self._rec = None
        self.last_used = 0.0

    def _load(self):
        import sherpa_onnx
        return sherpa_onnx.OfflineRecognizer.from_transducer(
            encoder=str(self.dir / "encoder.int8.onnx"), decoder=str(self.dir / "decoder.int8.onnx"),
            joiner=str(self.dir / "joiner.int8.onnx"), tokens=str(self.dir / "tokens.txt"),
            num_threads=self.threads, model_type="nemo_transducer")

    @property
    def loaded(self):
        return self._rec is not None

    def _recognizer(self):
        """The model, loaded first when it isn't. The lock is held."""
        if self._rec is None:
            t0 = time.monotonic()
            self._rec = self._make()
            log.info("parakeet: loaded in %.1fs (%s)", time.monotonic() - t0, self.dir.name)
        return self._rec

    def warm(self):
        """Start loading in the background, so the model is there when the recording ends (it
        takes about 2 s on the Frame). Counts as a use: unload_idle leaves it alone."""
        self.last_used = time.monotonic()
        if self._rec is None:
            threading.Thread(target=self._warm, name="parakeet-load", daemon=True).start()

    def _warm(self):
        try:
            with self._lock:
                self._recognizer()
        except Exception as err:  # decode() meets the same error and reports it
            log.error("parakeet: can't load %s: %s", self.dir, err)

    def decode(self, samples):
        """Text for 16 kHz mono float samples. Blocks: call it from a thread."""
        with self._lock:
            rec = self._recognizer()
            stream = rec.create_stream()
            stream.accept_waveform(RATE, samples)
            rec.decode_stream(stream)
            self.last_used = time.monotonic()
            return stream.result.text

    def unload_idle(self, seconds):
        """Drop the model when it hasn't been used for `seconds`. True when it isn't loaded
        (any more); False while it's in use or was used since."""
        if not self._lock.acquire(blocking=False):
            return False
        try:
            if self._rec is not None and time.monotonic() - self.last_used >= seconds:
                self._rec = None
                give_back_memory()
                log.info("parakeet: unloaded after %.0fs idle", seconds)
            return self._rec is None
        finally:
            self._lock.release()


def give_back_memory():
    """Freed memory goes back to the system, not only to malloc: without this, a process that
    dropped the model stayed at 230 MB, then 670, then 980 as it loaded and dropped it again;
    with it, at 70 every time."""
    try:
        ctypes.CDLL(None).malloc_trim(0)
    except (OSError, AttributeError):  # not glibc
        pass


def squeeze(samples, spans, pad):
    """A recording's speech: the VAD's `spans` ((start, end), in samples) with `pad` samples
    around each. The silence at both ends goes, and one between two spans is 2 * pad long at
    most, so a long think costs no decoding. None when there's no speech."""
    parts, done = [], 0
    for start, end in spans:
        a, b = max(done, start - pad), min(len(samples), end + pad)
        if b > a:
            parts.append(samples[a:b])
            done = b
    return np.concatenate(parts) if parts else None


def piece_cut(speech, level, first, end, min_pause):
    """Where to end a piece of a recording that got too long for one decode: the chunk index
    to cut before, in (first, end]. `speech` and `level` hold a flag and a loudness per chunk.
    The middle of the longest pause of at least `min_pause` chunks in the piece's second half
    (the later one of two as long); with no such pause, after its quietest chunk, one the VAD
    calls silence if there is any (a breath between words rather than the inside of one)."""
    half = first + (end - first) // 2
    runs, start = [], None
    for i in range(half, end):
        if not speech[i]:
            start = i if start is None else start
        elif start is not None:
            runs.append((start, i - start))
            start = None
    if start is not None:
        runs.append((start, end - start))
    runs = [r for r in runs if r[1] >= min_pause]
    if runs:
        start, length = max(runs, key=lambda r: (r[1], r[0]))
        return start + (length + 1) // 2
    return min(range(half, end), key=lambda i: (speech[i], level[i], -i)) + 1


def clean(text):
    return re.sub(r"\s+", " ", text).strip()


def join_pieces(texts):
    """The pieces' texts as one. A piece ends where the speaker paused, and the model reads the
    end of its audio as the end of a sentence: a period it put after a word no sentence ends on
    ("the", "to", "we are") goes, and so does the capital after it."""
    out = ""
    for text in filter(None, map(clean, texts)):
        if out:
            last = re.search(r"([\w']+)\.$", out)
            if last and last[1].lower() in DANGLING:
                out = out[:-1]
                if not re.match(r"I\b", text):
                    text = text[:1].lower() + text[1:]
            out += " "
        out += text
    return out
