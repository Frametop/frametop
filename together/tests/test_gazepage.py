#!/usr/bin/env python3
"""Where you look, on Frametop Settings' Together page (roompage.gaze, ft_together_gaze.py,
YourShares.qml), offline: ft-together's `gaze` read into the switch and what it says, and the
backend and the page against a stand-in that answers as ft-together does (pagekit.py). The
backend's part needs PySide6, the page's Kirigami too.

    python3 together/tests/test_gazepage.py
"""
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE))
import roompage  # noqa: E402
from pagekit import ADA, BEN, PageCase  # noqa: E402


def said(shared=False, fresh=False, quiet=False):
    return {"shared": shared, "fresh": fresh, "quiet": quiet}


class Rules(unittest.TestCase):
    def test_what_the_switch_says(self):
        self.assertEqual(roompage.gaze(said()), {"can": True, "shared": False, "text": "Not shared"})
        row = roompage.gaze(said(quiet=True))
        self.assertEqual((row["can"], row["shared"]), (True, False))
        self.assertIn("only while you click or drag", row["text"])
        self.assertEqual(roompage.gaze(said(shared=True, fresh=True)), {"can": True, "shared": True, "text": "The room sees where you look"})
        row = roompage.gaze(said(shared=True))
        self.assertEqual((row["can"], row["shared"]), (True, True))
        self.assertIn("Nothing is sent now", row["text"])
        self.assertEqual(roompage.gaze(said(shared=True, quiet=True))["text"], row["text"], "shared, the pointer isn't kept back")

    def test_only_a_plain_yes_is_one(self):
        self.assertIn("Nothing is sent now", roompage.gaze({"shared": True, "fresh": 1})["text"])
        self.assertEqual(roompage.gaze({"shared": False, "quiet": "yes"})["text"], "Not shared")
        self.assertEqual(roompage.gaze({"shared": False})["text"], "Not shared")

    def test_an_answer_that_isnt_one(self):
        for other in (None, {}, {"shared": 1}, {"shared": "true"}, {"fresh": True, "quiet": True}):
            self.assertEqual(roompage.gaze(other), {"can": False, "shared": False, "text": "Frametop Together doesn't say"}, other)


class Page(PageCase):
    def room(self):
        self.install()
        self.together.mode, self.together.people = "hosting", [ADA, BEN]
        self.assertTrue(self.fresh(lambda: self.b.gaze.can))
        return self.b.gaze

    def test_turned_on_and_off(self):
        g = self.room()
        self.assertEqual((g.shared, g.text), (False, "Not shared"))
        g.setShared(True)
        self.assertTrue(self.fresh(lambda: g.shared))
        self.assertIn("Nothing is sent now", g.text)
        self.together.gaze["fresh"] = True
        self.assertTrue(self.fresh(lambda: g.text == "The room sees where you look"))
        g.setShared(False)
        self.assertTrue(self.fresh(lambda: not g.shared))
        self.assertEqual(self.together.asked, ["gaze on", "gaze off"])
        self.assertEqual(self.said, [])

    def test_a_no_is_told(self):
        g = self.room()
        self.together.mode = "idle"
        g.setShared(True)
        self.assertTrue(self.until(lambda: self.said == [("You aren't in a room", True)]))
        self.assertFalse(g.shared)

    def test_it_goes_with_the_room(self):
        g = self.room()
        self.together.gaze.update(shared=True, fresh=True)
        self.assertTrue(self.fresh(lambda: g.shared))
        self.together.tailscale = False      # ft-together answers no more as a room would
        self.assertTrue(self.fresh(lambda: not self.b.ready))
        self.assertEqual((g.can, g.shared), (False, False))
        # ...and when it answers again, nothing of the old answer shows before the new one is in
        seen = []
        self.b.changed.connect(lambda: seen.append(g.shared))
        self.together.gaze.update(shared=False, fresh=False)
        self.together.tailscale = True
        self.assertTrue(self.fresh(lambda: g.can))
        self.assertEqual(set(seen), {False})

    def test_an_ft_together_that_knows_no_gaze(self):
        self.install()
        self.together.mode, self.together.people, self.together.gaze = "hosting", [ADA], None
        self.assertTrue(self.fresh(lambda: self.b.voiceUp))
        self.assertEqual((self.b.gaze.can, self.b.gaze.shared, self.b.gaze.text), (False, False, "Frametop Together doesn't say"))

    def test_the_page(self):
        """The page in a room: the switch shows what ft-together says, and asks what it says."""
        self.install()
        self.together.mode, self.together.people = "hosting", [ADA, BEN]
        self.together.gaze["quiet"] = True
        _, found, warnings = self.window()
        self.assertTrue(self.until(lambda: found("gazeSwitch") is not None and found("gazeSwitch").property("enabled")), "the page asks by itself")
        switch = found("gazeSwitch")
        self.assertFalse(switch.property("checked"))
        self.assertIn("only while you click or drag", switch.property("text"))
        self.assertIn("never a picture of your eyes", found("gazeNote").property("text"))
        self.assertIn("a faint mark on the panel you look at", found("gazeNote").property("text"))
        switch.toggle()
        switch.toggled.emit()
        self.assertTrue(self.until(lambda: self.together.asked == ["gaze on"] and self.b.gaze.shared and switch.property("checked")))
        self.assertIn("Nothing is sent now", switch.property("text"))
        self.together.gaze["fresh"] = True
        self.assertTrue(self.until(lambda: switch.property("text") == "The room sees where you look"))
        self.together.gaze.update(shared=False, fresh=False)     # the room was left and come back to: off again
        self.assertTrue(self.until(lambda: not switch.property("checked")), "the switch follows what ft-together says")
        switch.toggle()
        switch.toggled.emit()
        self.assertTrue(self.until(lambda: self.together.asked == ["gaze on", "gaze on"] and switch.property("checked")))
        switch.toggle()
        switch.toggled.emit()
        self.assertTrue(self.until(lambda: self.together.asked[-1] == "gaze off" and not switch.property("checked")))
        self.together.gaze_no = "not now"
        switch.toggle()
        switch.toggled.emit()
        self.assertTrue(self.until(lambda: self.said[-1:] == [("Not now", True)] and not switch.property("checked")), "a no puts the switch back")
        self.assertTrue(found("muteSwitch").property("visible") and found("avatarLabel").property("visible"), "the rest of what you share is there")
        self.together.mode, self.together.people = "idle", []
        self.assertTrue(self.until(lambda: not switch.property("visible")), "outside a room there is nothing to share")
        self.assertEqual([w for w in warnings if "not placed in the graphics scene" not in w], [])

    def test_the_page_opens_on_what_is(self):
        self.install()
        self.together.mode, self.together.people = "joined", [ADA, BEN]
        self.together.gaze.update(shared=True, fresh=True)
        _, found, warnings = self.window()
        self.assertTrue(self.until(lambda: found("gazeSwitch") is not None and found("gazeSwitch").property("checked")))
        self.assertEqual(found("gazeSwitch").property("text"), "The room sees where you look")
        self.assertEqual(self.together.asked, [])
        self.assertEqual([w for w in warnings if "not placed in the graphics scene" not in w], [])


if __name__ == "__main__":
    unittest.main()
