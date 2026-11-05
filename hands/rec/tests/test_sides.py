#!/usr/bin/env python3
"""Tests for the side cameras' naming in recordings (sides.py) and its readers: takes.py's
review and export, validate.py, and session.py's live decision and recording parts.

  python3 -m hands.rec.tests.test_sides
"""
import json
import os
import shutil
import subprocess
import tempfile
import time
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
from hands.rec import session  # noqa: E402
from hands.rec import sides  # noqa: E402
from hands.rec import takes  # noqa: E402
import recorder.files  # noqa: E402  (Cancelled, read_json, human_bytes, free_bytes)
from hands.rec import validate  # noqa: E402
from hands.rec.tests.test_validate import fhset, make_session, SESSION  # noqa: E402


def names(record):
    ncams = sides.HDR.unpack_from(record, 0)[1]
    return [sides.CAM.unpack_from(record, sides.HDR.size + k * sides.CAM.size)[0].split(b"\0")[0].decode()
            for k in range(ncams)]


class RulesTest(unittest.TestCase):
    def test_rename_record(self):
        rec = fhset(10 ** 9, 3)
        out = sides.rename_record(rec)
        self.assertEqual(names(rec), ["slam_left", "slam_right"])
        self.assertEqual(names(out), ["slam_right", "slam_left"])
        self.assertEqual(len(out), len(rec))
        head = sides.HDR.size + 2 * sides.CAM.size
        self.assertEqual(out[head:], rec[head:])   # pixels untouched
        self.assertEqual(sides.rename_record(out), rec)

    def test_other_name(self):
        self.assertEqual(sides.other_name("slam_left_dk"), "slam_right_dk")
        self.assertEqual(sides.other_name("upper_left"), "upper_left")

    def test_needs_rename(self):
        self.assertTrue(sides.needs_rename(True, False))
        self.assertFalse(sides.needs_rename(True, True))
        self.assertTrue(sides.needs_rename(False, True))
        self.assertFalse(sides.needs_rename(None, True))   # unknown: leave the names

    def test_part_rename(self):
        sess = {"sides": {"swapped": True}}
        take = {"parts": {"sets.bin": {"names_swapped": False}, "sets-2.bin": {"names_swapped": True}}}
        self.assertTrue(sides.part_rename(sess, take, "sets.bin"))
        self.assertFalse(sides.part_rename(sess, take, "sets-2.bin"))
        self.assertTrue(sides.part_rename(sess, {}, "sets-3.bin"))   # no record: as ft-camd named them
        self.assertFalse(sides.part_rename({}, take, "sets.bin"))    # no decision

    def test_recording_runs(self):
        d = tempfile.mkdtemp()
        try:
            self.assertEqual(sides.recording_runs(d), [(0, False)])
            with open(os.path.join(d, "sides.json"), "w") as f:
                json.dump({"swapped": True, "decided_by": "auto", "names_swapped": [[0, False], [37, True]]}, f)
            runs = sides.recording_runs(d)
            self.assertEqual(runs, [(0, True), (37, False)])
            self.assertTrue(sides.rename_at(runs, 36))
            self.assertFalse(sides.rename_at(runs, 37))
        finally:
            shutil.rmtree(d)

    def test_read_live(self):
        d = tempfile.mkdtemp()
        try:
            path, ring = os.path.join(d, "sides.json"), os.path.join(d, "cam-ring")
            open(ring, "w").close()
            now = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
            live = {"pid": 1, "ring_ino": os.stat(ring).st_ino, "swapped": True, "decided_by": "auto",
                    "state": "decided", "updated_ns": now}
            with open(path, "w") as f:
                json.dump(live, f)
            self.assertEqual(sides.read_live(path, ring)["swapped"], True)
            self.assertIsNone(sides.read_live(path, ring, now_ns=now + 10 * 10 ** 9))   # stale
            os.remove(ring)
            open(ring, "w").close()   # a new ft-camd: a new ring file
            if os.stat(ring).st_ino != live["ring_ino"]:
                self.assertIsNone(sides.read_live(path, ring))
            self.assertIsNone(sides.read_live(os.path.join(d, "none.json")))
        finally:
            shutil.rmtree(d)

    def test_read_live_forced(self):
        """HANDS_SWAP_SIDES=0 forced and the hands disagree: the hands are the truth, whether
        ft-hands published them (built after 2026-10-06) or kept the forced value (before)."""
        d = tempfile.mkdtemp()
        try:
            path = os.path.join(d, "sides.json")

            def live(**kw):
                s = dict(pid=1, ring_ino=0, mode="0", names_swapped=False,
                         updated_ns=time.clock_gettime_ns(time.CLOCK_MONOTONIC), **kw)
                with open(path, "w") as f:
                    json.dump(s, f)
                return sides.read_live(path)

            self.assertEqual(live(state="forced", swapped=False, decided_by="config")["swapped"], False)
            self.assertEqual(live(state="forced, agrees", swapped=False, decided_by="config")["swapped"], False)
            old = live(state="forced, disagrees", swapped=False, decided_by="config")
            self.assertEqual((old["swapped"], old["decided_by"]), (True, "auto"))
            new = live(state="forced, disagrees", swapped=True, decided_by="auto")
            self.assertEqual((new["swapped"], new["decided_by"]), (True, "auto"))
        finally:
            shutil.rmtree(d)


