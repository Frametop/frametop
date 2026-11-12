#!/usr/bin/env python3
"""The room's panels on Frametop Settings' Together page (together/controlpage.py,
ft_together_control.py, RoomPanels.qml), offline: ft-together's `state` and `control` read into
rows, and the backend and the page driven against the stand-ins of pagekit.py: someone asks for
a screen, is given it and has it taken back; a panel of theirs asked for; and a jump to a person
and to a panel. The backend's part needs PySide6, the page's Kirigami too.

    python3 together/tests/test_controlpage.py
"""
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE))
import controlpage  # noqa: E402
from pagekit import ADA, BEN, PageCase, panel  # noqa: E402

CY = {"id": 2, "name": "cy", "login": "cy@example.org", "away": 0, "linked": True, "spot": [2, 0, 0, 0]}
STATE = {"me": 0, "owner": 0, "people": [ADA, BEN, CY], "panels": [panel(0, "Screen 1"), panel(1, "Screen 2"), panel(8, "Screen 1"), panel(17, "Screen 3")]}


def control(**said):
    return dict({"there": True, "left": 9.5, "asks": [], "given": [], "holder": -1, "held": [], "asked": []}, **said)


class Rules(unittest.TestCase):
    def test_who_asks_and_who_was_given(self):
        said = control(asks=[{"number": 1, "person": 1}, {"number": 0, "person": 2}], given=[{"number": 0, "person": 1}])
        self.assertEqual(controlpage.asks(STATE, said), [{"number": 1, "person": 1, "text": "ben asks to use your Screen 2"},
                                                         {"number": 0, "person": 2, "text": "cy asks to use your Screen 1"}])
        self.assertEqual(controlpage.given(STATE, said), [{"number": 0, "person": 1, "text": "ben can use your Screen 1"}])
        self.assertEqual((controlpage.asks(STATE, control()), controlpage.given(STATE, control())), ([], []))

    def test_only_people_in_the_room_and_panels_that_are_shared(self):
        pairs = [{"number": 2, "person": 1},     # no such panel of this headset's
                 {"number": 0, "person": 3},     # nobody in that place
                 {"number": 0, "person": 0},     # this headset's own person
                 {"number": True, "person": 1}, {"number": "0", "person": 1}, {"person": 1}, "0 1", None, [0, 1]]
        self.assertEqual(controlpage.asks(STATE, control(asks=pairs)), [])
        self.assertEqual(controlpage.given(STATE, control(given=pairs)), [])
        # from ben's side his own panel has number 0 too: cy asks for that, not for ada's
        bens = dict(STATE, me=1)
        self.assertEqual([r["text"] for r in controlpage.asks(bens, control(asks=[{"number": 0, "person": 2}]))], ["cy asks to use your Screen 1"])
        self.assertEqual(controlpage.asks(bens, control(asks=[{"number": 1, "person": 2}])), [])

    def test_answers_that_arent_what_they_should_be(self):
        for said in ({}, {"asks": None, "given": 7, "held": "x", "asked": {"a": 1}}, {"asks": [[]], "held": [3], "asked": [None, "8"]}):
            self.assertEqual((controlpage.asks(STATE, said), controlpage.given(STATE, said), controlpage.note(STATE, said)), ([], [], ""))
            self.assertEqual([r["what"] for r in controlpage.panels(STATE, said)], ["", "", "", ""])
        for state in ({}, {"people": None, "panels": None}, {"people": [1, "x"], "panels": ["y", {"id": "8"}, {"id": True}, {"owner": 1}]}):
            self.assertEqual((controlpage.asks(state, control()), controlpage.panels(state, control())), ([], []))

    def test_a_note_while_nothing_can_be_given(self):
        asking = [{"number": 0, "person": 1}]
        self.assertEqual(controlpage.note(STATE, control(asks=asking)), "")
        self.assertIn("only while you wear the headset", controlpage.note(STATE, control(there=False, asks=asking)))
        self.assertIn("only while you wear the headset", controlpage.note(STATE, control(there=1, asks=asking)))    # only a plain yes is one
        self.assertEqual(controlpage.note(STATE, control(there=False)), "")                                          # nobody asks: nothing to say
        self.assertEqual(controlpage.note(STATE, control(there=False, asks=[{"number": 5, "person": 1}])), "")

    def test_the_rooms_panels(self):
        rows = controlpage.panels(STATE, control(held=[{"panel": 8, "left": 7.5}], asked=[17]))
        self.assertEqual(rows, [
            {"id": 0, "title": "Your Screen 1", "mine": True, "what": "", "ask": False},
            {"id": 1, "title": "Your Screen 2", "mine": True, "what": "", "ask": False},
            {"id": 8, "title": "ben's Screen 1", "mine": False, "what": "you can use it", "ask": False},
            {"id": 17, "title": "cy's Screen 3", "mine": False, "what": "asked…", "ask": False}])
        rows = controlpage.panels(STATE, control(held=[{"panel": 8, "left": 7.5}], asked=[8, 0]))
        self.assertEqual([(r["what"], r["ask"]) for r in rows], [("", False), ("", False), ("you can use it", False), ("", True)])
        # as ben sees the same room
        rows = controlpage.panels(dict(STATE, me=1), control())
        self.assertEqual([(r["title"], r["ask"]) for r in rows], [("ada's Screen 1", True), ("ada's Screen 2", True), ("Your Screen 1", False),
                                                                  ("cy's Screen 3", True)])

    def test_names_a_peer_chose(self):
        state = {"me": 0, "people": [ADA, dict(BEN, name="  ben\n<b>the</b>\tbold " + "x" * 80), {"id": 2, "name": ""}],
                 "panels": [panel(0, ""), panel(8, "a\nscreen " + "y" * 80), panel(16, None), panel(24, "Gone")]}
        rows = controlpage.panels(state, control())
        self.assertEqual(rows[0]["title"], "Your Panel 0")
        self.assertTrue(rows[1]["title"].startswith("ben <b>the</b> bold xxx") and "\n" not in rows[1]["title"])
        self.assertLessEqual(len(rows[1]["title"]), 48 + len("'s ") + 48)
        self.assertEqual(rows[2]["title"], "Person 2's Panel 16")
        self.assertEqual(rows[3]["title"], "Someone's Gone")            # its owner isn't among the room's people
        text = controlpage.asks(state, control(asks=[{"number": 0, "person": 1}]))[0]["text"]
        self.assertTrue(text.startswith("ben <b>the</b> bold x") and text.endswith(" asks to use your Panel 0") and "\n" not in text)


