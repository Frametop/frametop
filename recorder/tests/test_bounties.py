#!/usr/bin/env python3
"""The bounty board (recorder/bounties.py): a fetched board can only describe, reorder,
reprioritise or retire bounties, and the view says which can be done.

  python3 recorder/tests/test_bounties.py
"""
import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
from recorder import bounties  # noqa: E402


def board(*items, updated="2026-10-14"):
    return {"schema": 1, "updated": updated, "bounties": list(items)}


class ValidateTest(unittest.TestCase):
    def test_not_a_board(self):
        for b in (None, [], {"schema": 2, "updated": "2026-10-14", "bounties": []}, {"schema": 1, "bounties": []},
                  {"schema": 1, "updated": "soon", "bounties": []}, {"schema": 1, "updated": "2026-10-14"}):
            with self.assertRaises(ValueError):
                bounties.validate(b)

    def test_cleans_and_drops(self):
        b = bounties.validate(board(
            {"id": "edges", "title": "  View\nedges " + "x" * 100, "blurb": "a\tb", "minutes": 5, "priority": "high",
             "script": "edges", "needs": {"glasses": True, "makeup": True}, "run": "rm -rf /"},
            {"id": "edges", "title": "again"},                 # the same id twice: the first wins
            {"id": "../etc", "title": "bad id"},
            {"id": "ok", "script": "../../bin/sh"},            # a script name that's a path
            {"id": "plain", "minutes": 600, "priority": "urgent"},
            "not a bounty"))
        ids = [x["id"] for x in b["bounties"]]
        self.assertEqual(ids, ["edges", "plain"])
        e, p = b["bounties"]
        self.assertTrue(e["title"].startswith("View edges x"))
        self.assertEqual(len(e["title"]), 60)
        self.assertEqual(e["blurb"], "a b")
        self.assertEqual(e["needs"], ["glasses"], "needs it doesn't know are dropped")
        self.assertNotIn("run", e, "only known keys")
        self.assertEqual((p["script"], p["minutes"], p["priority"], p["title"]), ("plain", None, "normal", "plain"))

    def test_limits(self):
        many = bounties.validate(board(*({"id": "b%d" % i} for i in range(150))))
        self.assertEqual(len(many["bounties"]), bounties.MAX_BOUNTIES)


class ViewTest(unittest.TestCase):
    def setUp(self):
        self.board = bounties.validate(board(
            {"id": "low", "priority": "low", "script": "low"},
            {"id": "high-done", "priority": "high", "script": "high-done"},
            {"id": "high", "priority": "high", "script": "high"},
            {"id": "gone", "priority": "retired", "script": "gone"},
            {"id": "new", "priority": "high", "script": "future"},
            {"id": "specs", "priority": "high", "script": "specs", "needs": {"glasses": True}}))
        self.have = lambda s: s != "future"

    def test_order_and_reasons(self):
        profile = {"bounties": {"high-done": {"rounds": 2, "last": "r1"}}}
        v = bounties.view(self.board, profile, {"glasses": "none"}, self.have)
        self.assertEqual([b["id"] for b in v], ["high", "high-done", "low", "new", "specs"])
        by = {b["id"]: b for b in v}
        self.assertEqual((by["high-done"]["done"], by["high-done"]["last"]), (2, "r1"))
        self.assertEqual(by["new"]["available"], "Needs a newer recorder")
        self.assertEqual(by["specs"]["available"], "For people who wear glasses or inserts")
        self.assertNotIn("gone", by)

    def test_glasses_meet_the_need(self):
        for answer, ok in (("glasses", True), ("inserts", True), ("contacts", False), (None, False)):
            v = {b["id"]: b for b in bounties.view(self.board, {}, {"glasses": answer}, self.have)}
            self.assertEqual(v["specs"]["available"] == "", ok, answer)

    def test_mark_done(self):
        p = {}
        bounties.mark_done(p, "high", "r1")
        bounties.mark_done(p, "high", "r2")
        self.assertEqual(p["bounties"]["high"], {"rounds": 2, "last": "r2"})
        p = {"bounties": {"high": "nonsense"}}
        self.assertEqual(bounties.mark_done(p, "high", "r3")["bounties"]["high"], {"rounds": 1, "last": "r3"})


class FilesTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="bounties-test-")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def write(self, name, data):
        path = os.path.join(self.tmp, name)
        with open(path, "w") as f:
            f.write(data if isinstance(data, str) else json.dumps(data))
        return path

    def test_newest(self):
        a = bounties.validate(board({"id": "a"}, updated="2026-10-14"))
        b = bounties.validate(board({"id": "b"}, updated="2026-10-20"))
        self.assertIs(bounties.newest(a, b), b)
        self.assertIs(bounties.newest(b, a), b)
        same = bounties.validate(board({"id": "c"}, updated="2026-10-14"))
        self.assertIs(bounties.newest(a, same), same, "a tie: the fetched one")
        self.assertIs(bounties.newest(None, a), a)
        self.assertIs(bounties.newest(a, None), a)

    def test_load_and_fetch(self):
        self.assertIsNone(bounties.load(self.write("bad.json", "{")))
        good = self.write("board.json", board({"id": "edges", "priority": "high"}))
        self.assertEqual(bounties.load(good)["bounties"][0]["id"], "edges")
        cache = os.path.join(self.tmp, "cache", "board.json")
        got = bounties.fetch("file://" + good, cache)
        self.assertEqual(got, bounties.load(cache), "the cache keeps the cleaned board")
        big = self.write("big.json", " " * (bounties.MAX_BYTES + 10) + json.dumps(board()))
        with self.assertRaises(ValueError):
            bounties.fetch("file://" + big, cache)
        with self.assertRaises(ValueError):
            bounties.fetch("file://" + self.write("nb.json", {"schema": 9}), cache)
        self.assertEqual(bounties.load(cache), got, "a failed fetch leaves the cache")

    def test_shipped_board(self):
        """The eye recorder's board: valid, and every script it names is a round it ships."""
        rec = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "gaze", "rec")
        b = bounties.load(os.path.join(rec, "bounties.json"))
        self.assertIsNotNone(b)
        self.assertEqual(len(b["bounties"]), 10)
        for x in b["bounties"]:
            self.assertTrue(os.path.exists(os.path.join(rec, "rounds", x["script"] + ".json")), x["script"])


if __name__ == "__main__":
    unittest.main()
