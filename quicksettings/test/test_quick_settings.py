"""Frametop Settings' Quick Settings page (ft_quick_settings.py, palm_tiles.py) and the palm menu's file
(conf/ft_palm_menu.py): the card (screens/palmtiles.h) and the relay agree with them, and each edit on
the page writes what it should. No Qt window or VR needed."""
import ast
import os
import re
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
for folder in ("quicksettings", "conf", "commands", "input"):
    sys.path.insert(0, str(ROOT / folder))
import ft_conf  # noqa: E402
import ft_palm_menu as palm  # noqa: E402
import ft_quick_settings as qs  # noqa: E402
import ft_store  # noqa: E402
import game_pause  # noqa: E402
import palm_tiles  # noqa: E402
from ft_input_actions import ACTION_LABELS  # noqa: E402

CARD = (ROOT / "screens/palmtiles.h").read_text()
RELAY = (ROOT / "input/input-relay.py").read_text()


def c_strings(text):
    return [ast.literal_eval('"' + s + '"') for s in re.findall(r'"((?:[^"\\]|\\.)*)"', text)]


class OneCatalogue(unittest.TestCase):
    def test_the_card_has_the_same_defaults(self):
        block = re.search(r"kDefault =\n(.*?);\n", CARD, re.S).group(1)
        self.assertEqual("".join(c_strings(block)), palm.DEFAULT)

    def test_the_card_fits_as_many(self):
        tiles = (ROOT / "screens/quickaccess.h").read_text()
        cols, rows = (int(re.search(rf"\b{k} = (\d+)", tiles).group(1)) for k in ("kCols", "kRows"))
        self.assertEqual({palm.MAX["menu"], palm.MAX["quick"]}, {cols * rows})
        self.assertEqual(int(re.search(r"kMax = (\d+)", CARD).group(1)), cols * rows)

    def test_the_card_knows_what_works_paused(self):
        block = re.search(r"kPausedActions\[\] = \{(.*?)\};", CARD, re.S).group(1)
        self.assertEqual(set(c_strings(block)), set(game_pause.PAUSED_ACTIONS))

    def test_every_relay_action_is_offered_or_left_out(self):
        block = re.search(r"^ACTIONS = \((.*?)\)", RELAY, re.S | re.M).group(1)
        actions = set(re.findall(r'"([a-z_]+)"', block))
        offered = {a for a, _ in palm.CATALOGUE}
        self.assertLessEqual(offered, set(ACTION_LABELS))
        self.assertFalse(offered & palm.LEFT_OUT)
        self.assertFalse({a for a in actions if a in ACTION_LABELS} - offered - palm.LEFT_OUT)

    def test_defaults_are_offered_under_their_own_words(self):
        for e in palm.parse(palm.DEFAULT)[0]:
            self.assertEqual(e["label"], palm_tiles.suggested(e["action"], e["page"]))


