#!/usr/bin/env python3
"""The eye recorder's export check (gaze/rec/validate.py) on takes.py's export of a made-up
round of random frames (test_takes.make_round): a good one, with and without a key, and broken
ones. Needs zstd and the cryptography library: the dev container has both.

  python3 gaze/rec/tests/test_validate.py
"""
import json
import os
import shutil
import sys
import tempfile
import unittest
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
import takes  # noqa: E402
import validate  # noqa: E402
from recorder import age  # noqa: E402
from recorder.files import write_sums  # noqa: E402
from test_takes import HAVE, RID, TID, make_round  # noqa: E402


@unittest.skipUnless(HAVE, "needs zstd and the cryptography library (the dev container)")
class ValidateTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="eyerec-validate-")
        base = os.path.join(self.tmp, "contrib")
        make_round(base)
        self.contributor = str(uuid.uuid4())
        with open(os.path.join(base, "profile.json"), "w") as f:
            json.dump({"contributor": self.contributor,
                       "consent": {"version": "test-1", "adult": True, "region_ok": True, "accepted": "2026-10-14"},
                       "bounties": {}}, f)
        self.ident, self.rec = age.keygen()
        self.keys = os.path.join(self.tmp, "recipients.txt")
        with open(self.keys, "w") as f:
            f.write("# test\n%s\n" % self.rec)
        self.store = takes.Store(base)
        self.path = self.store.export(RID, recipients=[self.rec])
        self.take = os.path.join(self.path, "takes", TID)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def check(self, identity=None, resum=False):
        if resum:   # a change made on purpose, with checksums to match, so the other checks see it
            os.remove(os.path.join(self.path, "SHA256SUMS"))
            write_sums(self.path)
        return validate.validate(self.path, identity=identity, recipients_path=self.keys)

    def has(self, report, text):
        self.assertTrue(any(text in e for e in report.errors), (text, report.errors))

    def manifest(self, change):
        p = os.path.join(self.path, "manifest.json")
        with open(p) as f:
            m = json.load(f)
        change(m)
        with open(p, "w") as f:
            json.dump(m, f)

    def test_good(self):
        r = self.check()
        self.assertEqual(r.errors, [])
        self.assertEqual(r.summary["frames"], 60)
        self.assertTrue(r.summary["health"])
        r = self.check(identity=self.ident)
        self.assertEqual(r.errors, [])

    def test_shipped_keys(self):
        r = validate.validate(self.path)   # recipients.txt's, not the test key
        self.has(r, "other keys than the dataset's")

    def test_checksum(self):
        with open(os.path.join(self.take, "cam0", "000001.zst.age"), "r+b") as f:
            f.seek(200)
            b = f.read(1)
            f.seek(200)
            f.write(bytes([b[0] ^ 1]))
        self.has(self.check(), "checksum mismatch")

    def test_unencrypted(self):
        shutil.copyfile(os.path.join(self.take, "index.tsv"), os.path.join(self.take, "cam0", "000009.zst"))
        with open(os.path.join(self.path, "health.json"), "w") as f:
            f.write("{}")
        r = self.check(resum=True)
        self.has(r, "cam0/000009.zst: not an allowed file (unencrypted")
        self.has(r, "health.json: not an allowed file (unencrypted")

    def test_cut_short(self):
        # Without a key only some sizes show a cut (a last piece under a tag); the key finds any.
        p = os.path.join(self.take, "cam1", "000000.zst.age")
        with open(p, "rb") as f:
            data = f.read()
        body = data.index(b"\n", data.index(b"\n---") + 1) + 1 + 16
        whole = (len(data) - body) // (age.CHUNK + age.TAG)
        with open(p, "r+b") as f:
            f.truncate(body + whole * (age.CHUNK + age.TAG) + 5)
        self.has(self.check(resum=True), "cam1/000000.zst.age: a size no age file has")
        with open(p, "r+b") as f:
            f.truncate(len(data) - 65000)
        self.assertEqual(self.check(resum=True).errors, [])
        self.has(self.check(identity=self.ident, resum=True), "cam1/000000.zst.age: doesn't decrypt")

    def test_wrong_content(self):
        with open(os.path.join(self.take, "cam0", "000000.zst.age"), "wb") as f:
            f.write(age.encrypt_bytes(os.urandom(1000), [self.rec]))
        self.assertEqual(self.check(resum=True).errors, [], "without a key it can't tell")
        self.has(self.check(identity=self.ident), "doesn't decompress")
        other, _ = age.keygen()
        self.has(self.check(identity=other), "doesn't decrypt")

    def test_index(self):
        p = os.path.join(self.take, "index.tsv")
        with open(p) as f:
            lines = f.readlines()
        with open(p, "w") as f:
            f.writelines(lines[:3] + lines[4:])   # camera 0, chunk 0, frame 2 gone
        r = self.check(resum=True)
        self.has(r, "frames aren't numbered")
        self.has(r, "the manifest says [30, 30] frames, index.tsv has [29, 30]")
        os.remove(os.path.join(self.take, "cam1", "000001.zst.age"))
        self.has(self.check(resum=True), "cam1/000001.zst.age: in index.tsv but missing")

    def test_manifest(self):
        def change(m):
            m["session"]["dry_run"] = True
            m["session"]["replay"] = "/dev/shm/x"
            m["profile"]["health"] = {"x": 1}
            m["profile"]["consent"]["adult"] = False
            m["contributor"] = m["profile"]["contributor"] = "c0ffee"
        self.manifest(change)
        r = self.check(resum=True)
        for text in ("dry-run round", "from a replay", "unexpected keys: health", "18 or older", "random uuid4"):
            self.has(r, text)

    def test_two_recipients(self):
        _, second = age.keygen()
        with open(self.keys, "a") as f:
            f.write(second + "\n")
        self.has(self.check(), "other keys than the dataset's")
        self.manifest(lambda m: m["encryption"].update(recipients=[self.rec, second]))
        self.has(self.check(resum=True), "encrypted to 1 recipients, not 2")


if __name__ == "__main__":
    unittest.main()