class Page(PageCase):
    def room(self):
        self.install()
        self.together.mode, self.together.people = "hosting", [ADA, BEN]
        self.together.panels = [panel(0, "Screen 1"), panel(8, "Screen 1")]
        self.assertTrue(self.fresh(lambda: len(self.b.control.panels) == 2))
        return self.b.control

    def test_asked_given_and_taken_back(self):
        c = self.room()
        self.assertEqual((c.asks, c.given, c.note), ([], [], ""))
        self.together.control["asks"] = [{"number": 0, "person": 1}]
        self.assertTrue(self.fresh(lambda: [a["text"] for a in c.asks] == ["ben asks to use your Screen 1"]))
        self.together.control["there"] = False
        self.assertTrue(self.fresh(lambda: c.note != ""))
        c.give(0, 1)
        self.assertTrue(self.until(lambda: self.said and self.said[-1][1] and self.said[-1][0].startswith("Nobody says you have the headset on")))
        self.assertEqual(len(c.asks), 1)
        self.together.control["there"] = True
        c.give(0, 1)
        self.assertTrue(self.fresh(lambda: c.asks == [] and [g["text"] for g in c.given] == ["ben can use your Screen 1"]))
        self.assertEqual(c.note, "")
        c.take(0, 1)
        self.assertTrue(self.fresh(lambda: c.given == []))
        self.together.control["asks"] = [{"number": 0, "person": 1}]
        self.assertTrue(self.fresh(lambda: len(c.asks) == 1))
        c.take(0, 1)       # a no
        self.assertTrue(self.fresh(lambda: c.asks == [] and c.given == []))
        self.assertEqual(self.together.asked, ["control give 0 1", "control give 0 1", "control take 0 1", "control take 0 1"])

    def test_asking_for_someone_elses(self):
        c = self.room()
        self.assertEqual([(p["id"], p["title"], p["ask"]) for p in c.panels], [(0, "Your Screen 1", False), (8, "ben's Screen 1", True)])
        c.ask(8)
        self.assertTrue(self.fresh(lambda: [(p["what"], p["ask"]) for p in c.panels] == [("", False), ("asked…", False)]))
        self.together.control["held"], self.together.control["asked"] = [{"panel": 8, "left": 9.0}], []
        self.assertTrue(self.fresh(lambda: c.panels[1]["what"] == "you can use it"))
        c.ask(40)
        self.assertTrue(self.until(lambda: self.said and self.said[-1] == ("No such panel", True)))
        self.assertEqual(self.together.asked, ["control ask 8", "control ask 40"])

    def test_going_to_someone_and_to_a_panel(self):
        self.room()
        self.assertEqual([p["you"] for p in self.b.people], [True, False])
        self.b.goTo("person", 1)
        self.b.goTo("panel", 8)
        self.assertTrue(self.until(lambda: self.panels.asked == ["go person 1", "go panel 8"]))
        self.assertEqual(self.said, [])
        self.b.goTo("panel", 9)
        self.assertTrue(self.until(lambda: self.said == [("No head pose (is the headset on?)", True)]))
        for kind in ("", "place", "panel 8\ngo", "person 1; leave"):
            self.b.goTo(kind, 1)
        self.b.goTo("person", 1)
        self.assertTrue(self.until(lambda: self.panels.asked[-1] == "go person 1" and len(self.panels.asked) > 3))
        self.assertEqual(self.panels.asked, ["go person 1", "go panel 8", "go panel 9", "go person 1"], "nothing but a person or a panel is gone to")

    def test_it_goes_with_the_room(self):
        c = self.room()
        self.together.control["asks"] = [{"number": 0, "person": 1}]
        self.assertTrue(self.fresh(lambda: len(c.asks) == 1))
        self.together.tailscale = False      # ft-together answers no more as a room would
        self.assertTrue(self.fresh(lambda: not self.b.ready))
        self.assertEqual((c.asks, c.given, c.panels, c.note), ([], [], [], ""))
        # ...and when it answers again, nothing of the old answer shows before the new one is in
        seen = []
        self.b.changed.connect(lambda: seen.append(len(c.asks)))
        self.together.control["asks"] = []
        self.together.tailscale = True
        self.assertTrue(self.fresh(lambda: len(c.panels) == 2))
        self.assertEqual(set(seen), {0})

    def test_the_page(self):
        """The page with someone asking for a screen and a panel of theirs in the room: its rows
        show without a warning, and its buttons ask what they say."""
        self.install()
        self.together.mode, self.together.people = "hosting", [ADA, dict(BEN, name="ben <i>x</i>")]
        self.together.panels = [panel(0, "Screen 1"), panel(8, "Screen 1")]
        self.together.control["asks"] = [{"number": 0, "person": 1}]
        root, found, warnings = self.window()

        def one(name):
            return self.items(root, name)[0]
        self.assertTrue(self.until(lambda: found("roomPanels") is not None and found("roomPanels").property("count") == 2
                                   and found("controlAsks").property("count") == 1), "the page asks by itself")
        self.assertTrue(found("roomPanelRows").property("visible"))
        self.assertEqual(found("controlGiven").property("count"), 0)
        self.assertEqual(one("askLabel").property("text"), "ben <i>x</i> asks to use your Screen 1")
        self.assertIs(self.plain(one("askLabel")), True)
        self.assertFalse(found("controlNote").property("visible"))
        one("giveButton").clicked.emit()
        self.assertTrue(self.until(lambda: found("controlAsks").property("count") == 0 and found("controlGiven").property("count") == 1))
        self.assertEqual(one("givenLabel").property("text"), "ben <i>x</i> can use your Screen 1")
        self.assertIs(self.plain(one("givenLabel")), True)
        one("takeButton").clicked.emit()
        self.assertTrue(self.until(lambda: found("controlGiven").property("count") == 0))
        self.together.control["asks"] = [{"number": 0, "person": 1}]
        self.assertTrue(self.until(lambda: found("controlAsks").property("count") == 1))
        one("refuseButton").clicked.emit()
        self.assertTrue(self.until(lambda: found("controlAsks").property("count") == 0))
        self.assertEqual(self.together.asked, ["control give 0 1", "control take 0 1", "control take 0 1"])
        self.assertEqual([label.property("text") for label in self.items(root, "panelLabel")], ["Your Screen 1", "ben <i>x</i>'s Screen 1"])
        self.assertEqual([self.plain(label) for label in self.items(root, "panelLabel")], [True, True])
        buttons = self.items(root, "askButton")
        self.assertEqual([b.property("visible") for b in buttons], [False, True])
        buttons[1].clicked.emit()
        self.assertTrue(self.until(lambda: self.together.asked[-1] == "control ask 8" and not buttons[1].property("visible")))
        go = self.items(root, "goPanel")
        self.assertEqual(len(go), 2)
        go[1].clicked.emit()
        people = self.items(root, "goPerson")
        self.assertEqual([b.property("visible") for b in people], [False, True])
        people[1].clicked.emit()
        self.assertTrue(self.until(lambda: self.panels.asked == ["go panel 8", "go person 1"]))
        self.together.panels, self.together.control["asked"] = [], []
        self.assertTrue(self.until(lambda: not found("roomPanelRows").property("visible")), "no panels, no asks: no rows")
        self.assertEqual([w for w in warnings if "not placed in the graphics scene" not in w], [])


if __name__ == "__main__":
    unittest.main()
