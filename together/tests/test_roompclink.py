"""Its person's PC asked to send its windows, the rules (together/roompanels/pcwindows.py,
pclink.py): which panels of the saved layout are PC windows, and what the PC's streaming host
is asked and when, against stand-ins for ft-together and the PC.

    python3 together/tests/test_roompclink.py
"""
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from roompanels import pclink, pcwindows  # noqa: E402
from roompanels.pcwindows import Window  # noqa: E402

PC, OTHER = ("100.64.0.7", 48989), ("100.64.0.9", 47989)
CALC = Window("100.64.0.7", 48989, "0x55aa10", "frametop-pc-win1", "Calculator")
NOTES = Window("100.64.0.7", 48989, "0x55aa20", "frametop-pc-win2", "Notes")
ELSEWHERE = Window("100.64.0.9", 47989, "0x1f0a", "frametop-laptop-win1", "Paint")


def display(screen, window, **more):
    return {"id": f"pc-window-{screen}", "client": window.client, "app": "window:" + window.window, "label": window.label,
            "size": [1280, 720], "port": window.port, "window": True, "screen": screen, **more}


class Windows(unittest.TestCase):
    def layout(self, *displays):
        return {"preset": {}, "hosts": [{"name": "pc", "address": "100.64.0.7", "windows": {"port": 48989},
                                         "displays": [{"id": "pc-main", "client": "frametop-pc-main", "app": "display:DP-1", "screen": 101}, *displays]}]}

    def test_the_layouts_window_panels_are_found_by_their_screens(self):
        found = pcwindows.windows(self.layout(display(102, CALC), display(105, NOTES)))
        self.assertEqual(found, {102: CALC, 105: NOTES})

    def test_a_whole_display_and_a_window_that_is_off_are_none(self):
        self.assertEqual(pcwindows.windows(self.layout(display(102, CALC, off=True))), {})

    def test_what_doesnt_read_as_a_window_panel_is_left_out(self):
        good = display(102, CALC)
        for key, value in (("screen", 100), ("screen", 117), ("screen", "102"), ("screen", True), ("port", 0), ("port", 65535), ("port", "48989"),
                           ("app", "window:"), ("app", "display:FT-W1"), ("app", 7), ("app", "window:" + "0" * 80), ("client", ""), ("client", None),
                           ("window", "yes"), ("window", 1)):
            self.assertEqual(pcwindows.windows(self.layout({**good, key: value})), {}, (key, value))
        self.assertEqual(pcwindows.windows(self.layout({**good, "label": 5})), {102: CALC._replace(label="")})
        for layout in (None, [], {}, {"hosts": 3}, {"hosts": [3]}, {"hosts": [{"address": "100.64.0.7", "displays": 3}]},
                       {"hosts": [{"address": "100.64.0.7", "displays": [3]}]}, {"hosts": [{"address": 7, "displays": [good]}]},
                       {"hosts": [{"displays": [good]}]}):
            self.assertEqual(pcwindows.windows(layout), {}, layout)

    def test_the_file_is_read_again_only_when_it_changed(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "layout.json")
            read = pcwindows.reader(path)
            self.assertEqual(read(), {})                      # no file: no windows
            with open(path, "w") as f:
                json.dump(self.layout(display(102, CALC)), f)
            self.assertEqual(read(), {102: CALC})
            first = read()
            self.assertIs(read(), first)                      # (the same answer, not the file again)
            with open(path, "w") as f:
                f.write("{ not json")
            os.utime(path, ns=(1, 1))
            self.assertIsNone(read())                         # it doesn't read
            with open(path, "w") as f:
                json.dump(self.layout(display(105, NOTES)), f)
            os.utime(path, ns=(2, 2))
            self.assertEqual(read(), {105: NOTES})


class Room:
    """ft-together, as far as the PC's side asks it."""

    def __init__(self):
        self.pc_here = False
        self.here = "ok 100.64.0.1 47990 ada"
        self.codes = 0
        self.media = None    # what "media" answers, when it isn't the usual
        self.code = False    # ... and "sender"
        self.asked = []

    def __call__(self, text):
        self.asked.append(text)
        if text == "media":
            return self.media if self.media is not None else "ok " + json.dumps({"sources": [], "sinks": [], "sender": self.pc_here})
        if text == "here":
            return self.here
        if text == "sender":
            self.codes += 1
            return f"ok CODE{self.codes:04d}" if self.code is False else self.code
        raise AssertionError(f"ft-together was asked {text!r}")


