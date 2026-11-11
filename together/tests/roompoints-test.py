#!/usr/bin/env python3
"""ft-roompoints at one of three real ft-together on this machine's loopback, with a stand-in for
its Frame's ft-roompanels: where the other two point comes to it as theirs, on the panel this
Frame shows, and goes when they point away, fall silent, or the panel isn't shown here any more.
It runs with --probe: nothing is drawn, and what would be is read from its `state`.

    roompoints-test.py FT-TOGETHER FT-ROOMPOINTS
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
from roomkit import Watcher, check, point  # noqa: E402

SCREEN = "1920 1080 0 1.5 -2 1 0 0 0 2 0 Screen"


class Panels:
    """ft-roompanels, as far as ft-roompoints asks it."""

    def __init__(self):
        self.said = "ok 0"   # None: it says nothing

    def __call__(self, text):
        return self.said if text == "panels" else "error share N | unshare N | state | panels"


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip())
    ada, ben, cy = (roomkit.End(sys.argv[1], name) for name in ("ada", "ben", "cy"))
    name = "ft_test_%d_points" % os.getpid()
    panels = Panels()
    lock = serve(panels, name + "_panels")
    argv = [sys.argv[2], "--probe", "--socket", name, "--together", ada.sock, "--panels", name + "_panels"]
    tools, watchers = [], []

    def tool():
        tools.append(roomkit.Tool(argv))
        return tools[-1]

    def program(end):
        watchers.append(Watcher(end))
        return watchers[-1]

    def state():
        reply = ada.client.ask(name, "state") or "none"
        return json.loads(reply[3:]) if reply.startswith("ok {") else {"panels": -1, "heard": -1, "marks": [], "why": reply}

    def marks():
        return {m["person"]: m for m in state()["marks"]}

    def until(what, seconds=5.0):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if what():
                return True
            time.sleep(0.05)
        return False

    def says(text):
        with lock:
            panels.said = text

    try:
        ok = all(e.until(lambda s: s["mode"] == "idle") for e in (ada, ben, cy))
        ok = ok and ada.ask("open") == "ok" and roomkit.let_in(ada, ben) and roomkit.let_in(ada, cy)
        ok = ok and all(e.until(lambda s: len(s["people"]) == 3 and roomkit.linked(s)) for e in (ada, ben, cy))
        if not check("three of them are in ada's room", ok):
            return
        who = {p["name"]: p["id"] for p in ada.state()["people"]}
        mine, bens, other = who["ada"] * 8, who["ben"] * 8, who["cy"] * 8 + 1
        ok = ada.ask("share 0 " + SCREEN) == "ok" and ben.ask("share 0 " + SCREEN) == "ok"
        check("ada and ben each share a panel", ok and ada.until(lambda s: sorted(p["id"] for p in s["panels"]) == sorted([mine, bens])))

        points = tool()
        check("ft-roompoints starts with no panel known and nobody pointing", until(lambda: state() == {"panels": 0, "heard": 0, "marks": []}))
        second = roomkit.Tool(argv)
        check("a second one is told it runs already, and ends", second.wait(5) == 1 and second.said("already running", 1))
        check("it answers nothing but its own questions", ada.client.ask(name, "draw 1 2") == "error state|quit")

        says("ok 2 %d:1:3440x1440 %d:116:1920x1080" % (mine, bens))
        check("it hears from ft-roompanels which panels this Frame shows", until(lambda: state()["panels"] == 2))

        hand, hers, own = program(ben), program(cy), program(ada)
        hand.send(point(mine, 16384, 49151))
        got = {}
        ok = until(lambda: got.update(marks()) or who["ben"] in got)
        m = got.get(who["ben"], {})
        check("where ben points on ada's screen is a mark there, his",
              ok and list(got) == [who["ben"]] and (m["panel"], m["key"], m["pressed"], m["drawn"]) == (mine, "frametop.screen.1", False, True))
        check("...at the same part of the picture", ok and abs(m["x"] - 0.25) < 1e-3 and abs(m["y"] - 0.75) < 1e-3)

        hers.send(point(bens, 65535, 0, pressed=True))
        ok = until(lambda: who["cy"] in marks())
        got = marks()
        m = got.get(who["cy"], {})
        check("cy points at ben's panel with a button held: a second mark, hers, on the remote screen that shows it",
              ok and sorted(got) == sorted([who["ben"], who["cy"]]) and (m["panel"], m["key"], m["pressed"], m["drawn"]) == (bens, "frametop.remote.116", True, True) and
              abs(m["x"] - 1) < 1e-3 and m["y"] < 1e-3)
        hand.send(point(mine, 32768, 32768))
        check("ben's moves when his pointer does, and hers stays", until(lambda: abs(marks().get(who["ben"], {}).get("x", 0) - 0.5) < 1e-3) and abs(marks()[who["cy"]]["x"] - 1) < 1e-3)
        own.send(point(bens, 100, 100))
        time.sleep(0.4)
        check("ada's own pointer is no mark at ada's", who["ada"] not in marks() and state()["heard"] == 3)

        hers.send(point(other, 500, 500))
        check("pointing at a panel this Frame doesn't show is told, and not drawn",
              until(lambda: marks().get(who["cy"], {}).get("panel") == other) and (marks()[who["cy"]]["key"], marks()[who["cy"]]["drawn"]) == ("", False))
        hand.send(point(255, 0, 0))
        check("ben points at nothing: his mark goes at once", until(lambda: who["ben"] not in marks(), 1.0))

        hers.send(point(bens, 500, 500))
        said = time.monotonic()
        ok = until(lambda: marks().get(who["cy"], {}).get("drawn") is True, 1.0)
        gone = until(lambda: who["cy"] not in marks(), 5.0)
        took = time.monotonic() - said
        check("cy says no more: her mark stands a while, then goes (%.1f s)" % took, ok and gone and 2.0 < took < 4.0)

        # ft-roompanels stops answering, then says something that isn't an answer
        says(None)
        deadline, lost = time.monotonic() + 6, False
        while time.monotonic() < deadline and not lost:
            hand.send(point(mine, 1000, 1000))
            time.sleep(0.25)
            lost = state()["panels"] == 0
        m = marks().get(who["ben"], {})
        check("when ft-roompanels stops answering, no panel is known, and a pointer isn't drawn", lost and (m.get("panel"), m.get("key"), m.get("drawn")) == (mine, "", False))
        says("ok 1 %d:2:1920x1080" % mine)
        hand.send(point(mine, 1000, 1000))
        check("it answers again, the panel on another screen now: the mark is there", until(lambda: (marks().get(who["ben"], {}).get("key"), marks()[who["ben"]]["drawn"]) == ("frametop.screen.2", True)))
        says("ok 1 %d:2:1920x1080 junk" % mine)
        time.sleep(1.2)
        hand.send(point(mine, 2000, 1000))
        check("an answer that isn't one changes nothing", until(lambda: abs(marks().get(who["ben"], {}).get("x", 0) - 2000 / 65535) < 1e-3) and marks()[who["ben"]]["key"] == "frametop.screen.2")

        check("asked to quit, it ends", ada.client.ask(name, "quit") == "ok" and points.wait(5) == 0)
        again = tool()
        check("and one started after it has the name", until(lambda: state()["heard"] == 0 and state()["panels"] >= 0))
        check("it ends when told to", again.end() == 0)
    finally:
        for t in tools:
            t.end()
        for w in watchers:
            w.close()
        for e in (ada, ben, cy):
            e.end()
    roomkit.finish()


if __name__ == "__main__":
    main()
