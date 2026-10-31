#!/usr/bin/python3
"""The recorders' JSON writers (recorder/common.write_json, recorder/bounties.keep_json,
hands/rec/takes.write_json) go through ft_store (DEE-300): the same bytes as before, a temp file
of its own (no fixed NAME.tmp two writers would share), and the old file kept when the write
fails.

  python3 recorder/tests/test_write_json.py
"""
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(HERE, "..", "..", "hands", "rec"))
from recorder import bounties, common  # noqa: E402
import takes  # noqa: E402

DATA = {"take": 3, "name": "café", "ranges": [[1, 2]]}


class WriteJson(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="ft-recorder-json-test.")
        self.path = os.path.join(self.dir, "meta.json")

    def read(self):
        with open(self.path, encoding="utf-8") as f:
            return f.read()

    def test_same_bytes_as_before(self):
        common.write_json(self.path, DATA)
        self.assertEqual(self.read(), '{\n "take": 3,\n "name": "caf\\u00e9",\n "ranges": [\n  [\n   1,\n   2\n'
                                      '  ]\n ]\n}\n')
        takes.write_json(self.path, DATA)
        self.assertEqual(self.read(), '{\n  "take": 3,\n  "name": "caf\\u00e9",\n  "ranges": [\n    [\n'
                                      '      1,\n      2\n    ]\n  ]\n}\n')
        bounties.keep_json(os.path.join(self.dir, "new", "board.json"), DATA)  # its folder is made
        with open(os.path.join(self.dir, "new", "board.json"), encoding="utf-8") as f:
            self.assertEqual(f.read(), '{\n "take": 3,\n "name": "café",\n "ranges": [\n  [\n   1,\n   2\n  ]\n ]\n}')

    def test_failed_write_keeps_the_old_file_and_leaves_no_temp(self):
        for write in (common.write_json, takes.write_json, bounties.keep_json):
            with self.subTest(write=write.__module__):
                common.write_json(self.path, {"old": 1})
                before = self.read()
                replace = os.replace
                os.replace = lambda *a: (_ for _ in ()).throw(OSError("disk gone"))
                try:
                    with self.assertRaises(OSError):
                        write(self.path, DATA)
                finally:
                    os.replace = replace
                self.assertEqual(self.read(), before)
                self.assertEqual(os.listdir(self.dir), ["meta.json"])


if __name__ == "__main__":
    unittest.main()