class Pc:
    """The streaming hosts of the PCs, as far as they are asked: each takes a call, and has
    the windows in `out` out on outputs."""

    def __init__(self, room):
        self.room = room
        self.out = {"0x55aa10": (1280, 720), "0x55aa20": (1920, 1080)}
        self.calling = set()     # the PCs with a call
        self.asked = []          # (pc, path, body)
        self.answers = {}        # path -> (status, answer), in place of the usual
        self.panels = {}         # pc -> its panels, as it answers them
        self.meanwhile = None    # what happens while it is asked for its panels, once

    def __call__(self, pc, path, body):
        self.asked.append((pc, path, body))
        if path in self.answers:
            return self.answers[path]
        if path == pclink.SENDER:
            self.calling.add(pc)
            self.room.pc_here = True
            return 200, self.state(pc)
        if path == pclink.STOP:
            self.calling.discard(pc)
            self.panels.pop(pc, None)
            self.room.pc_here = False
            return 200, self.state(pc)
        if self.meanwhile:
            self.meanwhile()
            self.meanwhile = None
        if pc not in self.calling:
            return 409, {"error": "no call to a headset stands"}
        self.panels[pc] = [{"number": p["number"], "output": "FT-W1", "width": self.out[p["address"]][0], "height": self.out[p["address"]][1],
                            "watched": False, "encoding": False} for p in body["panels"] if p["address"] in self.out]
        return 200, self.state(pc)

    def state(self, pc):
        return {"status": True, "calling": pc in self.calling, "called": False, "ended": "", "panels": self.panels.get(pc, [])}

    def paths(self, pc=PC):
        return [path.rsplit("/", 1)[1] for at, path, _ in self.asked if at == pc]


class LinkCase(unittest.TestCase):
    def setUp(self):
        self.room, self.clock = Room(), 50.0
        self.pc = Pc(self.room)
        self.link = pclink.PcLink(self.room, self.pc, lambda: self.clock)

    def steps(self, count=1):
        for _ in range(count):
            self.link.step()
            self.clock += pclink.STEP


