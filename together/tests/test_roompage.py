#!/usr/bin/env python3
"""Frametop Settings' Together page (together/roompage.py, ft_together_settings.py,
TogetherPages.qml), offline: what the room's programs answer read into what the page says, what
its person types read into a `join`, and the backend and the page driven against stand-ins that
answer as ft-together, ft-roomvoice, ft-roompanels and ft-screens do (pagekit.py): nothing is
asked of the real programs or the host. The backend's part needs PySide6, the page's Kirigami
too. The avatar's row is test_roomavatar.py's.

    python3 together/tests/test_roompage.py
"""
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE))
import roompage  # noqa: E402
from pagekit import ADA, BEN, PageCase, page, state  # noqa: E402


class Rules(unittest.TestCase):
    def test_answers(self):
        self.assertEqual(roompage.told('ok {"a": 1}'), {"a": 1})
        for other in (None, "", "ok", "ok {", "ok [1]", "error no", '{"a": 1}'):
            self.assertIsNone(roompage.told(other), other)
        self.assertEqual(roompage.why_not("error not in a room"), "Not in a room")
        self.assertEqual(roompage.why_not(None), "No answer")
        self.assertEqual(roompage.why_not("ok"), "No answer")

    def test_here(self):
        at = roompage.here("ok 100.64.0.7 47193 ada's frame")
        self.assertEqual(at, {"address": "100.64.0.7", "port": 47193, "name": "ada's frame"})
        self.assertEqual(roompage.here_text(at), "100.64.0.7")
        self.assertEqual(roompage.here_text(roompage.here("ok 100.64.0.7 5000 ada")), "100.64.0.7:5000")
        for other in (None, "", "ok", "ok 100.64.0.7 x ada", "error Tailscale isn't running on this headset", "ok 1 2"):
            self.assertIsNone(roompage.here(other), other)
        self.assertEqual(roompage.here_text(None), "")

    def test_an_invites_code(self):
        self.assertEqual(roompage.code("ok ABCD2345"), "ABCD2345")
        for other in (None, "ok", "ok A B", "error not your room", "ok <b>x</b>", "ok " + "A" * 40):
            self.assertEqual(roompage.code(other), "", other)

    def test_what_is_typed_to_join(self):
        for typed, expect in (("ada-frame", "join ada-frame 47193 ABCD2345"), (" 100.64.0.7 ", "join 100.64.0.7 47193 ABCD2345"),
                              ("ada-frame.tail1234.ts.net:5000", "join ada-frame.tail1234.ts.net 5000 ABCD2345"),
                              ("ada-frame 5000", "join ada-frame 5000 ABCD2345"), ("ada-frame, 5000", "join ada-frame 5000 ABCD2345")):
            self.assertEqual(roompage.join_words(typed, " abcd 2345 ".upper()), (expect, ""), typed)
        for typed in ("", "  ", "a b c", "ada frame", "ada:0", "ada:70000", "ada:x", "-ada", "ada;leave", "ada\nleave 1 2", "fd7a::1", "a" * 300):
            question, wrong = roompage.join_words(typed, "ABCD2345")
            self.assertIsNone(question, typed)
            self.assertTrue(wrong)
        for typed_code in ("", "abc", "ABCD;2345", "ABCD\x002345", "A" * 40):
            self.assertIsNone(roompage.join_words("ada", typed_code)[0], typed_code)
        # a code's spaces go, so what was typed after one is never a word of its own
        self.assertEqual(roompage.join_words("ada", "ABCD 2345 leave")[0], "join ada 47193 ABCD2345leave")

    def test_the_desktops_screens(self):
        self.assertEqual(roompage.screens("ok 2 1:3440x1440:1.200 2:1920x1080:1.200"), [(1, "3440x1440"), (2, "1920x1080")])
        for other in (None, "", "ok", "error no", "ok 1 one:1x1:1", "ok 1 1:big:1", "ok 1 1"):
            self.assertEqual(roompage.screens(other), [], other)

    def test_what_the_switch_says(self):
        self.assertEqual(roompage.status(False, False, None, "nobody"), ("Not installed", False))
        self.assertEqual(roompage.status(True, False, None, "nobody"), ("Off", False))
        self.assertEqual(roompage.status(True, True, None, "nobody"), ("Starting…", False))
        self.assertEqual(roompage.status(True, True, None, "timeout"), ("Not answering", False))
        self.assertEqual(roompage.status(True, True, "error Tailscale isn't running on this headset", ""),
                         ("Tailscale isn't running on this headset", False))
        self.assertEqual(roompage.status(True, True, state(), ""), ("On", True))
        self.assertEqual(roompage.status(False, False, state(), ""), ("On", True))   # started by hand

    def test_the_room_in_a_line(self):
        said = roompage.told
        self.assertEqual(roompage.room_text(said(state())), "You are in no room")
        self.assertEqual(roompage.room_text(said(state("hosting", [ADA]))), "Your room is open: nobody else is in it yet")
        self.assertEqual(roompage.room_text(said(state("hosting", [ADA, BEN]))), "Your room is open, with 1 other in it")
        self.assertEqual(roompage.room_text(said(state("hosting", [ADA, BEN, BEN]))), "Your room is open, with 2 others in it")
        self.assertEqual(roompage.room_text(said(state("calling"))), "Asking to come in…")
        self.assertEqual(roompage.room_text(said(state("joined", [ADA, BEN], me=1))), "You are in ada's room")
        self.assertEqual(roompage.room_text(said(state("joined", [BEN], me=1))), "You are in a room")
        self.assertEqual(roompage.room_text({}), "You are in no room")

    def test_people_and_asks(self):
        rows = roompage.people(roompage.told(state("hosting", [ADA, BEN, dict(BEN, id=2, name="cy", away=1, linked=False), "junk"])))
        self.assertEqual([(r["id"], r["name"], r["what"]) for r in rows],
                         [(0, "ada", "you, the room's owner"), (1, "ben", "headset off"), (2, "cy", "in a game, connecting")])
        self.assertEqual([r["you"] for r in rows], [True, False, False])
        guest = roompage.people(roompage.told(state("joined", [ADA, BEN], me=1)))
        self.assertEqual([(r["you"], r["what"]) for r in guest], [(False, "the room's owner, connecting"), (True, "you, headset off")])
        self.assertEqual(roompage.people({}), [])
        asked = roompage.asks(roompage.told(state("hosting", [ADA], [{"id": 3, "name": "ben", "login": "ben@example.org", "address": "100.64.0.9"}, 7])))
        self.assertEqual(asked, [{"id": 3, "name": "ben", "login": "ben@example.org", "address": "100.64.0.9"}])

    def test_the_screens_to_share(self):
        found = [(1, "3440x1440"), (2, "1920x1080"), (3, "1920x1080"), (4, "1280x720")]
        rows = roompage.shares(found, {"shares": [{"screen": 2, "panel": 0, "told": True}, {"screen": 3, "panel": 1, "told": False}]})
        self.assertEqual([(r["screen"], r["shared"], r["live"], r["can"]) for r in rows],
                         [(1, False, False, True), (2, True, True, True), (3, True, False, True), (4, False, False, True)])
        self.assertEqual(rows[0]["title"], "Screen 1 (3440 × 1440)")
        full = roompage.shares(found, {"shares": [{"screen": n, "panel": n - 1, "told": True} for n in (1, 2, 3)]})
        self.assertEqual([r["can"] for r in full], [True, True, True, False])   # three are shared: no fourth
        self.assertEqual([r["can"] for r in roompage.shares(found, None)], [False] * 4)   # ft-roompanels isn't there
        self.assertEqual(roompage.shares([], {"shares": []}), [])
        self.assertEqual({r["wait"] for r in rows}, {"starting…"})

    def test_the_windows_of_its_pc_to_share(self):
        found = [(1, "3440x1440")]
        windows = [{"screen": 101, "name": "Calculator"}, {"screen": 102, "name": "a\nb\x00 <b> " + "x" * 80}, {"screen": 103, "name": ""}]
        state = {"shares": [{"screen": 1, "panel": 0, "told": True}, {"screen": 101, "panel": 3, "told": True}, {"screen": 102, "panel": 4, "told": False}],
                 "windows": windows, "pc": ""}
        rows = roompage.shares(found, state)
        self.assertEqual([(r["screen"], r["shared"], r["live"], r["can"]) for r in rows],
                         [(1, True, True, True), (101, True, True, True), (102, True, False, True), (103, False, False, True)])
        self.assertEqual([r["title"] for r in rows[1:]], ["Calculator (your PC's window)", "a b <b> " + "x" * 52 + " (your PC's window)", "A window (your PC's window)"])
        self.assertEqual(rows[2]["wait"], "calling your PC…")
        held = roompage.shares(found, dict(state, pc="the PC says: Forbidden\n" + "y" * 200))
        self.assertEqual(held[2]["wait"], "the PC says: Forbidden " + "y" * 97)
        # the desktop's three and the PC's four are counted apart
        many = [{"screen": n, "name": f"W{n}"} for n in range(101, 106)]
        took = [{"screen": n, "panel": n - 98, "told": True} for n in range(101, 105)]
        full = roompage.shares([(1, "1920x1080"), (2, "1920x1080")], {"shares": took, "windows": many, "pc": ""})
        self.assertEqual([r["can"] for r in full], [True, True, True, True, True, True, False])
        three = [{"screen": n, "panel": n - 1, "told": True} for n in (1, 2, 3)]
        full = roompage.shares([(n, "1920x1080") for n in (1, 2, 3, 4)], {"shares": three, "windows": many[:1], "pc": ""})
        self.assertEqual([r["can"] for r in full], [True, True, True, False, True])
        # what doesn't read as a window is no row
        for bad in ("windows", [3], [{"screen": "101", "name": "x"}], [{"screen": 5, "name": "x"}], [{"name": "x"}], [{"screen": True}]):
            self.assertEqual(roompage.shares([], {"shares": [], "windows": bad, "pc": 7}), [], bad)
        self.assertEqual(roompage.shares([], {"shares": [{"screen": "1"}, 4], "windows": many[:1]})[0]["shared"], False)


