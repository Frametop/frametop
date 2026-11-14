#!/usr/bin/env python3
"""The wearer's hand on someone else's panel, through two real ft-together on this machine's
loopback: what ft-screens says of ben's pointer and keys on ada's panel reaches ada's end as his
pointer, for everyone, and as his input, once she gave him the panel; and nothing of his stays
down when his program ends.

    panelhand-test.py FT-TOGETHER HANDWORDS

HANDWORDS (together/tests/handwords.cpp) is ft-peerpanel's hand without its decoder: the same
rules (together/local/panelhand.h, tested alone in panelhand-test.cpp) and the same sender.
"""
import json
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import roomkit  # noqa: E402
from roomkit import Watcher, act, check  # noqa: E402

MOVE, BUTTON, SCROLL, KEY = 1, 2, 3, 4
SCREEN = "1920 1080 0 1.5 -2 1 0 0 0 2 0 Screen"
MIDDLE = (32768, 32768)


class Hand:
    """ben's hand: ft-screens' words in, a line each."""

    def __init__(self, program, panel, sock):
        self.proc = subprocess.Popen([program, str(panel), sock, "1920", "1080"], stdin=subprocess.PIPE)

    def say(self, *words):
        self.proc.stdin.write("".join(w + "\n" for w in words).encode())
        self.proc.stdin.flush()

    def end(self):
        if self.proc.stdin:
            self.proc.stdin.close()
            self.proc.stdin = None
        try:
            return self.proc.wait(5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            return "it didn't end"


def points(watcher, seconds):
    """The pointers that came in that time: [(person, panel, x, y, pressed)]."""
    return [(person, data[3], int.from_bytes(data[4:6], "little"), int.from_bytes(data[6:8], "little"), data[8]) for person, data in watcher.got(b"\x84", seconds)]


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip())
    ada, ben = roomkit.End(sys.argv[1], "ada"), roomkit.End(sys.argv[1], "ben")
    desk, seen, hand = Watcher(ada), Watcher(ada), None

    def asking():
        """The programs at ada's end ask again, as they do each second."""
        return desk.ask("inputs") == "ok" and seen.ask("points") == "ok"

    try:
        ok = all(e.until(lambda s: s["mode"] == "idle") for e in (ada, ben))
        ok = ok and ada.ask("open") == "ok" and roomkit.let_in(ada, ben) and ada.until(roomkit.linked) and ben.until(roomkit.linked)
        if not check("ada opens a room and ben comes in", ok):
            return
        check("ben's Frame says his hand moves his pointer", ben.hand())
        who = {p["name"]: p["id"] for p in ada.state()["people"]}
        panel = who["ada"] * 8
        check("ada shares a panel", ada.ask("share 0 " + SCREEN) == "ok" and ben.until(lambda s: [p["id"] for p in s["panels"]] == [panel]))
        hand = Hand(sys.argv[2], panel, ben.sock)

        # not given the panel
        ok = asking()
        hand.say("move 960 540", "button 272 1", "button 272 0", "key 30 1", "key 30 0", "leave")
        got = points(seen, 0.6)
        check("ben points at ada's panel, clicks and goes: ada's end sees where, that he held a button there, and that he went",
              ok and got == [(who["ben"], panel, *MIDDLE, 0), (who["ben"], panel, *MIDDLE, 1), (who["ben"], panel, *MIDDLE, 0), (who["ben"], 255, *MIDDLE, 0)])
        check("...and none of it reaches her desktop: she gave him nothing", desk.got(b"\x85", 0.3) == [])

        # given it
        ok = ada.ask("control here") == "ok" and ada.ask("control give 0 %d" % who["ben"]) == "ok"
        ok = ok and ben.until(lambda _: [h["panel"] for h in json.loads(ben.ask("control")[3:])["held"]] == [panel]) and asking()
        hand.say("move 480 270", "button 272 1")
        time.sleep(0.1)
        hand.say("move 1440 810", "button 272 0", "key 30 1", "key 30 0", "scroll 0 -1", "scroll 0.5 0")
        sent = [(MOVE, 16384, 16384), (BUTTON, 1, 1), (MOVE, 49152, 49152), (BUTTON, 1, 0), (KEY, 30, 1), (KEY, 30, 0), (SCROLL, 0, -120), (SCROLL, 60, 0)]
        check("given the panel, what his hand does on it reaches the program at her desktop, in order, as his",
              ok and desk.got(b"\x85", 0.6) == [(who["ben"], act(panel, *one)) for one in sent])
        got = points(seen, 0.3)
        check("...with his pointer, held while he dragged", [g[1:] for g in got] == [(panel, 16384, 16384, 0), (panel, 16384, 16384, 1), (panel, 49152, 49152, 1), (panel, 49152, 49152, 0)])

        # resting, and leaving
        ok = asking()
        rests = (who["ben"], panel, 49152, 49152, 0)
        got = points(seen, 1.3)
        check("a pointer that rests is said again each second, and nothing of it goes to the desktop", ok and got in ([rests], [rests, rests]) and desk.got(b"\x85", 0.2) == [])
        hand.say("leave")
        got = points(seen, 0.5)
        check("when it leaves the panel, ada's end hears that he points at nothing", [g[1] for g in got[-1:]] == [255] and all(g == rests for g in got[:-1]))
        check("...and hears no more of it", asking() and points(seen, 1.5) == [])

        # his program ends with a button and a key down
        ok = asking()
        hand.say("move 960 540", "button 273 1", "key 42 1")
        ok = ok and desk.got(b"\x85", 0.5) == [(who["ben"], act(panel, *one)) for one in ((MOVE, *MIDDLE), (BUTTON, 2, 1), (KEY, 42, 1))]
        seen.got(b"\x84", 0.1)
        status = hand.end()
        check("his program ends with a button and a key down: both come up at her desktop, and his pointer goes",
              ok and status == 0 and desk.got(b"\x85", 0.5) == [(who["ben"], act(panel, KEY, 42, 0)), (who["ben"], act(panel, BUTTON, 2, 0))] and
              [g[1] for g in points(seen, 0.3)[-1:]] == [255])
    finally:
        if hand:
            hand.end()
        for w in (desk, seen):
            w.close()
        ends = [ada.end(), ben.end()]
    check("both ft-together end cleanly, with nothing from the sanitizers", all(status == 0 and "ERROR: " not in out for status, out in ends))
    roomkit.finish()


if __name__ == "__main__":
    main()
