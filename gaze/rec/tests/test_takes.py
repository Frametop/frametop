#!/usr/bin/env python3
"""The eye recorder's store and encrypted export (gaze/rec/takes.py), on a made-up round of
random frames (never real eye images): listing, deleted ranges across the two clocks, the export's
chunks, logs and checksums, and unpacking it with a key. Needs zstd (Python 3.14's, as eyerec.py)
and the cryptography library: the dev container has both.

  python3 gaze/rec/tests/test_takes.py
"""
import json
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import eyerec  # noqa: E402
import takes  # noqa: E402
from recorder import age  # noqa: E402

try:
    age._crypto()
    HAVE = eyerec.compress is not None
except RuntimeError:
    HAVE = False

RID = "20261014-120000-edges"
TID = "01-dots"
MONO0 = 1000 * 10**9          # the take starts at 1000 s MONOTONIC
OFF = 5 * 10**9 + 123         # RAW is 5 s ahead
FPS = 90.0
PER_CHUNK = 10
CHUNKS = 3


def raw_s(mono_ns):
    return (mono_ns + OFF) / 1e9


def make_round(base, health=True):
    """A round with one take: CHUNKS chunks of PER_CHUNK random frames per camera, frame k of
    camera c at MONO0 + k / FPS (plus 1 ms for camera 1). Returns {(c, k): frame bytes}."""
    rdir = os.path.join(base, "rounds", RID)
    tdir = os.path.join(rdir, "takes", TID)
    frames = {}
    lines = [eyerec.INDEX_HEADER]
    for c in eyerec.CAMERAS:
        os.makedirs(os.path.join(tdir, "cam%d" % c))
        for chunk in range(CHUNKS):
            data = []
            for i in range(PER_CHUNK):
                k = chunk * PER_CHUNK + i
                frames[(c, k)] = os.urandom(eyerec.FRAME)
                data.append(frames[(c, k)])
                lines.append("%d\t%d\t%d\t%d\t%.6f\n" % (c, chunk, i, k % 8, raw_s(MONO0 + int(k / FPS * 1e9)) + c * 0.001))
            with open(os.path.join(tdir, "cam%d" % c, "%06d.zst" % chunk), "wb") as f:
                f.write(eyerec.compress(b"".join(data)))
    with open(os.path.join(tdir, "index.tsv"), "w") as f:
        f.writelines(lines)
    end = MONO0 + int(CHUNKS * PER_CHUNK / FPS * 1e9)
    with open(os.path.join(tdir, "take.json"), "w") as f:
        json.dump({"group": "dots", "started_ns": MONO0, "ended_ns": end, "status": "complete", "steps": [],
                   "deleted": [], "clock": [[MONO0, OFF], [end, OFF]], "notes": ""}, f)
    with open(os.path.join(tdir, "own.jsonl"), "w") as f:
        for k in range(CHUNKS * PER_CHUNK):
            f.write(json.dumps({"t": raw_s(MONO0 + int(k / FPS * 1e9)), "cam": 0, "x": 1.0}) + "\n")
    with open(os.path.join(tdir, "prompts.jsonl"), "w") as f:
        f.write(json.dumps({"t": MONO0, "event": "take"}) + "\n")
    with open(os.path.join(tdir, "targets.jsonl"), "w") as f:
        f.write(json.dumps({"t": raw_s(MONO0), "dot": "on", "yaw": 0, "pitch": 0}) + "\n")
    with open(os.path.join(rdir, "session.json"), "w") as f:
        json.dump({"schema": 1, "round": RID, "bounty": "edges", "status": "done", "takes": [TID],
                   "started": "2026-10-14T12:00:00+0000", "checklist": {"glasses": "none"},
                   "uploads": [{"pr": 1}]}, f)
    with open(os.path.join(rdir, "fit.json"), "w") as f:
        json.dump({"fitment": {"state": "ok"}}, f)
    with open(os.path.join(rdir, "session.log"), "w") as f:
        f.write("a log that names /home/someone\n")
    if health:
        with open(os.path.join(rdir, "health.json"), "w") as f:
            json.dump({"conditions": ["test"]}, f)
    with open(os.path.join(base, "profile.json"), "w") as f:
        json.dump({"contributor": "c0ffee", "consent": {"version": "test-1"}, "health": {"x": 1},
                   "optional": {"notes": "private"}}, f)
    return frames


