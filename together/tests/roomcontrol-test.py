#!/usr/bin/env python3
"""Control of shared panels through ft-together for real, three of it on this machine's
loopback: a panel asked for and given, the input that reaches the program that gives it to the
desktop and the input that doesn't, what is let go when a grant is taken back, when its holder
leaves and when nobody says the wearer is there any more, and everyone's pointers.

    roomcontrol-test.py FT-TOGETHER

The rules are tested without a network or a clock in control-test.cpp and
controlrules-test.cpp; this is what only the real program, its links and its clock show.
"""
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import roomkit  # noqa: E402
from roomkit import Watcher, act, check, point  # noqa: E402

MOVE, BUTTON, SCROLL, KEY = 1, 2, 3, 4
SCREEN = "1920 1080 0 1.5 -2 1 0 0 0 2 0 Screen"


def control(end):
    reply = end.ask("control")
    return json.loads(reply[3:]) if reply.startswith("ok {") else {"there": False, "asks": [], "given": [], "holder": -1, "held": [], "asked": [], "why": reply}


def until(what, seconds=5.0):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if what():
            return True
        time.sleep(0.05)
    return False


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip())
    ada, ben, cy = (roomkit.End(sys.argv[1], name) for name in ("ada", "ben", "cy"))
    watchers = []

    def program(end):
        watchers.append(Watcher(end))
        return watchers[-1]

    try:
        ok = all(e.until(lambda s: s["mode"] == "idle") for e in (ada, ben, cy))
        ok = ok and ada.ask("open") == "ok" and roomkit.let_in(ada, ben) and roomkit.let_in(ada, cy)
        ok = ok and all(e.until(lambda s: len(s["people"]) == 3 and roomkit.linked(s)) for e in (ada, ben, cy))
        if not check("three of them are in ada's room", ok):
            return
        check("each Frame says its wearer's hand moves their pointer", all(e.hand() for e in (ada, ben, cy)))
        who = {p["name"]: p["id"] for p in ada.state()["people"]}
        ok = ada.ask("share 0 " + SCREEN) == "ok" and all(e.until(lambda s: [p["id"] for p in s["panels"]] == [who["ada"] * 8]) for e in (ada, ben, cy))
        check("ada shares a panel", ok)
        panel = who["ada"] * 8

        # the program that gives the input to the desktop
        desk, second = program(ada), program(ada)
        check("one program takes the input for ada's desktop, and a second is told no",
              desk.ask("inputs") == "ok" and desk.ask("inputs") == "ok" and second.ask("inputs") == "error another program takes the input")

        # asked for and given
        check("without a grant ben has no control of it", ben.ask("control act %d move 1 1" % panel) == "error you have no control of that panel")
        ok = ben.ask("control ask %d" % panel) == "ok" and until(lambda: control(ada)["asks"] == [{"number": 0, "person": who["ben"]}])
        check("ben asks for it, and ada's end knows who asks", ok and control(ben)["asked"] == [panel])
        check("it isn't given while nobody says ada has the headset on", ada.ask("control give 0 %d" % who["ben"]).startswith("error nobody says you have the headset on") and
              not control(ada)["there"])
        ok = ada.ask("control here") == "ok" and ada.ask("control give 0 %d" % who["ben"]) == "ok"
        ok = ok and until(lambda: [h["panel"] for h in control(ben)["held"]] == [panel]) and control(ben)["asked"] == []
        check("said to be there, ada gives it, and ben's end hears", ok and control(ada)["given"] == [{"number": 0, "person": who["ben"]}] and control(ada)["asks"] == [])

        sent = [(MOVE, 30000, 65535), (BUTTON, 1, 1), (BUTTON, 1, 0), (KEY, 30, 1), (KEY, 30, 0), (SCROLL, 0, -120)]
        ok = all(ben.ask("control act %d %s %d %d" % (panel, ("", "move", "button", "scroll", "key")[what], a, b)) == "ok" for what, a, b in sent)
        got = desk.got(b"\x85", 0.5)
        check("ben's input reaches the program at ada's desktop, in order, as his", ok and got == [(who["ben"], act(panel, *one)) for one in sent])
        check("...and nobody else's does: cy wasn't given it", cy.ask("control act %d move 5 5" % panel) == "error you have no control of that panel" and desk.got(b"\x85", 0.3) == [])
        check("what no panel takes isn't sent", ben.ask("control act %d key 125 1" % panel) == "error that isn't input a panel takes" and
              ben.ask("control act %d move 70000 0" % panel) == "error that isn't input a panel takes" and
              ben.ask("control act %d fly 1 1" % panel).startswith("error control |") and desk.got(b"\x85", 0.3) == [])

        # taken back with a button down
        ok = ben.ask("control act %d button 1 1" % panel) == "ok" and desk.got(b"\x85", 0.3) == [(who["ben"], act(panel, BUTTON, 1, 1))]
        ok = ok and ada.ask("control take 0") == "ok" and desk.got(b"\x85", 0.3) == [(who["ben"], act(panel, BUTTON, 1, 0))]
        check("taken back while ben holds a button: it is let go for him, and his end hears", ok and until(lambda: control(ben)["held"] == []) and control(ada)["given"] == [])

        # pointers
        mine, bens, cys = program(ada), program(ben), program(cy)
        check("programs ask where the others point", bens.ask("points") == "ok" and cys.ask("points") == "ok" and bens.ask("points") == "ok")
        mine.send(point(panel, 1000, 2000, pressed=True, count=7))
        got = bens.got(b"\x84", 0.5)
        check("where ada points reaches the others' programs, and says whose it is",
              [(p, data[3:]) for p, data in got] == [(who["ada"], point(panel, 1000, 2000, pressed=True)[3:])] and [p for p, _ in cys.got(b"\x84", 0.3)] == [who["ada"]])
        for i in range(5):
            mine.send(point(255, i, i, count=7))   # whatever count the program put in, ft-together's own goes out
        got = bens.got(b"\x84", 0.5)
        counts = [int.from_bytes(data[1:3], "little") for _, data in got]
        check("ft-together counts them itself, so each is newer than the last", len(got) == 5 and counts == sorted(set(counts)) and [data[3] for _, data in got] == [255] * 5)
        for bad in (point(77, 1, 1), point(panel, 1, 1)[:-1] + b"\x02", point(panel, 1, 1)[:-2], b"\x03"):
            mine.send(bad)
        check("what isn't a pointer goes nowhere, and nobody is told of their own", bens.got(b"\x84", 0.3) == [] and mine.got(b"\x84", 0.2) == [])

        # nobody says ada is there any more: the real clock ends the grant
        ok = desk.ask("inputs") == "ok" and ada.ask("control here") == "ok" and ada.ask("control give 0 %d" % who["cy"]) == "ok"
        ok = ok and until(lambda: [h["panel"] for h in control(cy)["held"]] == [panel]) and cy.ask("control act %d key 42 1" % panel) == "ok"
        ok = ok and desk.got(b"\x85", 0.3) == [(who["cy"], act(panel, KEY, 42, 1))]
        began, got = time.monotonic(), []
        while not got and time.monotonic() - began < 15:
            desk.send(b"inputs")   # the taker keeps asking, as it does about once a second
            got = desk.got(b"\x85", 1.0)
        took = time.monotonic() - began
        check("with no word that ada is there, a grant runs out in about ten seconds: the key cy held is let go",
              ok and got == [(who["cy"], act(panel, KEY, 42, 0))] and 8 < took < 15.5 and control(ada)["given"] == [] and not control(ada)["there"] and
              until(lambda: control(cy)["held"] == []))
        check("...and cy's input is nothing after that", cy.ask("control act %d move 1 1" % panel) == "error you have no control of that panel")

        # the holder leaves with a button down
        ok = ada.ask("control here") == "ok" and ada.ask("control give 0 %d" % who["ben"]) == "ok" and until(lambda: len(control(ben)["held"]) == 1)
        ok = ok and ben.ask("control act %d button 3 1" % panel) == "ok" and desk.got(b"\x85", 0.3) == [(who["ben"], act(panel, BUTTON, 3, 1))]
        ok = ok and ben.ask("leave") == "ok" and desk.got(b"\x85", 1.0) == [(who["ben"], act(panel, BUTTON, 3, 0))]
        check("ben leaves with a button down: it is let go, and nothing of his stands at ada's end", ok and until(lambda: control(ada)["given"] == []) and
              all(control(ben)[k] == [] for k in ("held", "asked", "given", "asks")))
    finally:
        for w in watchers:
            w.close()
        ends = [e.end() for e in (ada, ben, cy)]
    check("all three ft-together end cleanly, with nothing from the sanitizers", all(status == 0 and "ERROR: " not in out for status, out in ends))
    if roomkit.failed:
        for status, out in ends:
            print("--- said:\n%s" % out)
    roomkit.finish()


if __name__ == "__main__":
    main()