class File(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.path = os.path.join(tmp.name, "frametop-palm.txt")
        self.conf = os.path.join(tmp.name, "frametop.conf")

    def test_round_trip(self):
        entries = palm.parse(palm.DEFAULT)[0]
        self.assertEqual(palm.parse(palm.text_of(entries)), (entries, []))

    def test_bad_lines_are_left_out(self):
        entries, problems = palm.parse("# mine\n\nmenu\t1\tdictate\tTalk\nmenu\t2\tdictate\tx\nnope\t1\ta\tb\nmenu\t1\tdictate\n")
        self.assertEqual([e["label"] for e in palm.of(entries, "menu")], ["Talk"])
        self.assertEqual(len(problems), 3)
        self.assertEqual(len(palm.of(entries, "eyes")), 1)   # (it's always there)

    def test_a_bad_file_is_not_saved_over(self):
        Path(self.path).write_text("rubbish\n")
        with self.assertRaises(ft_store.StoreError):
            palm.update(lambda e: e, self.path, self.conf)
        self.assertEqual(Path(self.path).read_text(), "rubbish\n")
        palm.restore(self.path)
        self.assertEqual(palm.load(self.path, self.conf), palm.parse(palm.DEFAULT)[0])
        kept = [p for p in Path(self.path).parent.iterdir() if p.name != "frametop-palm.txt" and p.read_text() == "rubbish\n"]
        self.assertEqual(len(kept), 1)

    def test_fields_are_trimmed_as_the_card_trims_them(self):
        e = palm.parse_line("menu\t1\tdictate\t \u00a0Talk \r")
        self.assertEqual(e["label"], "\u00a0Talk")
        self.assertIsNone(palm.parse_line("  # a note"))
        self.assertEqual(len(palm.parse_line("menu\t1\tdictate\t" + "x" * 900)["label"]), palm.LABEL_MAX)
        self.assertEqual(int(re.search(r"kLabelMax = (\d+)", CARD).group(1)), palm.LABEL_MAX)

    def test_restore_needs_its_copy(self):
        Path(self.path).write_text("rubbish\n")
        real, ft_store.backup = ft_store.backup, lambda path: None
        self.addCleanup(setattr, ft_store, "backup", real)
        with self.assertRaises(ft_store.StoreError):
            palm.restore(self.path)
        self.assertEqual(Path(self.path).read_text(), "rubbish\n")

    def test_the_relay_takes_only_what_shows(self):
        lines = [f"menu\t1\trun:c{i}\tC{i}\n" for i in range(8)] + ["menu\t0\tdictate\tTalk\n", "quick\t1\tfrunner\tFind\n"]
        Path(self.path).write_text("".join(lines))
        shows = palm.actions(palm.load(self.path, self.conf))
        self.assertEqual(shows, {f"run:c{i}" for i in range(6)} | {"frunner", "gaze_quickcal"})

    def test_a_gone_command_takes_its_tiles(self):
        self.assertEqual(palm.drop_action("run:x", self.path), 0)
        self.assertFalse(os.path.exists(self.path))
        Path(self.path).write_text("menu\t1\trun:x\tX\nquick\t0\trun:x\tX\nmenu\t1\trun:xy\tXY\n")
        self.assertEqual(palm.drop_action("run:x", self.path), 2)
        self.assertEqual([e["action"] for e in palm.of(palm.load(self.path), "menu")], ["run:xy"])
        Path(self.path).write_text("eyes\t0\trun:x\tMine\n")
        self.assertEqual(palm.drop_action("run:x", self.path), 1)
        self.assertEqual(palm.of(palm.load(self.path), "eyes"),
                         [palm.Entry("eyes", False, "gaze_quickcal", "Quick Eye Calibrate")])


class Page(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.conf = os.path.join(tmp.name, "frametop.conf")
        self.path = os.path.join(tmp.name, "frametop-palm.txt")
        Path(self.conf).write_text("POINTER=1   # kept\n")
        self.b = qs.Backend(conf=self.conf, path=self.path)
        self.said = []
        self.b.message.connect(lambda text, bad: self.said.append((text, bad)))

    def group(self, page):
        return next(g for g in self.b.groups if g["page"] == page)

    def tiles(self, page):
        return [(t["label"], t["action"], t["on"]) for t in self.group(page)["tiles"]]

    def test_no_file_is_the_defaults(self):
        self.assertEqual([t[0] for t in self.tiles("menu")],
                         ["Steam Menu", "Hide Screens", "Reset Layout…", "Spin Left", "Spin Right", "Dock Windows"])
        self.assertEqual(self.tiles("eyes"), [("Quick Eye Calibrate", "gaze_quickcal", True)])
        self.assertFalse(os.path.exists(self.path))

    def test_the_old_pick_is_kept(self):
        ft_conf.set_value("QUICK_SETTINGS", "bogus,hands_toggle", self.conf)
        self.assertEqual([t[1] for t in self.tiles("quick") if t[2]], ["hands_toggle"])
        self.b.setShown("quick", 5, True)   # the first edit writes the file, that pick in it
        self.assertEqual([t[1] for t in self.tiles("quick") if t[2]], ["hands_toggle", "dictate"])
        ft_conf.set_value("QUICK_SETTINGS", "", self.conf)
        self.assertEqual([t[1] for t in self.tiles("quick") if t[2]], ["hands_toggle", "dictate"])

    def test_hiding_and_showing(self):
        self.b.setShown("menu", 1, False)
        self.assertEqual(self.group("menu")["shown"], 5)
        self.assertFalse(self.tiles("menu")[1][2])
        self.b.setShown("eyes", 0, False)
        self.assertNotIn("gaze_quickcal", palm.actions(palm.load(self.path)))
        self.b.setShown("menu", 1, True)
        self.assertEqual(self.said, [])

    def test_no_seventh(self):
        self.b.add("menu", "dictate")
        self.assertEqual(self.tiles("menu")[6], ("Dictation", "dictate", False))
        self.b.setShown("menu", 6, True)
        self.assertFalse(self.tiles("menu")[6][2])
        self.assertEqual(len(self.said), 2)
        self.b.setShown("menu", 0, False)
        self.b.setShown("menu", 6, True)
        self.assertTrue(self.tiles("menu")[6][2])

    def test_its_words(self):
        self.b.setLabel("menu", 0, "  Big\tPicture\n ")
        self.assertEqual(self.tiles("menu")[0][0], "Big Picture")
        self.b.setLabel("menu", 0, "x" * 90)
        self.assertEqual(len(self.tiles("menu")[0][0]), palm.LABEL_MAX)
        self.b.setLabel("menu", 0, "   ")
        self.assertEqual(self.tiles("menu")[0][0], "Steam Menu")
        self.b.setLabel("menu", 0, "#1")   # (a label can't turn its line into a comment)
        self.assertEqual(self.tiles("menu")[0][0], "#1")

    def test_what_it_runs(self):
        self.b.setAction("menu", 3, "run:stop-framecorder")   # words nobody changed follow it
        self.assertEqual(self.tiles("menu")[3], ("Stop Framecorder", "run:stop-framecorder", True))
        self.b.setLabel("menu", 3, "Stop it")
        self.b.setAction("menu", 3, "profile:Work desk")      # and yours stay
        self.assertEqual(self.tiles("menu")[3], ("Stop it", "profile:Work desk", True))
        self.assertIn("profile:Work desk", palm.actions(palm.load(self.path)))
        self.b.setAction("menu", 3, "")
        self.b.setAction("menu", 3, "run:a\tb")
        self.assertNotIn("\t\t", Path(self.path).read_text())

    def test_order_and_taking_out(self):
        self.b.move("menu", 0, 1)
        self.b.move("menu", 0, -1)   # (at the top already)
        self.assertEqual([t[0] for t in self.tiles("menu")[:2]], ["Hide Screens", "Steam Menu"])
        self.b.remove("menu", 0)
        self.assertEqual([t[0] for t in self.tiles("menu")[:2]], ["Steam Menu", "Reset Layout…"])
        self.b.remove("eyes", 0)     # the eye row only hides
        self.assertEqual(len(self.tiles("eyes")), 1)
        self.b.remove("menu", 40)
        self.assertEqual(len(self.tiles("menu")), 5)
        for _ in range(5):
            self.b.remove("menu", 0)
        self.assertEqual(self.tiles("menu"), [])
        self.b.restoreDefaults()
        self.assertEqual(len(self.tiles("menu")), 6)

    def test_a_file_that_does_not_parse_says_so(self):
        Path(self.path).write_text("rubbish\n")
        self.b.setShown("menu", 0, False)
        self.b.add("menu", "dictate")
        self.assertEqual([bad for _, bad in self.said], [True, True])
        self.assertEqual(Path(self.path).read_text(), "rubbish\n")


    def test_a_file_changed_elsewhere_is_not_edited_by_place(self):
        self.assertEqual(len(self.b.groups), 3)
        palm.update(lambda e: palm_tiles.remove(e, "menu", 0), self.path, self.conf)
        self.b.setLabel("menu", 1, "Mine")   # (the page's second tile is the file's first now)
        self.assertNotIn("Mine", Path(self.path).read_text())
        self.assertEqual(len(self.said), 1)
        self.assertEqual(self.tiles("menu")[0][0], "Hide Screens")   # (the page reads it again)
        self.b.setLabel("menu", 0, "Mine")
        self.assertEqual(self.tiles("menu")[0][0], "Mine")


class Choices(unittest.TestCase):
    def test_commands_native_and_profiles(self):
        listed = palm_tiles.choices(["gonk"], ["Desk"])
        self.assertEqual(listed[0], {"value": "run:gonk", "text": "Run command gonk"})
        self.assertEqual(listed[-1], {"value": "profile:Desk", "text": "Open profile Desk"})
        self.assertEqual(len(listed), len(palm.CATALOGUE) + 2)

    def test_what_a_tile_has_is_always_listed(self):
        listed = palm_tiles.choices([], [])
        entries = [palm.Entry("menu", True, a, "x") for a in ("run:gone", "dictate", "run:gone", "keys:125+32")]
        extra = palm_tiles.with_missing(listed, entries)[len(listed):]
        self.assertEqual(extra, [{"value": "keys:125+32", "text": "keys:125+32"},
                                 {"value": "run:gone", "text": "run:gone (not available)"}])

    def test_profiles_read(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "layout.json")
            self.assertEqual(palm_tiles.profiles(path), [])
            Path(path).write_text('{"layouts": {"b": {}, "A": {}}}')
            self.assertEqual(palm_tiles.profiles(path), ["A", "b"])
            Path(path).write_text("[1")
            self.assertEqual(palm_tiles.profiles(path), [])


if __name__ == "__main__":
    unittest.main()
