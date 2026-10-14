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