class Link(LinkCase):
    def test_with_nothing_wanted_nobody_is_asked(self):
        self.steps(3)
        self.assertEqual((self.room.asked, self.pc.asked, self.link.sizes(), self.link.why()), ([], [], {}, ""))

    def test_the_pc_is_called_in_with_where_this_frame_is_and_a_code_then_told_its_panels(self):
        self.link.want({3: CALC, 4: NOTES})
        self.steps()
        self.assertEqual(self.room.asked, ["media", "here", "sender"])
        self.assertEqual(self.pc.asked, [
            (PC, pclink.SENDER, {"address": "100.64.0.1", "port": 47990, "code": "CODE0001"}),
            (PC, pclink.PANELS, {"panels": [{"number": 3, "address": "0x55aa10"}, {"number": 4, "address": "0x55aa20"}]})])
        self.assertEqual(self.link.sizes(), {3: (1280, 720), 4: (1920, 1080)})
        self.assertEqual(self.link.why(), "")

    def test_a_pc_that_is_here_is_only_told_its_panels(self):
        self.link.want({3: CALC})
        self.steps(4)
        self.assertEqual(self.pc.paths(), ["sender", "panels", "panels", "panels", "panels"])
        self.assertEqual(self.room.codes, 1)

    def test_a_call_that_doesnt_stand_is_made_again_but_not_at_once(self):
        self.link.want({3: CALC})
        self.steps()
        self.room.pc_here = False          # the link ended
        self.steps(2)                      # 2 s and 4 s after the call
        self.assertEqual(self.pc.paths().count("sender"), 1)
        self.assertEqual(self.pc.paths().count("panels"), 1)   # (and nothing is told a PC that isn't here)
        self.steps()                       # 6 s after
        self.assertEqual((self.pc.paths().count("sender"), self.room.codes), (2, 2))
        self.assertEqual(self.pc.asked[-2][2]["code"], "CODE0002")

    def test_a_pc_that_doesnt_take_the_call_is_told_no_panels_and_why_is_kept(self):
        self.link.want({3: CALC})
        for answer, why in (((0, None), "the PC doesn't answer"), ((403, {"error": "Forbidden"}), "the PC says: Forbidden"),
                            ((404, None), "the PC answers 404"), ((400, {"error": "a\x00b\n" + "x" * 300}), "the PC says: ab" + "x" * 118)):
            self.pc.answers[pclink.SENDER] = answer
            self.steps()
            self.clock += pclink.CALL_EVERY
            self.assertEqual((self.link.why(), self.pc.paths().count("panels"), self.link.sizes()), (why, 0, {}))
        del self.pc.answers[pclink.SENDER]
        self.steps()
        self.assertEqual((self.link.why(), self.link.sizes()), ("", {3: (1280, 720)}))

    def test_while_ft_together_says_nothing_the_pc_is_left_alone(self):
        self.link.want({3: CALC})
        for media in ("", "error busy", "ok {", "ok []", 'ok {"sender": 1}', 'ok {"sources": []}', 'no {"sender": false}'):
            self.room.media = media
            self.steps()
        self.assertEqual(self.pc.asked, [])
        self.room.media, self.room.asked = None, []
        for here in ("", "error none", "ok", "ok 100.64.0.1", "error 100.64.0.1 47990 ada", "ok frame.example 47990 ada", "ok 100.64.0.1 0 ada", "ok 100.64.0.1 65536 ada", "ok 100.64.0.1 port ada",
                     "ok 1 47990 ada", "ok " + "1" * 46 + " 47990 ada"):
            self.room.here = here
            self.steps()
            self.clock += pclink.CALL_EVERY
        self.assertEqual((self.pc.asked, self.room.codes), ([], 0))      # no code is made for a call that can't be told
        self.assertEqual(self.link.why(), "Frametop Together doesn't say where the PC is to call")

    def test_only_a_code_that_reads_as_one_is_handed_on(self):
        self.link.want({3: CALC})
        for said in (None, "error not now", "ok", "ok SHORT", "ok TOOLONG99", "ok CODE 0001", "ok CODÉ0001", "ok CODE-001", "ok CODE0001 more", "error CODE0001"):
            self.room.code = said
            self.steps()
            self.clock += pclink.CALL_EVERY
        self.assertEqual((self.pc.asked, self.room.codes), ([], 10))
        self.room.code = False
        self.steps()
        self.assertEqual(self.pc.asked[0][2]["code"], "CODE0011")

    def test_a_window_missing_twice_in_a_row_is_gone_and_said_once(self):
        self.link.want({3: CALC, 4: NOTES})
        self.steps()
        del self.pc.out["0x55aa20"]
        self.steps()
        self.assertEqual((self.link.take_gone(), self.link.sizes()), (set(), {3: (1280, 720)}))    # once: it may yet be back
        self.pc.out["0x55aa20"] = (1920, 1080)
        self.steps()
        del self.pc.out["0x55aa20"]
        self.steps()
        self.assertEqual(self.link.take_gone(), set())                                             # (not in a row)
        self.steps()
        self.assertEqual((self.link.take_gone(), self.link.take_gone()), ({4}, set()))
        self.assertEqual(self.link.sizes(), {3: (1280, 720)})

    def test_a_panel_that_another_window_took_starts_over(self):
        self.link.want({3: NOTES})
        self.steps()
        del self.pc.out["0x55aa20"]
        self.steps()
        self.link.want({3: CALC})
        self.assertEqual(self.link.sizes(), {})                  # the size was the other window's
        del self.pc.out["0x55aa10"]
        self.steps()
        self.assertEqual(self.link.take_gone(), set())           # and so was the look it was missing at

    def test_an_answer_about_a_window_that_was_swapped_meanwhile_isnt_the_new_ones(self):
        self.link.want({3: CALC})
        self.pc.meanwhile = lambda: self.link.want({3: NOTES})
        self.steps()
        self.assertEqual(self.link.sizes(), {})
        self.steps()
        self.assertEqual(self.link.sizes(), {3: (1920, 1080)})

    def test_a_window_no_longer_wanted_isnt_said_to_be_gone(self):
        self.link.want({3: CALC, 4: NOTES})
        self.steps()
        del self.pc.out["0x55aa20"]
        self.steps(2)
        self.link.want({3: CALC})
        self.assertEqual(self.link.take_gone(), set())

    def test_only_a_size_a_room_takes_is_a_panels(self):
        self.link.want({3: CALC, 4: NOTES, 5: CALC})
        self.steps()
        for panels in ([{"number": 3, "width": 127, "height": 720}], [{"number": 3, "width": 1280, "height": 4097}], [{"number": 3, "width": 1280.0, "height": 720}],
                       [{"number": 3, "width": "1280", "height": 720}], [{"number": 3, "width": True, "height": 720}], [{"number": "3", "width": 1280, "height": 720}], [{"number": 3.0, "width": 1280, "height": 720}],
                       [{"number": 3}], [3], [None]):
            self.pc.answers[pclink.PANELS] = (200, {"calling": True, "panels": panels})
            self.steps()
            self.assertEqual(self.link.sizes(), {}, panels)
        self.pc.answers[pclink.PANELS] = (200, {"calling": True, "panels": [{"number": 3, "width": 128, "height": 4096}, {"number": 9, "width": 640, "height": 360}]})
        self.steps()
        self.assertEqual(self.link.sizes(), {3: (128, 4096)})    # (and a number that wasn't asked for is none of ours)

    def test_an_answer_that_isnt_one_changes_nothing_and_why_is_kept(self):
        self.link.want({3: CALC})
        self.steps()
        for answer, why in (((0, None), "the PC doesn't answer"), ((400, {"error": "no call to a headset stands"}), "the PC says: no call to a headset stands"),
                            ((200, None), "the PC answers 200"), ((200, {"panels": "none"}), "the PC answers 200"), ((200, []), "the PC answers 200"),
                            ((500, {"panels": []}), "the PC answers 500")):
            self.pc.answers[pclink.PANELS] = answer
            self.steps(3)
            self.assertEqual((self.link.why(), self.link.sizes(), self.link.take_gone()), (why, {3: (1280, 720)}, set()))
        del self.pc.answers[pclink.PANELS]
        self.steps()
        self.assertEqual((self.link.why(), self.pc.paths().count("sender")), ("", 1))

    def test_a_pc_with_no_call_while_another_is_here_is_called_in(self):
        self.room.pc_here = True               # another PC's link, or this one's from before its host started again
        self.link.want({3: CALC})
        self.steps()
        self.assertEqual((self.pc.paths(), self.link.why(), self.link.sizes()), (["panels"], "the PC says: no call to a headset stands", {}))
        self.steps()
        self.assertEqual((self.pc.paths(), self.link.why(), self.link.sizes()), (["panels", "sender", "panels"], "", {3: (1280, 720)}))
        self.pc.calling.clear()                # and once more, too soon after that call
        self.steps(2)                          # 2 s and 4 s after it
        self.assertEqual(self.pc.paths().count("sender"), 1)
        self.steps()
        self.assertEqual((self.pc.paths().count("sender"), self.link.sizes()), (2, {3: (1280, 720)}))

    def test_a_pc_that_was_called_in_and_took_its_panels_isnt_called_again(self):
        self.room.pc_here = True
        self.link.want({3: CALC})
        self.steps(7)
        self.assertEqual(self.pc.paths().count("sender"), 1)

    def test_one_pcs_no_call_isnt_anothers(self):
        self.room.pc_here = True
        self.link.want({3: CALC})
        self.steps()
        self.pc.calling.add(OTHER)
        self.pc.out["0x1f0a"] = (640, 480)
        self.link.want({3: ELSEWHERE})
        self.steps(2)
        self.assertEqual((self.pc.paths(PC), self.pc.paths(OTHER)), (["panels"], ["panels", "panels"]))

    def test_when_nothing_is_wanted_any_more_the_pc_is_told_to_stop_once(self):
        self.link.want({3: CALC})
        self.steps()
        self.link.want({})
        self.steps(3)
        self.assertEqual(self.pc.paths(), ["sender", "panels", "stop"])
        self.assertEqual(self.pc.asked[-1], (PC, pclink.STOP, {}))
        self.assertEqual(self.link.sizes(), {})

    def test_a_pc_that_was_called_but_took_no_panels_is_told_to_stop_and_nothing_holds_it_up_then(self):
        self.pc.answers[pclink.PANELS] = (0, None)
        self.link.want({3: CALC})
        self.steps()
        self.assertEqual(self.link.why(), "the PC doesn't answer")
        self.link.want({})
        self.steps()
        self.assertEqual((self.pc.paths(), self.link.why()), (["sender", "panels", "stop"], ""))

    def test_a_pc_that_was_sending_when_this_started_is_told_to_stop_too(self):
        self.pc.calling.add(PC)
        self.room.pc_here = True
        self.link.want({3: CALC})
        self.steps()
        self.link.want({})
        self.steps()
        self.assertEqual(self.pc.paths(), ["panels", "stop"])

    def test_another_pcs_windows_stop_the_first_and_call_the_other(self):
        self.pc.out["0x1f0a"] = (640, 480)
        self.link.want({3: CALC})
        self.steps()
        self.link.want({3: ELSEWHERE})
        self.steps()                       # (called at once: the wait is between calls to one PC)
        self.assertEqual((self.pc.paths(PC), self.pc.paths(OTHER)), (["sender", "panels", "stop"], ["sender", "panels"]))
        self.assertEqual(self.link.sizes(), {3: (640, 480)})

    def test_windows_of_two_pcs_at_once_are_asked_of_neither(self):
        self.link.want({3: CALC, 4: ELSEWHERE})
        self.steps(2)
        self.assertEqual(self.pc.asked, [])


if __name__ == "__main__":
    unittest.main()