def load(path, mode="r"):
    with open(path, mode) as f:
        return json.load(f) if path.endswith(".json") else f.read()


def frame_mono(k):
    return MONO0 + int(k / FPS * 1e9)


@unittest.skipUnless(HAVE, "needs zstd and the cryptography library (the dev container)")
class TakesTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="eyerec-takes-")
        self.base = os.path.join(self.tmp, "contrib")
        self.frames = make_round(self.base)
        self.store = takes.Store(self.base)
        self.ident, self.rec = age.keygen()
        self.key = os.path.join(self.tmp, "key.txt")
        with open(self.key, "w") as f:
            f.write("# test key\n" + self.ident + "\n")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_ids(self):
        for bad in ("../x", "20261014-120000-../x", "", "x"):
            with self.assertRaises(ValueError):
                self.store.round_dir(bad)
        for bad in ("..", "01-a/b", ".x"):
            with self.assertRaises(ValueError):
                self.store.take_dir(RID, bad)
        self.store.round_dir("20261014-120000-edges-2")

    def test_clock(self):
        clock = [[0, 100], [1000, 200]]
        self.assertEqual(takes.raw_offset_ns(-5, clock), 100)
        self.assertEqual(takes.raw_offset_ns(500, clock), 150)
        self.assertEqual(takes.raw_offset_ns(5000, clock), 200)
        self.assertEqual(takes.raw_offset_ns(5, []), 0)

    def test_listing(self):
        [r] = self.store.rounds()
        self.assertEqual((r["id"], r["bounty"], r["takes"], r["exported"]), (RID, "edges", 1, False))
        [t] = self.store.takes(RID)
        self.assertEqual(t["frames"], [30, 30])
        self.assertEqual(t["deleted_frames"], [0, 0])
        self.assertAlmostEqual(t["duration_s"], 29 / FPS, places=2)

    def test_ranges(self):
        # index.tsv's times are whole microseconds: the ranges have a millisecond to spare
        self.store.delete_range(RID, TID, frame_mono(12) - 10**6, frame_mono(25))
        self.store.delete_range(RID, TID, frame_mono(20), frame_mono(29) + 10**6)
        self.assertEqual(self.store.ranges(RID, TID), [[frame_mono(12) - 10**6, frame_mono(29) + 10**6]])
        [t] = self.store.takes(RID)
        self.assertEqual(t["deleted_frames"][0], 18)
        self.store.restore_range(RID, TID, 0)
        self.assertEqual(self.store.ranges(RID, TID), [])

    def test_export_and_unpack(self):
        # frames 15..29 deleted: chunk 0 whole, chunk 1 half (10..14 kept), chunk 2 gone
        self.store.delete_range(RID, TID, frame_mono(15) - 1000, frame_mono(29) + 5 * 10**6)
        path = self.store.export(RID, recipients=[self.rec])
        self.assertFalse(os.path.exists(path + ".partial"))
        m = load(os.path.join(path, "manifest.json"))
        [e] = m["takes"]
        self.assertEqual(e["frames_deleted"][0], 15)
        self.assertEqual(e["frames"][0], 15)
        self.assertEqual(e["chunks"], 4)
        self.assertEqual(e["logs"]["own.jsonl"], {"rows": 15, "deleted": 15})
        self.assertNotIn("uploads", m["session"])
        self.assertNotIn("health", m["profile"])
        self.assertNotIn("notes", m["profile"]["optional"])
        self.assertEqual(m["encryption"]["recipients"], [self.rec])
        files = sorted(os.path.relpath(os.path.join(r, n), path) for r, _, ns in os.walk(path) for n in ns)
        self.assertIn("health.json.age", files)
        self.assertNotIn("health.json", files)
        self.assertNotIn("session.log", files)
        self.assertFalse([f for f in files if f.endswith(".zst")], "every chunk is encrypted")
        self.assertIn("takes/%s/cam0/000001.zst.age" % TID, files)
        self.assertNotIn("takes/%s/cam0/000002.zst.age" % TID, files)
        # chunk 0 untouched: its encrypted copy decrypts to the recorded file
        src = load(os.path.join(self.base, "rounds", RID, "takes", TID, "cam0", "000000.zst"), "rb")
        enc = load(os.path.join(path, "takes", TID, "cam0", "000000.zst.age"), "rb")
        self.assertEqual(age.decrypt_bytes(enc, self.ident), src)
        with open(os.path.join(path, "SHA256SUMS")) as f:
            self.assertEqual(sorted(line.split("  ", 1)[1].strip() for line in f), [f for f in files if f != "SHA256SUMS"])

        out = takes.unpack(path, self.key, os.path.join(self.tmp, "unpacked"))
        self.assertEqual(load(os.path.join(out, "health.json")), {"conditions": ["test"]})
        got = {}
        tdir = os.path.join(out, "takes", TID)
        for c, t, slot, data in eyerec.iter_frames(tdir):
            got.setdefault(c, []).append(data)
        for c in eyerec.CAMERAS:
            self.assertEqual(got[c], [self.frames[(c, k)] for k in range(15)])
        self.assertEqual(load(os.path.join(tdir, "take.json"))["export"]["deleted_raw_s"][0][0],
                         raw_s(frame_mono(15) - 1000))

        [r] = self.store.rounds()
        self.assertTrue(r["exported"] and not r["export_stale"])

    def test_default_recipients(self):
        path = self.store.export(RID)
        m = load(os.path.join(path, "manifest.json"))
        self.assertEqual(m["encryption"]["recipients"], age.read_recipients(takes.RECIPIENTS))
        with self.assertRaises(ValueError):   # the test key isn't one of them
            takes.unpack(path, self.key, os.path.join(self.tmp, "no"))

    def test_bad_chunks(self):
        tdir = os.path.join(self.base, "rounds", RID, "takes", TID)
        shutil.copyfile(os.path.join(tdir, "cam0", "000000.zst"), os.path.join(tdir, "cam0", "000007.zst"))
        with open(os.path.join(tdir, "cam1", "000001.zst"), "r+b") as f:
            f.truncate(100)
        os.remove(os.path.join(tdir, "cam1", "000002.zst"))
        path = self.store.export(RID, recipients=[self.rec])
        [e] = load(os.path.join(path, "manifest.json"))["takes"]
        self.assertEqual(e["left_out"], {"not_indexed": 1, "missing": 1, "bad": 1})
        self.assertEqual(e["frames"], [30, 10])
        with open(os.path.join(path, "takes", TID, "index.tsv")) as f:
            self.assertEqual(sum(1 for line in f if line.startswith("1\t")), 10)

    def test_cancel(self):
        with self.assertRaises(takes.Cancelled):
            self.store.export(RID, recipients=[self.rec], cancel=lambda: True)
        self.assertFalse(os.path.exists(self.store.export_dir(RID) + ".partial"))
        self.assertFalse(os.path.exists(self.store.export_dir(RID)))

    def test_delete(self):
        self.store.export(RID, recipients=[self.rec])
        self.store.delete_take(RID, TID)
        meta = load(os.path.join(self.base, "rounds", RID, "session.json"))
        self.assertEqual((meta["takes"], meta["deleted_takes"]), ([], [TID]))
        self.store.delete_round(RID)
        self.assertEqual(self.store.rounds(), [])
        self.assertFalse(os.path.exists(self.store.export_dir(RID)))


if __name__ == "__main__":
    unittest.main()