class TakesTest(unittest.TestCase):
    """A swapped session: one take's parts recorded before the decision (ft-camd's names) and
    after it (named right)."""

    def setUp(self):
        if not takes.find_zstd():
            self.skipTest("no zstd")
        self.tmp = tempfile.mkdtemp(prefix="handrec-sides-test-")
        make_session(self.tmp)
        self.sdir = os.path.join(self.tmp, "sessions", SESSION)
        meta = recorder.files.read_json(os.path.join(self.sdir, "session.json"))
        meta["sides"] = {"swapped": True, "decided_by": "auto", "state": "confirmed"}
        takes.write_json(os.path.join(self.sdir, "session.json"), meta)
        self.tdir = os.path.join(self.sdir, "takes", "01-hand-size")
        with open(os.path.join(self.tdir, "sets-2.bin"), "wb") as f:   # after the decision: --sides 1
            for i in range(5):
                f.write(sides.rename_record(fhset(9 * 10 ** 12 + i * 10 ** 8, i)))
        tj = recorder.files.read_json(os.path.join(self.tdir, "take.json"))
        tj["parts"] = {"sets.bin": {"names_swapped": False}, "sets-2.bin": {"names_swapped": True}}
        takes.write_json(os.path.join(self.tdir, "take.json"), tj)
        self.store = takes.Store(self.tmp)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_review_reads_right_names(self):
        index = takes.take_index(self.tdir)
        self.assertEqual(index.rename, [True, False])
        self.assertEqual([c["name"] for c in index.cams], ["slam_right", "slam_left"])
        self.assertEqual([c["name"] for c in index.read_set(0)], ["slam_right", "slam_left"])
        self.assertEqual([c["name"] for c in index.read_set(len(index) - 1)], ["slam_right", "slam_left"])
        # the same pixels under the other name
        raw = takes.TakeIndex(self.tdir).read_set(0, only="slam_right")[0]["pixels"]
        self.assertEqual(raw, fhset(10 ** 12, 0)[sides.HDR.size + 2 * sides.CAM.size:][:32 * 24])
        self.assertEqual(index.renamed_sets(), 30)
        # the other take has no parts record: recorded as ft-camd named them, renamed too
        other = takes.take_index(os.path.join(self.sdir, "takes", "02-no-hands"))
        self.assertEqual(other.rename, [True])

    def test_export_writes_right_names(self):
        path = self.store.export(SESSION, low_priority=False)
        r = validate.validate(path)
        self.assertEqual(r.errors, [])
        self.assertEqual(r.warnings, [])
        m = recorder.files.read_json(os.path.join(path, "manifest.json"))
        t = m["takes"][0]
        self.assertEqual(t["sides"], {"names_swapped": True, "renamed_sets": 30})
        self.assertEqual(m["session"]["sides"]["swapped"], True)
        self.assertEqual(recorder.files.read_json(os.path.join(path, "takes", t["id"], "take.json"))["parts"],
                         {"sets.bin": {"names_swapped": True}})
        # every exported set: right names (the rule applied again changes nothing)
        data = subprocess.run([takes.find_zstd(), "-dc", os.path.join(path, t["file"])],
                              capture_output=True, check=True).stdout
        off, seen = 0, 0
        while off < len(data):
            nbytes = sides.HDR.unpack_from(data, off)[2]
            self.assertEqual(names(data[off:off + nbytes]), ["slam_right", "slam_left"])
            off, seen = off + nbytes, seen + 1
        self.assertEqual(seen, 35)

    def test_validate_catches_wrong_names(self):
        path = self.store.export(SESSION, low_priority=False)
        mpath = os.path.join(path, "manifest.json")
        m = recorder.files.read_json(mpath)
        m["takes"][0]["sides"]["names_swapped"] = False
        takes.write_json(mpath, m)
        report = validate.Report(path)
        validate.check_manifest(m, report, path)
        self.assertTrue(any("aren't named right" in e for e in report.errors), report.errors)

    def test_undecided_warns(self):
        meta = recorder.files.read_json(os.path.join(self.sdir, "session.json"))
        meta["sides"] = {"swapped": None}
        takes.write_json(os.path.join(self.sdir, "session.json"), meta)
        path = self.store.export(SESSION, low_priority=False)
        r = validate.validate(path)
        self.assertEqual(r.errors, [])
        self.assertTrue(any("wasn't decided" in w for w in r.warnings), r.warnings)
        m = recorder.files.read_json(os.path.join(path, "manifest.json"))
        self.assertIsNone(m["takes"][0]["sides"]["names_swapped"])   # the parts differ
        self.assertEqual(m["takes"][0]["sides"]["renamed_sets"], 0)

    def test_sides_command(self):
        out = self.store.sides(SESSION)
        self.assertTrue(out["sides"]["swapped"])
        self.assertEqual(out["takes"]["01-hand-size"]["sets-2.bin"], {"names_swapped": True, "renamed_when_read": False})
        out = self.store.sides(SESSION, False)
        self.assertEqual(out["sides"]["decided_by"], "manual")
        self.assertTrue(out["sides"]["replaced"]["swapped"])
        self.assertTrue(out["takes"]["01-hand-size"]["sets-2.bin"]["renamed_when_read"])
        self.assertEqual(takes.take_index(self.tdir).rename, [False, True])   # the cache saw session.json change


class SessionSidesTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="handrec-sides-session-")
        self.script = os.path.join(self.tmp, "script.json")
        with open(self.script, "w") as f:
            json.dump({"version": 1, "sections": [{"id": "a", "title": "A", "prompts": [{"text": "x", "seconds": 1}]}]},
                      f)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def live(self, **kw):
        d = dict(pid=1, ring_ino=0, mode="auto", state="decided", swapped=True, decided_by="auto",
                 evidence={"as_named": 0, "swapped": 10}, updated_ns=time.clock_gettime_ns(time.CLOCK_MONOTONIC))
        d.update(kw)
        with open(os.path.join(self.tmp, "sides.json"), "w") as f:
            json.dump(d, f)

    def test_read_sides(self):
        s = session.Session(os.path.join(self.tmp, "base"), {}, {}, "room", self.script, dry_run=True,
                            hands_dir=self.tmp)
        s.session_dir = self.tmp
        s._session_json = {"sides": {"swapped": None}}
        s._read_sides(force=True)
        self.assertIsNone(s._sides_swapped())   # no file
        self.live(swapped=None, state="deciding", decided_by=None)
        s._read_sides(force=True)
        self.assertIsNone(s._sides_swapped())
        self.live()
        s._read_sides(force=True)
        self.assertTrue(s._sides_swapped())
        self.assertEqual(s._session_json["sides"]["evidence"]["swapped"], 10)
        self.live(state="confirmed")
        s._read_sides(force=True)
        self.assertEqual(s._session_json["sides"]["state"], "confirmed")
        self.live(swapped=False, state="decided, reversed")
        s._read_sides(force=True)
        self.assertFalse(s._sides_swapped())
        self.assertTrue(s._session_json["sides"]["reversed_from"]["swapped"])
        self.assertFalse(recorder.files.read_json(os.path.join(self.tmp, "session.json"))["sides"]["swapped"])

    def test_read_sides_forced(self):
        """PR #4 on the dataset: HANDS_SWAP_SIDES=0 from the old example config, and the hands
        disagree. The session takes the hands' answer, not the forced one."""
        s = session.Session(os.path.join(self.tmp, "base"), {}, {}, "room", self.script, dry_run=True,
                            hands_dir=self.tmp)
        s.session_dir = self.tmp
        s._session_json = {"sides": {"swapped": None}}
        self.live(mode="0", state="forced", swapped=False, decided_by="config", names_swapped=False, evidence=None)
        s._read_sides(force=True)
        self.assertIs(s._sides_swapped(), False)
        self.live(mode="0", state="forced, disagrees", swapped=False, decided_by="config", names_swapped=False)
        s._read_sides(force=True)
        self.assertIs(s._sides_swapped(), True)
        self.assertEqual(s._session_json["sides"]["decided_by"], "auto")
        self.assertEqual(s._session_json["sides"]["reversed_from"]["decided_by"], "config")

    def test_recorder_parts(self):
        calls = []

        class FakeProc:
            returncode = 0

            def __init__(self, argv, **kw):
                calls.append(argv)

            def poll(self):
                return 0

        take = os.path.join(self.tmp, "take")
        os.makedirs(take)
        with mock.patch.object(session.subprocess, "Popen", FakeProc):
            r1 = session.Recorder(take, 1, 10, None, None)
            r2 = session.Recorder(take, 2, 10, None, None, swap=True)
            r3 = session.Recorder(take, 3, 10, None, None, swap=False)
        argv = [c[c.index("--sides") + 1] for c in calls]
        self.assertEqual(argv, ["auto", "1", "0"])
        # ft-hands' DIR/sides.json goes into names_swapped and away
        os.makedirs(r2.dir, exist_ok=True)
        with open(os.path.join(r2.dir, "sets.bin"), "wb") as f:
            f.write(fhset(10 ** 9, 0))
        with open(os.path.join(r2.dir, "sides.json"), "w") as f:
            json.dump({"swapped": None, "names_swapped": [[0, True]]}, f)
        r2.stop()
        self.assertTrue(r2.names_swapped)
        self.assertEqual(r2.file, "sets-2.bin")
        self.assertTrue(os.path.isfile(os.path.join(take, "sets-2.bin")))
        self.assertFalse(os.path.exists(r2.dir))
        with open(os.path.join(take, "sides.json"), "w") as f:
            json.dump({"swapped": None, "names_swapped": [[0, False]]}, f)
        r1.stop()
        self.assertFalse(r1.names_swapped)
        self.assertFalse(os.path.exists(os.path.join(take, "sides.json")))
        self.assertFalse(r3.names_swapped)


if __name__ == "__main__":
    unittest.main()
