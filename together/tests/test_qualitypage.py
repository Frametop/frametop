#!/usr/bin/env python3
"""How good the video of your screens may get, on Frametop Settings' Together page
(roompage.quality, ft_together_quality.py, YourShares.qml), offline: ft-together's `quality`
read into the box and what it says, and the backend and the page against a stand-in that
answers as ft-together does (pagekit.py). The backend's part needs PySide6, the page's Kirigami
too.

    python3 together/tests/test_qualitypage.py
"""
import re
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1] / "conf"))
import ft_conf  # noqa: E402
import roompage  # noqa: E402
from pagekit import ADA, BEN, PageCase  # noqa: E402


class Rules(unittest.TestCase):
    def test_what_the_box_says(self):
        self.assertEqual([name for name, _title, _text in roompage.QUALITIES], ["low", "balanced", "high"])
        for index, (name, _title, text) in enumerate(roompage.QUALITIES):
            self.assertEqual(roompage.quality(f"ok {name}"), {"can": True, "index": index, "text": text})

    def test_the_numbers_are_the_programs(self):
        """What the page says each gets a screen is what together/flow/quality.h gives it."""
        header = (HERE.parent / "flow" / "quality.h").read_text()
        row = re.search(r'kLow\{"low", ([\d\']+), (\d+)\}, kBalanced\{"balanced", ([\d\']+), (\d+)\}, kHigh\{"high", ([\d\']+), (\d+)\}', header)
        self.assertIsNotNone(row)
        numbers = [int(n.replace("'", "")) for n in row.groups()]
        for (_name, _title, text), bits, pictures in zip(roompage.QUALITIES, numbers[0::2], numbers[1::2]):
            self.assertIn(f"up to {bits // 1000000} Mbit/s and {pictures} pictures a second", text)

    def test_an_answer_that_isnt_one(self):
        for other in (None, "", "ok", "ok ", "ok best", "ok Low", "low", "no low", "errhigh", "error quality low|balanced|high", "error what", {"quality": "low"}):
            self.assertEqual(roompage.quality(other), {"can": False, "index": 1, "text": "Frametop Together doesn't say"}, other)


class Page(PageCase):
    def room(self):
        self.install()
        self.together.mode, self.together.people = "hosting", [ADA, BEN]
        self.assertTrue(self.fresh(lambda: self.b.quality.can))
        return self.b.quality

    def kept(self):
        return ft_conf.read(str(self.conf), warn=lambda *_: None).get("TOGETHER_QUALITY")

    def test_picked(self):
        q = self.room()
        self.assertEqual((q.index, q.names, q.text), (1, ["Low", "Balanced", "High"], roompage.QUALITIES[1][2]))
        self.assertIsNone(self.kept(), "nothing is written until its person picks")
        q.setIndex(2)
        self.assertTrue(self.fresh(lambda: q.index == 2))
        self.assertEqual((q.text, self.kept()), (roompage.QUALITIES[2][2], "high"))
        q.setIndex(0)
        self.assertTrue(self.fresh(lambda: q.index == 0))
        self.assertEqual((self.together.asked, self.kept(), self.said), (["quality high", "quality low"], "low", []))
        for none in (-1, 3):
            q.setIndex(none)
        q.setIndex(1)
        self.assertTrue(self.fresh(lambda: q.index == 1))
        self.assertEqual(self.together.asked, ["quality high", "quality low", "quality balanced"], "only one of the three is asked for")

    def test_a_no_is_told_and_nothing_kept(self):
        q = self.room()
        self.together.quality = None       # an ft-together that takes none
        q.setIndex(2)
        self.assertTrue(self.until(lambda: self.said == [("What", True)]))
        self.assertIsNone(self.kept())
        self.assertTrue(self.fresh(lambda: not q.can))
        self.assertEqual((q.index, q.text), (1, "Frametop Together doesn't say"))

    def test_a_file_that_cant_be_written(self):
        q = self.room()
        self.conf.mkdir()                  # (a folder where the file should be)
        q.setIndex(2)
        self.assertTrue(self.until(lambda: len(self.said) == 1 and self.said[0][0].startswith("Couldn't save the setting") and self.said[0][1]))
        self.assertTrue(self.fresh(lambda: q.index == 2), "ft-together has it all the same, until it ends")

    def test_it_goes_with_the_program(self):
        q = self.room()
        self.together.quality = "high"
        self.assertTrue(self.fresh(lambda: q.index == 2))
        self.together.tailscale = False    # ft-together answers no more as a room would
        self.assertTrue(self.fresh(lambda: not self.b.ready))
        self.assertEqual((q.can, q.index), (False, 1))

    def test_the_page(self):
        """The page in a room: the box shows what ft-together says, and asks what is picked."""
        self.install()
        self.together.mode, self.together.people, self.together.quality = "hosting", [ADA, BEN], "low"
        _, found, warnings = self.window()
        self.assertTrue(self.until(lambda: found("qualityBox") is not None and found("qualityBox").property("enabled")), "the page asks by itself")
        box = found("qualityBox")
        self.assertEqual((box.property("currentIndex"), box.property("count"), box.property("currentText")), (0, 3, "Low"))
        self.assertIn("1 Mbit/s and 15 pictures a second", found("qualityNote").property("text"))
        self.assertIn("it gets less by itself on a slow link", found("qualityNote").property("text"))
        box.setProperty("currentIndex", 2)
        box.activated.emit(2)
        self.assertEqual(box.property("currentIndex"), 0, "the box shows what ft-together has, also until it has answered")
        self.assertTrue(self.until(lambda: self.together.asked == ["quality high"] and self.b.quality.index == 2 and self.kept() == "high"))
        self.assertIn("8 Mbit/s and 60 pictures a second", found("qualityNote").property("text"))
        self.together.quality = "balanced"   # ft-together started again, with another
        self.assertTrue(self.until(lambda: box.property("currentIndex") == 1), "the box follows what ft-together says")
        self.together.quality = None
        box.setProperty("currentIndex", 0)
        box.activated.emit(0)
        self.assertTrue(self.until(lambda: self.said[-1:] == [("What", True)] and not box.property("enabled") and box.property("currentIndex") == 1), "a no puts the box back")
        self.assertEqual([w for w in warnings if "not placed in the graphics scene" not in w], [])


if __name__ == "__main__":
    unittest.main()