class Page(PageCase):
    def test_not_installed(self):
        self.together.tailscale = False   # (and nothing here answers as a room would)
        self.assertFalse(self.b.installed)
        self.assertFalse(self.b.on)
        self.b.setOn(True)
        self.assertEqual(self.said, [("Frametop Together isn't installed", True)])
        self.assertEqual(self.host, [])

    def test_the_switch(self):
        self.install(on=False)
        self.together.tailscale = False
        self.assertTrue(self.b.installed)
        self.assertFalse(self.b.on)
        self.b.setOn(True)
        self.assertTrue(self.b.busy)
        self.assertEqual(self.b.status, "Starting…")
        self.b.setOn(False)   # while the first still runs
        self.assertEqual(self.said, [("Still busy: try again in a moment", True)])
        self.assertTrue(self.until(lambda: not self.b.busy))
        self.assertEqual(self.host, [("systemctl", "--user", "enable", "--now", page.UNIT)])
        self.install(on=True)   # what systemctl did
        self.assertTrue(self.fresh(lambda: self.b.status == "Tailscale isn't running on this headset"))
        self.assertFalse(self.b.ready)
        self.together.tailscale = True
        self.assertTrue(self.fresh(lambda: self.b.ready and self.b.status == "On" and self.b.here == "100.64.0.7"))
        self.systemctl = ["false"]
        self.b.setOn(False)
        self.assertTrue(self.until(lambda: not self.b.busy))
        self.assertEqual(self.host[-1], ("systemctl", "--user", "disable", "--now", page.UNIT))
        self.assertEqual(self.said[-1], ("Frametop Together didn't turn off (systemctl: exit code 1)", True))

    def test_a_room_of_ones_own(self):
        self.install()
        self.assertTrue(self.fresh(lambda: self.b.ready))
        self.assertEqual((self.b.mode, self.b.roomText, self.b.people, self.b.code), ("idle", "You are in no room", [], ""))
        self.b.invite()
        self.assertTrue(self.until(lambda: self.said == [("It isn't your room", True)]))
        self.b.openRoom()
        self.assertTrue(self.fresh(lambda: self.b.mode == "hosting"))
        self.assertEqual([p["what"] for p in self.b.people], ["you, the room's owner"])
        self.b.invite()
        self.assertTrue(self.until(lambda: self.b.code == "ABCD2345"))
        self.together.asks = [{"id": 4, "name": "ben", "login": "ben@example.org", "address": "100.64.0.9"}]
        self.assertTrue(self.fresh(lambda: [a["id"] for a in self.b.asks] == [4]))
        self.b.decline(9)
        self.assertTrue(self.until(lambda: self.said[-1] == ("No such ask", True)))
        self.b.accept(4)
        self.assertTrue(self.fresh(lambda: [p["name"] for p in self.b.people] == ["ada", "ben"] and self.b.asks == []))
        self.assertEqual(self.b.roomText, "Your room is open, with 1 other in it")
        self.b.leave()
        self.assertTrue(self.fresh(lambda: self.b.mode == "idle"))
        self.assertEqual(self.b.code, "", "an invite goes with its room")
        self.assertEqual(self.together.asked, ["invite", "open", "invite", "decline 9", "accept 4", "leave"])

    def test_joining(self):
        self.install()
        self.assertTrue(self.fresh(lambda: self.b.ready))
        self.b.join("ben frame", "ABCD2345")
        self.b.join("ben-frame", "")
        self.assertEqual([bad for _, bad in self.said], [True, True])
        self.assertEqual(self.together.asked, [], "nothing that doesn't read as an address and a code is sent")
        self.b.join("ben-frame:5000", "abcd 2345")
        self.assertTrue(self.fresh(lambda: self.b.mode == "calling"))
        self.assertEqual(self.together.asked, ["join ben-frame 5000 abcd2345"])
        self.assertEqual(self.b.roomText, "Asking to come in…")

    def test_voice_and_screens(self):
        self.install()
        self.together.mode, self.together.people = "hosting", [ADA]
        self.assertTrue(self.fresh(lambda: self.b.voiceUp and len(self.b.shares) == 2))
        self.assertEqual((self.b.muted, self.b.voiceText), (False, "The room hears you"))
        self.b.setMuted(True)
        self.assertTrue(self.fresh(lambda: self.b.muted))
        self.assertEqual(self.b.voiceText, "Muted: the room doesn't hear you")
        self.b.setMuted(False)
        self.assertTrue(self.fresh(lambda: not self.b.muted))
        self.assertEqual(self.voice.asked, ["mute", "unmute"])
        self.assertEqual([(s["screen"], s["shared"]) for s in self.b.shares], [(1, False), (2, False)])
        self.b.setShared(2, True)
        self.assertTrue(self.fresh(lambda: [(s["screen"], s["shared"], s["live"]) for s in self.b.shares] == [(1, False, False), (2, True, True)]))
        self.b.setShared(7, True)
        self.assertTrue(self.until(lambda: self.said and self.said[-1] == ("Screen 7 isn't one of this desktop's screens", True)))
        self.b.setShared(2, False)
        self.assertTrue(self.fresh(lambda: not any(s["shared"] for s in self.b.shares)))
        self.assertEqual(self.panels.asked, ["share 2", "share 7", "unshare 2"])

    def test_a_window_of_the_pcs_is_shared_like_a_screen(self):
        self.install()
        self.together.mode, self.together.people = "hosting", [ADA]
        self.panels.windows = [{"screen": 101, "name": "Calculator"}]
        self.assertTrue(self.fresh(lambda: [(s["screen"], s["title"], s["shared"]) for s in self.b.shares][2:] == [(101, "Calculator (your PC's window)", False)]))
        self.b.setShared(101, True)
        self.assertTrue(self.fresh(lambda: [(s["screen"], s["shared"], s["live"]) for s in self.b.shares][2:] == [(101, True, True)]))
        self.assertEqual(self.panels.asked, ["share 101"])

    def test_the_page(self):
        """The page itself over a room with someone asking, people in it and a shared screen: it
        loads without a warning, and its buttons ask what they say."""
        self.install()
        self.together.mode, self.together.people = "hosting", [ADA, BEN]
        self.together.asks = [{"id": 4, "name": "cy <b>", "login": "cy@example.org", "address": "100.64.0.9"}]
        self.panels.shared = {1: 0}
        root, found, warnings = self.window()
        self.assertTrue(self.until(lambda: self.b.mode == "hosting" and len(self.b.shares) == 2 and self.b.voiceUp), "the page asks by itself")
        self.assertTrue(self.until(lambda: found("people") is not None and found("people").property("count") == 2))
        self.assertEqual(found("asks").property("count"), 1)
        self.assertTrue(self.until(lambda: len(self.items(root, "askText")) == 1))
        ask = self.items(root, "askText")[0]
        self.assertEqual(ask.property("text"), "cy <b> asks to come in (cy@example.org)")
        self.assertIs(self.plain(ask), True, "the name the asker chose isn't read as markup")
        self.items(root, "askLetIn")[0].clicked.emit()
        self.assertTrue(self.until(lambda: self.together.asked and self.together.asked[-1] == "accept 4"))
        self.assertEqual(found("shares").property("count"), 2)
        self.assertEqual(found("roomLabel").property("text"), "Your room is open, with 1 other in it")
        self.assertEqual(found("hereLabel").property("text"), "100.64.0.7")
        self.assertTrue(found("togetherSwitch").property("checked"))
        self.assertTrue(found("muteSwitch").property("checked"))
        self.assertTrue(found("inviteButton").property("visible"))
        self.assertFalse(found("openButton").property("visible"))
        found("inviteButton").clicked.emit()
        self.assertTrue(self.until(lambda: found("codeLabel").property("text") == "ABCD2345" and found("codeLabel").property("visible")))
        found("muteSwitch").toggle()
        found("muteSwitch").toggled.emit()
        self.assertTrue(self.until(lambda: self.voice.asked == ["mute"] and self.b.muted and not found("muteSwitch").property("checked")))
        self.together.mode, self.together.people, self.together.asks = "idle", [], []
        self.assertTrue(self.until(lambda: found("openButton").property("visible") and not found("inviteButton").property("visible")))
        found("joinAddress").setProperty("text", "ben-frame")
        found("joinCode").setProperty("text", "WXYZ6789")
        found("joinButton").clicked.emit()
        self.assertTrue(self.until(lambda: found("cancelButton").property("visible")))
        self.assertEqual(self.together.asked[-1], "join ben-frame 47193 WXYZ6789")
        self.assertEqual([w for w in warnings if "not placed in the graphics scene" not in w], [])


if __name__ == "__main__":
    unittest.main()
