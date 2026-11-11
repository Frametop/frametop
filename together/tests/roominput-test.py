#!/usr/bin/env python3
"""ft-roominput next to a real ft-together in a room with another, with stand-ins for
ft-roompanels and ft-screens: what someone who was given a panel does reaches ft-screens as
"peer" commands on the screen the panel is, and what they hold is let go when the wearer's own
hand is on the desktop, when the panel is taken back and when ft-together goes.

    roominput-test.py FT-TOGETHER

ft-screens' side of it (what a "peer" command does on the seat) is screens/tests/peerinput-test.c.
"""
import json
import os
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import roomkit  # noqa: E402
from fakescreens import serve  # noqa: E402
from roomkit import check  # noqa: E402

SCREEN = "1920 1080 0 1.5 -2 1 0 0 0 2 0 Screen"


class Screens:
    """ft-screens, as far as ft-roominput tells it: it writes down what it hears and answers nothing."""

    def __init__(self):
        self.heard = []

    def __call__(self, text):
        self.heard.append(text)

    def acts(self, since=0):
        return [t for t in self.heard[since:] if t != "peer on"]

    def until(self, what, seconds=5.0):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if what():
                return True
            time.sleep(0.05)
        return what()


class Panels:
    """ft-roompanels, as far as ft-roominput asks it: which screen each shared panel is."""

    def __init__(self):
        self.shares = {0: 2}   # panel number -> screen

    def __call__(self, text):
        if text != "state":
            raise AssertionError(f"ft-roompanels was asked {text!r}")
        return "ok " + json.dumps({"room": True, "floor": None, "panels": [], "shares": [{"screen": s, "panel": k, "told": True} for k, s in sorted(self.shares.items())]})


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip())
    ada, ben = roomkit.End(sys.argv[1], "ada"), roomkit.End(sys.argv[1], "ben")
    screens, panels = Screens(), Panels()
    names = ["ft_test_%d_%s" % (os.getpid(), n) for n in ("screens", "panels")]
    serve(screens, names[0])
    serve(panels, names[1])
    argv = [str(HERE.parent / "ft-roominput"), "--together", ada.sock, "--panels", names[1], "--screens", names[0]]
    tool, second = roomkit.Tool(argv), None

    def act(panel, words):
        return ben.ask("control act %d %s" % (panel, words))

    def give(number, who):
        """ada, said to be there, gives ben her panel `number`; True once his end has it."""
        ok = ada.ask("control here") == "ok" and ada.ask("control give %d %d" % (number, who)) == "ok"
        return ok and ben.until(lambda _: any(h["panel"] % 8 == number for h in json.loads(ben.ask("control")[3:])["held"]))

    try:
        ok = all(e.until(lambda s: s["mode"] == "idle") for e in (ada, ben))
        ok = ok and ada.ask("open") == "ok" and roomkit.let_in(ada, ben) and ada.until(roomkit.linked) and ben.until(roomkit.linked)
        if not check("ada opens a room and ben comes in", ok):
            return
        who = {p["name"]: p["id"] for p in ada.state()["people"]}
        panel = who["ada"] * 8
        check("ada shares a screen", ada.ask("share 0 " + SCREEN) == "ok" and ben.until(lambda s: [p["id"] for p in s["panels"]] == [panel]))
        check("it takes the input for ada's desktop, and says nothing to ft-screens while nobody has a panel",
              tool.said("taking the input") and tool.said("nobody has a panel of this desktop") and screens.heard == [])
        second = roomkit.Tool(argv)
        check("a second one is told no by ft-together", second.said("ft-together: error another program takes the input"))
        check("...and ends cleanly", second.end() == 0)
        screens.heard.clear()   # (its "peer letgo" as it ended)

        check("ben is given the panel: ft-screens hears that someone may be at the desktop", give(0, who["ben"]) and tool.said("someone has a panel of this desktop") and
              screens.until(lambda: "peer on" in screens.heard))
        sent = ["move 30000 65535", "button 1 1", "button 1 0", "key 30 1", "key 30 0", "scroll 0 -120"]
        ok = all(act(panel, words) == "ok" for words in sent)
        check("what ben does on it reaches ft-screens in order, on the screen the panel is", ok and screens.until(lambda: len(screens.acts()) >= len(sent)) and
              screens.acts() == ["peer 2 " + words for words in sent])
        began = len(screens.heard)
        check("...and it keeps hearing that someone may be", screens.until(lambda: screens.heard[began:].count("peer on") >= 2, 4))

        # the wearer's own hand, as ft-screens says it
        began = len(screens.heard)
        ok = act(panel, "button 1 1") == "ok" and screens.until(lambda: screens.acts(began) == ["peer 2 button 1 1"])
        ok = ok and ada.ask("control own") == "ok" and screens.until(lambda: screens.acts(began) == ["peer 2 button 1 1", "peer 2 button 1 0"])
        check("ada's own hand on her desktop: the button ben holds is let go", ok)
        ok = act(panel, "move 5 5") == "ok"
        time.sleep(0.4)
        check("...and for a moment nothing of his is passed on", ok and screens.acts(began) == ["peer 2 button 1 1", "peer 2 button 1 0"])
        time.sleep(1.3)
        check("...then it is again", act(panel, "move 6 6") == "ok" and screens.until(lambda: screens.acts(began)[2:] == ["peer 2 move 6 6"]))

        # a panel ft-roompanels doesn't know, and then does
        began = len(screens.heard)
        ok = ada.ask("share 1 " + SCREEN) == "ok" and ben.until(lambda s: len(s["panels"]) == 2) and give(1, who["ben"])
        ok = ok and act(panel + 1, "move 1 1") == "ok"
        time.sleep(0.5)
        check("input on a panel that is no screen of the desktop goes nowhere", ok and screens.acts(began) == [])
        panels.shares[1] = 1
        time.sleep(0.3)
        check("...and once it is one, it goes there", act(panel + 1, "move 2 2") == "ok" and screens.until(lambda: screens.acts(began) == ["peer 1 move 2 2"]))

        # taken back with a key down
        began, lines = len(screens.heard), len(tool.lines)
        ok = act(panel, "key 42 1") == "ok" and screens.until(lambda: screens.acts(began) == ["peer 2 key 42 1"])
        ok = ok and ada.ask("control take 0") == "ok" and ada.ask("control take 1") == "ok"
        check("taken back while ben holds a key: it comes up, and then everything is let go", ok and
              screens.until(lambda: screens.acts(began) == ["peer 2 key 42 1", "peer 2 key 42 0", "peer letgo"]) and tool.said("nobody has a panel of this desktop", since=lines))
        began = len(screens.heard)
        time.sleep(1.5)
        check("...and ft-screens hears no more that someone may be at the desktop", screens.heard[began:] == [])

        # ft-together goes with a button down
        began = len(screens.heard)
        ok = give(0, who["ben"]) and act(panel, "button 3 1") == "ok" and screens.until(lambda: screens.acts(began) == ["peer 2 button 3 1"])
        ok = ok and screens.until(lambda: "peer on" in screens.heard[began:], 3)   # (it has looked: it knows someone has a panel)
        status, out = ada.end()
        check("ada's ft-together goes while ben holds a button: everything is let go", ok and status == 0 and "ERROR: " not in out and
              screens.until(lambda: screens.acts(began)[-1:] == ["peer letgo"], 4))
        began = len(screens.heard)
        check("told to end, it lets go once more and ends cleanly", tool.end() == 0 and not any("Traceback" in line for line in tool.lines) and
              screens.until(lambda: screens.heard[began:] == ["peer letgo"], 2))
    finally:
        tool.end()
        if second:
            second.end()
        ends = [ada.end(), ben.end()]
    check("both ft-together end cleanly, with nothing from the sanitizers", all(status == 0 and "ERROR: " not in out for status, out in ends))
    roomkit.finish()


if __name__ == "__main__":
    main()
