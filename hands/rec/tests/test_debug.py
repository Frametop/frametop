#!/usr/bin/env python3
"""Debug sessions (DESIGN.md, "Debug sessions"): livelog.py's copies of the live tracker's
files, and debug.json's free step in a dry run (marks, finishing early, session.json's record).

  python3 hands/rec/tests/test_debug.py
"""
import json
import os
import shutil
import struct
import sys
import tempfile
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REC = os.path.dirname(HERE)
sys.path.insert(0, REC)
sys.path.insert(0, HERE)   # test_validate, also under python -I
import session  # noqa: E402
import livelog  # noqa: E402
import voicelog  # noqa: E402
import eventlog  # noqa: E402


class FakeTracker:
    """Publishes hands and gestures files in dir under the sequence lock, every period_s."""

    def __init__(self, d, period_s=0.01):
        self.d, self.period_s, self.seq = d, period_s, 0
        self.files = {"hands": (b"FHHANDS1", 2, 4704), "gestures": (b"FHGEST01", 3, 448)}
        for name in self.files:
            self._write(name, 0)
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)

    def _write(self, name, seq):
        magic, version, size = self.files[name]
        head = struct.pack("<8sIIQQQ", magic, version, size, seq, seq * 1000, session.mono_ns())
        with open(os.path.join(self.d, name), "r+b" if os.path.exists(os.path.join(self.d, name)) else "wb") as f:
            f.write(head + bytes([seq // 2 % 256]) * (size - len(head)))

    def _run(self):
        while not self._stop.is_set():
            self.seq += 2
            for name in self.files:
                self._write(name, self.seq)
            time.sleep(self.period_s)

    def __enter__(self):
        self._thread.start()
        return self

    def __exit__(self, *_):
        self._stop.set()
        self._thread.join()


class LiveLogTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp)

    def test_copies(self):
        path = os.path.join(self.tmp, "live.bin")
        with FakeTracker(self.tmp):
            log = livelog.LiveLog(self.tmp, path).start()
            time.sleep(0.3)
            counts = log.stop()
        recs = livelog.read(path)
        self.assertGreater(counts["hands"], 5)
        self.assertEqual(len(recs), counts["hands"] + counts["gestures"])
        for name in ("hands", "gestures"):
            mine = [r for r in recs if r[0] == name]
            seqs = [r[2] for r in mine]
            self.assertEqual(seqs, sorted(set(seqs)))   # each publish once, in order
            self.assertTrue(all(s % 2 == 0 for s in seqs))
            self.assertEqual({len(r[3]) for r in mine}, {4704 if name == "hands" else 448})
            for _, _, seq, data in mine:   # a whole copy of that publish, not half of the next
                self.assertEqual(struct.unpack_from("<Q", data, 16)[0], seq)
                self.assertEqual(data[-1], seq // 2 % 256)

    def test_odd_and_missing(self):
        """No file yet, or one being written (odd seq): nothing copied."""
        path = os.path.join(self.tmp, "live.bin")
        with open(os.path.join(self.tmp, "hands"), "wb") as f:
            f.write(struct.pack("<8sIIQ", b"FHHANDS1", 2, 4704, 7) + bytes(4704 - 24))
        counts = livelog.LiveLog(self.tmp, path).start()
        time.sleep(0.05)
        self.assertEqual(counts.stop(), {"hands": 0, "gestures": 0})
        self.assertEqual(livelog.read(path), [])


FAKE_PW_RECORD = """#!/bin/sh
# 0.5 s of a 1000-step ramp, then waits to be stopped
python3 -c 'import struct, sys; sys.stdout.buffer.write(struct.pack("<8000h", *range(-4000, 4000)))'
exec sleep 30
"""


class VoiceLogTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp)
        self.path = os.environ.get("PATH", "")
        self.addCleanup(os.environ.__setitem__, "PATH", self.path)

    def fake(self, script):
        bindir = os.path.join(self.tmp, "bin")
        os.makedirs(bindir, exist_ok=True)
        exe = os.path.join(bindir, "pw-record")
        with open(exe, "w") as f:
            f.write(script)
        os.chmod(exe, 0o755)
        os.environ["PATH"] = bindir + os.pathsep + self.path

    def test_records(self):
        self.fake(FAKE_PW_RECORD)
        path = os.path.join(self.tmp, "voice.bin")
        log = voicelog.VoiceLog(path).start()
        end = time.monotonic() + 5
        while log.samples < 8000 and time.monotonic() < end:
            time.sleep(0.02)
        counts = log.stop()
        self.assertEqual(counts["error"], "")
        self.assertEqual(counts["seconds"], 0.5)
        self.assertEqual(counts["peak"], round(4000 / 32768, 3))
        rate, recs = voicelog.read(path)
        self.assertEqual(rate, 16000)
        self.assertEqual(len(recs), counts["chunks"])
        self.assertEqual(sum(len(d) for _, d in recs), 16000)
        self.assertEqual(recs, sorted(recs, key=lambda r: r[0]))
        wav = os.path.join(self.tmp, "voice.wav")
        voicelog.to_wav(path, wav)
        import wave
        with wave.open(wav) as w:
            self.assertEqual(w.getframerate(), 16000)
            self.assertGreaterEqual(w.getnframes(), 8000)

    def test_no_microphone(self):
        """No pw-record, or one that fails at once: an error to report, nothing raised."""
        os.environ["PATH"] = os.path.join(self.tmp, "empty")
        counts = voicelog.VoiceLog(os.path.join(self.tmp, "voice.bin")).start().stop()
        self.assertIn("pw-record", counts["error"])
        self.fake("#!/bin/sh\nexit 1\n")
        counts = voicelog.VoiceLog(os.path.join(self.tmp, "voice.bin")).start()
        time.sleep(0.2)
        counts = counts.stop()
        self.assertEqual(counts["seconds"], 0)
        self.assertIn("exited", counts["error"])
        self.assertEqual(voicelog.read(os.path.join(self.tmp, "voice.bin")), (16000, []))


class VoiceFlagsTest(unittest.TestCase):
    """hands/tools/voice_flags.py: a testing take's marks and spoken notes, with the events around them."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp)
        sys.path.insert(0, os.path.join(os.path.dirname(REC), "tools"))
        self.addCleanup(sys.path.remove, sys.path[0])
        import voice_flags
        self.vf = voice_flags

    def test_loopback_only(self):
        for url in ("http://127.0.0.1:8178", "http://localhost:8178", "http://[::1]:8178"):
            self.assertTrue(self.vf.loopback(url), url)
        for url in ("http://192.168.1.5:8178", "https://127.0.0.1:8178", "http://example.com", "127.0.0.1:8178"):
            self.assertFalse(self.vf.loopback(url), url)

    def test_bursts(self):
        """Speech over the noise floor, short pauses kept in, silence left out."""
        import array
        rate = 16000
        noise, loud = [3, -3] * (rate // 2), [3000, -3000] * (rate // 2)
        samples = array.array("h", noise * 2 + loud + noise[:rate // 2] + loud + noise * 3 + loud)
        got = self.vf.bursts(samples, rate)
        pad = int(self.vf.PAD_S * rate)
        self.assertEqual(got, [(2 * rate - pad, 4.5 * rate + pad), (7.5 * rate - pad, 8.5 * rate)])

    def test_flags(self):
        """Marks and notes in time order, a note paired with a mark under PAIR_S away, events around each."""
        take = os.path.join(self.tmp, "01-free")
        os.makedirs(take)
        rate, t0 = voicelog.RATE, 1_000_000_000_000
        with open(os.path.join(take, "take.json"), "w") as f:
            json.dump({"started_ns": t0}, f)
        with open(os.path.join(take, "prompts.jsonl"), "w") as f:
            f.write(json.dumps({"t": t0 + 11 * 10**9, "event": "mark", "id": "use", "n": 1}) + "\n")
            f.write(json.dumps({"t": t0 + 40 * 10**9, "event": "mark", "id": "use", "n": 2}) + "\n")
        with open(os.path.join(take, "events.jsonl"), "w") as f:
            f.write(json.dumps({"t": t0 + 9 * 10**9, "kind": "poke", "outcome": "missed"}) + "\n")
            f.write(json.dumps({"t": t0 + 30 * 10**9, "kind": "grab", "outcome": "done"}) + "\n")
        with open(os.path.join(take, "voice.bin"), "wb") as f:     # speech 10-11 s and 20-21 s
            f.write(voicelog.HDR.pack(voicelog.MAGIC, 1, rate))
            for k in range(1, 31):
                loud = k in (11, 21)
                f.write(voicelog.REC.pack(t0 + k * 10**9, rate * 2))
                f.write(struct.pack("<h", 3000 if loud else 0) * rate)
        said = iter(["that poke missed", "fine now"])
        orig = self.vf.transcribe
        self.vf.transcribe = lambda wav, server: [(0.0, 1.0, next(said))]
        self.addCleanup(setattr, self.vf, "transcribe", orig)
        out = self.vf.flags(take)
        self.assertEqual([(f["source"], round(f["s"])) for f in out],
                         [("voice", 10), ("mark", 11), ("voice", 20), ("mark", 40)])
        self.assertEqual(out[0]["text"], "that poke missed")
        self.assertEqual(out[0]["mark"], 1)
        self.assertIsNone(out[2]["mark"])
        self.assertEqual([e["kind"] for e in out[0]["events"]], ["poke"])
        self.assertEqual(out[3]["events"], [])
        self.assertIn('voice (mark 1): "that poke missed"  [poke 1 missed]', self.vf.line(out[0]))


class SettingsTest(unittest.TestCase):
    def test_settings(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp)
        conf = os.path.join(tmp, "frametop.conf")
        with open(conf, "w") as f:
            f.write("HANDS_MODELS=/home/someone/models/student-v13b-p1  # a note\n"
                    "POINTER_ROLE=right\n# HANDS_MODE=off\nGAZE_EYE=both\nHANDS_CPUS='5,6,7'\n")
        self.assertEqual(session.live_settings(conf), {"HANDS_MODELS": "student-v13b-p1", "POINTER_ROLE": "right",
                                                       "HANDS_CPUS": "5,6,7"})


class DebugSessionTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp)
        self.live = os.path.join(self.tmp, "live")
        os.makedirs(self.live)
        self.panel = []

    def run_debug(self, act):
        """A dry debug session; act(s) runs once its free step is running."""
        s = session.Session(os.path.join(self.tmp, "base"), {}, {}, "room", session.DEBUG_SCRIPT, dry_run=True,
                            speed=5, next_after=0.02, poses_dir=os.path.join(self.tmp, "no-poses"), button=False)
        self.assertTrue(s.debug)
        self.assertEqual(s.quiet_units, ())   # Frametop's hands and pointer stay on: they're what's tested
        s.live_dir = self.live
        s.print = self.panel.append
        with FakeTracker(self.live):
            s.start()
            end = time.monotonic() + 30
            while s.state != "running" and time.monotonic() < end:
                time.sleep(0.01)
            time.sleep(0.1)
            act(s)
            while s._thread.is_alive() and time.monotonic() < end:
                s.join(0.1)
        return s

    def events(self, s):
        with open(os.path.join(s.session_dir, "takes", "01-free", "prompts.jsonl")) as f:
            return [json.loads(line) for line in f]

    def test_marks_and_finish(self):
        def act(s):
            s.button_gesture("press")
            time.sleep(0.1)
            s.mark()
            time.sleep(0.1)
            s.button_gesture("hold")   # finish, not stop
        s = self.run_debug(act)
        self.assertEqual(s.state, "done")
        ev = self.events(s)
        self.assertEqual([e["n"] for e in ev if e["event"] == "mark"], [1, 2])
        self.assertEqual([e["event"] for e in ev][-3:], ["end_early", "wait", "end"])
        self.assertIn("panel: hide", self.panel)
        self.assertIn("panel: big Marked", self.panel)
        with open(os.path.join(s.session_dir, "session.json")) as f:
            sj = json.load(f)
        self.assertEqual(sj["status"], "done")
        self.assertEqual(sj["debug"]["use"], "testing")
        self.assertEqual(sj["record_hz"], 30)
        with open(os.path.join(s.session_dir, "takes", "01-free", "take.json")) as f:
            tj = json.load(f)
        self.assertEqual(tj["status"], "complete")
        self.assertGreater(tj["live"]["live.bin"]["hands"], 3)
        recs = livelog.read(os.path.join(s.session_dir, "takes", "01-free", "live.bin"))
        self.assertEqual(len(recs), tj["live"]["live.bin"]["hands"] + tj["live"]["live.bin"]["gestures"])

    def test_double_press_marks(self):
        """Two quick presses mark (twice pressed, one moment): never a redo, which would throw the
        recording away (2026-10-14: a testing session lost its recording that way)."""
        def act(s):
            s.button_gesture("double")
            time.sleep(0.1)
            s.redo()   # the window's R does nothing either
            time.sleep(0.1)
            s.button_gesture("hold")
        s = self.run_debug(act)
        self.assertEqual(s.state, "done")
        ev = self.events(s)
        self.assertEqual([e["n"] for e in ev if e["event"] == "mark"], [1])
        self.assertNotIn("redo", [e["event"] for e in ev])
        with open(os.path.join(s.session_dir, "takes", "01-free", "take.json")) as f:
            self.assertEqual(sorted(json.load(f)["live"]), ["live.bin"])

    def test_pause_parts(self):
        """A pause ends the recording part, and its live log with it: live-2.bin for the next."""
        def act(s):
            s.pause()
            time.sleep(0.2)
            s.resume()
            time.sleep(0.2)
            s.finish()
        s = self.run_debug(act)
        self.assertEqual(s.state, "done")
        with open(os.path.join(s.session_dir, "takes", "01-free", "take.json")) as f:
            self.assertEqual(sorted(json.load(f)["live"]), ["live-2.bin", "live.bin"])

    def test_voice(self):
        """Ticked "Record my voice": each recording part has its voice file beside the live log."""
        made = []

        class FakeVoice:
            def __init__(self, path):
                self.path, self.error = path, ""
                made.append(os.path.basename(path))

            def start(self):
                with open(self.path, "wb") as f:
                    f.write(voicelog.HDR.pack(voicelog.MAGIC, 1, voicelog.RATE))
                return self

            def stop(self):
                return {"seconds": 1.0, "chunks": 10, "peak": 0.2, "error": ""}

        orig = session.voicelog.VoiceLog
        session.voicelog.VoiceLog = FakeVoice
        self.addCleanup(setattr, session.voicelog, "VoiceLog", orig)

        def act(s):
            s.pause()
            time.sleep(0.2)
            s.resume()
            time.sleep(0.2)
            s.finish()
        orig_init = session.Session.__init__

        def init(s, *a, **k):
            orig_init(s, *a, **k)
            self.assertFalse(s.voice)   # a dry run records no voice, even ticked
            s.voice = True
        session.Session.__init__ = init
        self.addCleanup(setattr, session.Session, "__init__", orig_init)
        s = self.run_debug(act)
        self.assertEqual(s.state, "done")
        self.assertEqual(made, ["voice.bin", "voice-2.bin"])
        with open(os.path.join(s.session_dir, "takes", "01-free", "take.json")) as f:
            self.assertEqual(sorted(json.load(f)["voice"]), ["voice-2.bin", "voice.bin"])

    def test_events(self):
        """The panels' and pointer's events (fh_events.h) while each part records go into its
        events file: events-want asked for them, an older line is left out, and nothing is asked
        for after the session."""
        want = os.path.join(self.live, eventlog.WANT)
        src = os.path.join(self.live, eventlog.FILE)
        with open(src, "w") as f:   # a crashed session's
            f.write('{"t": 1, "src": "screens", "kind": "poke", "outcome": "done", "hand": "right", "panel": 1, "text": "old"}\n')

        def write(kind, outcome):
            self.assertTrue(os.path.exists(want))
            with open(src, "a") as f:
                f.write(json.dumps({"t": eventlog.mono_ns(), "src": "screens", "kind": kind, "outcome": outcome,
                                    "hand": "right", "panel": 2, "text": kind}) + "\n")
                f.write("not json\n")

        def act(s):
            write("lazy-susan", "done")
            write("poke", "refused")
            s.pause()
            time.sleep(0.2)
            self.assertFalse(os.path.exists(want))
            s.resume()
            time.sleep(0.2)
            write("poke", "done")
            s.finish()
        s = self.run_debug(act)
        self.assertEqual(s.state, "done")
        self.assertFalse(os.path.exists(want))
        self.assertFalse(os.path.exists(src))
        tdir = os.path.join(s.session_dir, "takes", "01-free")
        self.assertEqual([e["kind"] for e in eventlog.read_lines(os.path.join(tdir, "events.jsonl"))], ["lazy-susan", "poke"])
        self.assertEqual([e["outcome"] for e in eventlog.read_lines(os.path.join(tdir, "events-2.jsonl"))], ["done"])
        with open(os.path.join(tdir, "take.json")) as f:
            tj = json.load(f)
        self.assertEqual(tj["events"]["events.jsonl"], {"events": 2, "by": {"lazy-susan": {"done": 1}, "poke": {"refused": 1}}})
        self.assertEqual(tj["events"]["events-2.jsonl"]["by"], {"poke": {"done": 1}})

    def test_needs_live_hands(self):
        s = session.Session(os.path.join(self.tmp, "base"), {}, {}, "room", session.DEBUG_SCRIPT, button=False)
        s.dry_run = False   # the check, without starting anything
        orig = session.unit_active
        session.unit_active = lambda unit: False
        try:
            self.assertTrue(s._preflight())
        finally:
            session.unit_active = orig
        self.assertEqual(s.state, "error")
        self.assertEqual(s._status["error"], session.DEBUG_NEEDS_HANDS)


class DebugExportTest(unittest.TestCase):
    """A debug session's export: its recordings encrypted to the debug keys, the live parts as one
    live.bin (deleted ranges left out), and validate.py's checks of it."""

    def setUp(self):
        import takes
        import validate
        from recorder import age
        from test_validate import SESSION, make_session
        try:
            self.identity, recipient = age.keygen()
        except RuntimeError as e:
            self.skipTest(str(e))
        if not takes.find_zstd():
            self.skipTest("no zstd")
        self.takes, self.validate, self.age, self.sid = takes, validate, age, SESSION
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp)
        keys = os.path.join(self.tmp, "recipients.txt")
        with open(keys, "w") as f:
            f.write("# test\n%s\n" % recipient)
        for mod in (takes, validate):
            orig = mod.TEST_RECIPIENTS
            mod.TEST_RECIPIENTS = keys
            self.addCleanup(setattr, mod, "TEST_RECIPIENTS", orig)
        self.base = os.path.join(self.tmp, "base")
        make_session(self.base)
        # recorded under the testing consent (TESTING.md), with the window's own profile keys
        profile = takes.read_json(os.path.join(self.base, "profile.json"))
        profile["testing_consent"] = dict(profile.pop("consent"), version="2026-10-14", region_ok=True)
        profile["bounties"] = {"index-taps": {"done": 1}}
        takes.write_json(os.path.join(self.base, "profile.json"), profile)
        sdir = os.path.join(self.base, "sessions", SESSION)
        self.session_json = os.path.join(sdir, "session.json")
        sj = takes.read_json(self.session_json)
        sj["debug"] = {"use": "testing", "max_s": 180, "settings": {"POINTER_ROLE": "right"}, "frametop": "", "models": {}}
        sj["consent"] = {"kind": "testing", "version": "2026-10-14"}
        takes.write_json(self.session_json, sj)
        self.tdir = os.path.join(sdir, "takes", "01-hand-size")
        t0 = 10 ** 12
        for name, times in (("live.bin", range(0, 10)), ("live-2.bin", range(10, 20))):
            with open(os.path.join(self.tdir, name), "wb") as f:
                f.write(livelog.FILE_HDR.pack(livelog.MAGIC, livelog.VERSION, 0))
                for i in times:
                    for src, n in ((0, 4704), (1, 448)):
                        f.write(livelog.REC_HDR.pack(src, n, t0 + i * 100_000_000, 2 * i))
                        f.write(bytes([i]) * n)
                f.write(livelog.REC_HDR.pack(0, 4704, t0, 99)[:10])   # cut off mid-write
        self.store = takes.Store(self.base)
        index = takes.take_index(self.tdir)
        self.store.delete_range(SESSION, "01-hand-size", index.time_ns(3), index.time_ns(5))

    def test_export(self):
        export = self.store.export(self.sid, low_priority=False)
        files = sorted(os.path.relpath(os.path.join(r, n), export) for r, _, ns in os.walk(export) for n in ns)
        self.assertNotIn("takes/01-hand-size/sets.bin.zst", files)
        self.assertIn("takes/01-hand-size/sets.bin.zst.age", files)
        self.assertIn("takes/01-hand-size/live.bin.zst.age", files)
        self.assertNotIn("takes/02-no-hands/live.bin.zst.age", files)   # no live parts there
        with open(os.path.join(export, "manifest.json")) as f:
            m = json.load(f)
        self.assertEqual(sorted(m["encryption"]["files"]), ["takes/01-hand-size/live.bin.zst.age",
                                                           "takes/01-hand-size/sets.bin.zst.age",
                                                           "takes/02-no-hands/sets.bin.zst.age"])
        live = m["takes"][0]["live"]
        self.assertEqual(live["records"], {"hands": 17, "gestures": 17})   # 3 of 20 in the deleted range
        r = self.validate.validate(export)
        self.assertEqual(r.errors, [])
        self.assertTrue(r.summary["debug"])
        self.assertEqual(m["consent_kind"], "testing")
        self.assertEqual(m["consent_version"], "2026-10-14")
        self.assertEqual(sorted(m["profile"]), ["contributor", "optional", "schema", "testing_consent"])
        # the key holder gets the recordings back
        import subprocess
        with open(os.path.join(export, live["file"]), "rb") as f:
            raw = subprocess.run([self.takes.find_zstd(), "-dc"], input=self.age.decrypt_bytes(f.read(), self.identity),
                                 capture_output=True, check=True).stdout
        out = os.path.join(self.tmp, "live.bin")
        with open(out, "wb") as f:
            f.write(raw)
        recs = livelog.read(out)
        self.assertEqual(len(recs), 34)
        self.assertEqual(len(raw), live["raw_bytes"])
        with open(os.path.join(export, "takes/02-no-hands/sets.bin.zst.age"), "rb") as f:
            sets = subprocess.run([self.takes.find_zstd(), "-dc"], input=self.age.decrypt_bytes(f.read(), self.identity),
                                  capture_output=True, check=True).stdout
        self.assertEqual(len(sets), m["takes"][1]["raw_bytes"])

    def write_voice(self):
        """voice.bin and voice-2.bin around the deleted range: a chunk before it, one ending in it,
        one inside, one starting in it, one after; the second file also cut off mid-write."""
        a, b = self.takes.read_json(os.path.join(self.tdir, "take.json"))["deleted"][0]
        ms = 1_000_000
        chunk = bytes(range(64)) * 50   # 3200 bytes: 100 ms at 16 kHz
        ends = [(a - 500 * ms, a + 50 * ms, (a + b) // 2), (min(b, a + 50 * ms) + 120 * ms, b + 500 * ms)]
        for name, part in zip(("voice.bin", "voice-2.bin"), ends):
            with open(os.path.join(self.tdir, name), "wb") as f:
                f.write(voicelog.HDR.pack(voicelog.MAGIC, 1, 16000))
                for t in part:
                    f.write(voicelog.REC.pack(t, len(chunk)) + chunk)
                if name == "voice-2.bin":
                    f.write(voicelog.REC.pack(b + 900 * ms, len(chunk)) + chunk[:100])
        # chunks: kept, ends in range (gone), inside (gone), starts in range (gone if b - a > 70 ms), kept
        return 2 if b - a > 70 * ms else 3

    def test_voice(self):
        kept = self.write_voice()
        export = self.store.export(self.sid, low_priority=False)
        with open(os.path.join(export, "manifest.json")) as f:
            m = json.load(f)
        voice = m["takes"][0]["voice"]
        self.assertEqual(voice["file"], "takes/01-hand-size/voice.bin.zst.age")
        self.assertIn(voice["file"], m["encryption"]["files"])
        self.assertNotIn("voice", m["takes"][1])
        self.assertEqual(voice["chunks"], kept)
        self.assertEqual(voice["seconds"], kept / 10)
        r = self.validate.validate(export)
        self.assertEqual(r.errors, [])
        import subprocess
        with open(os.path.join(export, voice["file"]), "rb") as f:
            raw = subprocess.run([self.takes.find_zstd(), "-dc"], input=self.age.decrypt_bytes(f.read(), self.identity),
                                 capture_output=True, check=True).stdout
        out = os.path.join(self.tmp, "voice.bin")
        with open(out, "wb") as f:
            f.write(raw)
        self.assertEqual(len(raw), voice["raw_bytes"])
        rate, recs = voicelog.read(out)
        self.assertEqual((rate, len(recs)), (16000, kept))
        # not ticked as a debug session's, or encrypted
        files = sorted(os.path.relpath(os.path.join(r, n), export) for r, _, ns in os.walk(export) for n in ns)
        self.assertFalse(any(f.endswith("voice.bin.zst") for f in files))
        m["takes"][0]["voice"]["file"] = "takes/01-hand-size/voice.bin.zst"
        m["session"].pop("debug")
        self.takes.write_json(os.path.join(export, "manifest.json"), m)
        from test_validate import write_sums
        write_sums(export)
        r = self.validate.validate(export)
        self.assertTrue(any("voice recording" in e for e in r.errors), r.errors)

    def test_events(self):
        """The events parts as one file, those in the deleted range left out, only ever encrypted."""
        a, b = self.takes.read_json(os.path.join(self.tdir, "take.json"))["deleted"][0]
        for name, times in (("events.jsonl", (a - 1, (a + b) // 2)), ("events-2.jsonl", (b + 1,))):
            with open(os.path.join(self.tdir, name), "w") as f:
                for t in times:
                    f.write(json.dumps({"t": t, "src": "pointer", "kind": "click", "outcome": "done", "hand": "left",
                                        "panel": 0, "text": "pinch: clicked on x"}) + "\n")
        export = self.store.export(self.sid, low_priority=False)
        with open(os.path.join(export, "manifest.json")) as f:
            m = json.load(f)
        ev = m["takes"][0]["events"]
        self.assertEqual(ev["file"], "takes/01-hand-size/events.jsonl.zst.age")
        self.assertIn(ev["file"], m["encryption"]["files"])
        self.assertEqual((ev["events"], ev["by"]), (2, {"click": {"done": 2}}))
        self.assertEqual(self.validate.validate(export).errors, [])
        import subprocess
        with open(os.path.join(export, ev["file"]), "rb") as f:
            raw = subprocess.run([self.takes.find_zstd(), "-dc"], input=self.age.decrypt_bytes(f.read(), self.identity),
                                 capture_output=True, check=True).stdout
        self.assertEqual([json.loads(line)["t"] for line in raw.splitlines()], [a - 1, b + 1])
        # a public export has none
        sj = self.takes.read_json(self.session_json)
        del sj["debug"]
        sj["consent"] = {"kind": "public", "version": "2026-10-02"}
        self.takes.write_json(self.session_json, sj)
        profile = self.takes.read_json(os.path.join(self.base, "profile.json"))
        profile["consent"] = dict(profile["testing_consent"], version="2026-10-02")
        self.takes.write_json(os.path.join(self.base, "profile.json"), profile)
        export = self.store.export(self.sid, low_priority=False)
        self.assertFalse(any(n.startswith("events") for _, _, ns in os.walk(export) for n in ns))

    def test_bounty_round(self):
        """A bounty's round in the test phase (no debug): encrypted the same way, no live log."""
        sj = self.takes.read_json(self.session_json)
        del sj["debug"]
        sj["bounty"] = {"id": "index-taps", "title": "Index taps", "board_updated": "2026-10-14"}
        self.takes.write_json(self.session_json, sj)
        for name in ("live.bin", "live-2.bin"):
            os.remove(os.path.join(self.tdir, name))
        export = self.store.export(self.sid, low_priority=False)
        with open(os.path.join(export, "manifest.json")) as f:
            m = json.load(f)
        self.assertEqual(len(m["encryption"]["files"]), 2)
        r = self.validate.validate(export)
        self.assertEqual(r.errors, [])
        self.assertTrue(r.summary["testing"])
        self.assertFalse(r.summary["debug"])

    def test_public(self):
        """A full session (public consent): nothing encrypted, the window's profile keys left out."""
        sj = self.takes.read_json(self.session_json)
        del sj["debug"]
        sj["consent"] = {"kind": "public", "version": "2026-10-02"}
        self.takes.write_json(self.session_json, sj)
        profile = self.takes.read_json(os.path.join(self.base, "profile.json"))
        profile["consent"] = dict(profile["testing_consent"], version="2026-10-02")
        self.takes.write_json(os.path.join(self.base, "profile.json"), profile)
        export = self.store.export(self.sid, low_priority=False)
        with open(os.path.join(export, "manifest.json")) as f:
            m = json.load(f)
        self.assertNotIn("encryption", m)
        self.assertEqual(sorted(m["profile"]), ["consent", "contributor", "optional", "schema"])
        r = self.validate.validate(export)
        # the live log is a debug session's only
        self.assertTrue(any("live log" in e for e in r.errors), r.errors)

    def test_public_no_voice(self):
        """Voice is only ever exported encrypted: a public export leaves it out."""
        self.write_voice()
        sj = self.takes.read_json(self.session_json)
        del sj["debug"]
        sj["consent"] = {"kind": "public", "version": "2026-10-02"}
        self.takes.write_json(self.session_json, sj)
        profile = self.takes.read_json(os.path.join(self.base, "profile.json"))
        profile["consent"] = dict(profile["testing_consent"], version="2026-10-02")
        self.takes.write_json(os.path.join(self.base, "profile.json"), profile)
        export = self.store.export(self.sid, low_priority=False)
        files = [n for _, _, ns in os.walk(export) for n in ns]
        self.assertFalse(any(n.startswith("voice") for n in files), files)

    def test_checks(self):
        export = self.store.export(self.sid, low_priority=False)
        from test_validate import write_sums
        # an unencrypted recording in a debug export
        shutil.copyfile(os.path.join(export, "takes/02-no-hands/sets.bin.zst.age"),
                        os.path.join(export, "takes/02-no-hands/sets.bin.zst"))
        write_sums(export)
        r = self.validate.validate(export)
        self.assertTrue(any("unencrypted" in e for e in r.errors), r.errors)
        os.remove(os.path.join(export, "takes/02-no-hands/sets.bin.zst"))
        # not an age file (its insides can't be checked without a key: only the header and size)
        path = os.path.join(export, "takes/01-hand-size/live.bin.zst.age")
        with open(path, "r+b") as f:
            f.write(b"not-age!")
        write_sums(export)
        r = self.validate.validate(export)
        self.assertTrue(any("live.bin.zst.age" in e for e in r.errors), r.errors)
        # other keys
        with open(os.path.join(export, "manifest.json")) as f:
            m = json.load(f)
        m["encryption"]["recipients"] = [self.age.keygen()[1]]
        m["session"]["debug"]["use"] = "training"
        self.takes.write_json(os.path.join(export, "manifest.json"), m)
        write_sums(export)
        r = self.validate.validate(export)
        self.assertTrue(any("other keys" in e for e in r.errors), r.errors)
        self.assertTrue(any("testing only" in e for e in r.errors), r.errors)


if __name__ == "__main__":
    unittest.main()
