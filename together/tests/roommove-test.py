#!/usr/bin/env python3
"""ft-roommove next to a real ft-together and a real ft-roompanels, with a stand-in for its
Frame's ft-screens (fakescreens.py): with both grips down, the room goes where the hands go and
the Frame's floor moves in the room so that each hand keeps hold of the same place of it; with
one grip, outside a room, or with a hand that isn't tracked, nothing moves.

    roommove-test.py FT-TOGETHER

The grips are told on its socket (--probe). Reading them from SteamVR is test_roommove.py's,
against a stand-in for its web socket.
"""
import json
import math
import os
import signal
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
import roomkit  # noqa: E402
from fakescreens import Screens, serve  # noqa: E402
from roomkit import check  # noqa: E402
from roompanels import placing  # noqa: E402

HANDS = ((-0.3, 1.1, -0.4), (0.3, 1.2, -0.4))


def near(a, b, by):
    return len(a) == len(b) and all(abs(x - y) <= by for x, y in zip(a, b))


class Frame:
    """One person's Frame: ft-together, a stand-in ft-screens, ft-roompanels and ft-roommove."""

    def __init__(self, program):
        self.end = roomkit.End(program, "ada")
        self.screens = Screens()
        self.screens.hands = HANDS
        self.screens.at = {1: (-0.3, 1.5, -0.4), 2: (0.3, 1.5, -0.4)}     # a screen at each hand: what the hands hold
        self.name = "ft_test_%d" % os.getpid()
        self.lock = serve(self.screens, self.name + "_screens")
        self.panels = roomkit.Tool([sys.executable, str(HERE.parent / "ft-roompanels"), "--socket", self.name + "_panels",
                                    "--together", self.end.sock, "--screens", self.name + "_screens"])
        self.mover = self.move()

    def move(self):
        return roomkit.Tool([sys.executable, str(HERE.parent / "ft-roommove"), "--probe", "--socket", self.name + "_move", "--together", self.end.sock,
                             "--screens", self.name + "_screens", "--panels", self.name + "_panels"])

    def ask(self, text):
        return self.end.client.ask(self.name + "_move", text) or "none"

    def state(self):
        reply = self.ask("state")
        return json.loads(reply[3:]) if reply.startswith("ok {") else {}

    def until(self, what, seconds=5.0):
        """Whether what(ft-roommove's state) comes true in time."""
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if what(self.state()):
                return True
            time.sleep(0.05)
        return False

    def floor(self):
        words = self.end.ask("floor").split()
        return tuple(float(w) for w in words[1:]) if len(words) == 4 else None

    def hands(self, at):
        with self.lock:
            self.screens.hands = at

    def seen(self):
        """(how often ft-screens was asked for the hands, how often to move the room)."""
        with self.lock:
            return self.screens.count("reach"), self.screens.count("shift") - self.screens.count("shift back")

    def held(self):
        """The two places of the room in the hands: (x, z) each."""
        with self.lock:
            at = self.screens.hands
        floor = self.floor()
        return [placing.turned(floor[2], (hand[0], 0.0, hand[2])) for hand in at], floor

    def in_hand(self, by=0.005):
        with self.lock:
            return all(near(self.screens.at[n][::2], self.screens.hands[n - 1][::2], by) for n in (1, 2))


def in_room(frame):
    """The places of the room under the two hands."""
    turned, floor = frame.held()
    return [(t[0] + floor[0], t[2] + floor[1]) for t in turned]


def carried(at, yaw, by):
    """Both hands turned `yaw` about their middle and moved `by`."""
    mid = [(at[0][k] + at[1][k]) / 2 for k in range(3)]
    out = []
    for hand in at:
        t = placing.turned(yaw, (hand[0] - mid[0], 0.0, hand[2] - mid[2]))
        out.append((t[0] + mid[0] + by[0], hand[1], t[2] + mid[2] + by[1]))
    return tuple(out)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip())
    ada = Frame(sys.argv[1])
    try:
        check("ft-roommove starts outside a room", ada.mover.said("not in a room") and ada.state() == {"room": False, "both": False, "held": False, "pulls": 0, "refused": 0})
        second = ada.move()
        check("a second one doesn't start", second.wait(10) == 1 and second.said("runs already"))
        check("grips closed outside a room hold nothing", ada.ask("grips 1 1") == "ok" and not ada.until(lambda s: s.get("both"), 1.5))
        check("and ft-screens isn't asked where the hands are", ada.seen() == (0, 0))

        check("ada opens a room and her floor is laid", ada.end.ask("open") == "ok" and ada.end.until(lambda s: ada.floor() is not None))
        check("ft-roommove sees it", ada.mover.said("in a room: both grips pull you through it"))
        check("the grips closed before count for nothing", not ada.until(lambda s: s.get("both"), 0.5) and ada.seen() == (0, 0))

        check("one grip holds nothing", ada.ask("grips 1 0") == "ok" and not ada.until(lambda s: s.get("both") or s.get("held"), 0.5) and ada.seen() == (0, 0))
        check("both take hold of the room", ada.ask("grips 1 1") == "ok" and ada.until(lambda s: s == {"room": True, "both": True, "held": True, "pulls": 0, "refused": 0}))
        was, start = in_room(ada), ada.floor()
        at = HANDS
        for i in range(40):
            at = carried(at, math.radians(1.5), (0.012, -0.007 if i < 20 else 0.01))
            ada.hands(at)
            time.sleep(0.03)
        check("the hands moved and turned, and the room came along", ada.until(lambda s: ada.in_hand()))
        check("in many small pulls, none refused", ada.until(lambda s: s["pulls"] >= 20 and s["refused"] == 0))
        now = in_room(ada)
        check("each hand holds the place of the room it took hold of", all(near(a, b, 0.006) for a, b in zip(was, now)))
        moved = ada.floor()
        check("so her floor lies elsewhere in the room, turned the other way", math.hypot(moved[0] - start[0], moved[1] - start[1]) > 0.2
              and abs(placing.wrap(moved[2] - start[2] + math.radians(60))) < math.radians(1))

        check("letting one grip go lets the room go", ada.ask("grips 0 1") == "ok" and ada.until(lambda s: not s["held"]))
        shifts = ada.seen()[1]
        ada.hands(carried(at, 0.3, (0.2, 0.2)))
        time.sleep(0.4)
        check("the hands then move without it", ada.seen()[1] == shifts and near(ada.floor(), moved, 1e-6))
        check("closing it again takes hold where the hands are", ada.ask("grips 1 1") == "ok" and ada.until(lambda s: s["held"]) and ada.seen()[1] == shifts)

        ada.hands("error no right controller tracked")
        check("a hand that isn't tracked lets go, and its grip counts for nothing", ada.until(lambda s: not s["both"] and not s["held"]))
        ada.hands(HANDS)
        time.sleep(0.4)
        check("tracked again it holds nothing until pressed again", not ada.state()["held"] and ada.seen()[1] == shifts)
        check("pressed again it does", ada.ask("grips 1 1") == "ok" and ada.until(lambda s: s["held"]))

        check("a question it doesn't know is refused", ada.ask("grips 2 1") == "error state | quit" and ada.ask("pull 1 0 0") == "error state | quit")

        check("ada leaves the room", ada.end.ask("leave") == "ok")
        check("ft-roommove sees that too and lets go", ada.mover.said("not in a room", since=2) and ada.until(lambda s: s == {**s, "room": False, "both": False, "held": False}))
        asked = ada.seen()[0]
        time.sleep(0.4)
        check("and asks ft-screens nothing more", ada.seen()[0] == asked)

        check("it ends cleanly when told to", ada.mover.end(signal.SIGTERM) == 0)
        again = ada.move()
        check("and when asked to", again.said("not in a room") and ada.ask("quit") == "ok" and again.wait(10) == 0)
        for tool in (ada.mover, second, again, ada.panels):
            trouble = [line for line in tool.lines if "Traceback" in line or "Error" in line]
            check("nothing printed an error", not trouble)
            for line in trouble:
                print("    " + line)
    finally:
        ada.mover.end()
        ada.panels.end()
        status, out = ada.end.end()
    check("ft-together ends cleanly, with nothing from the sanitizers", status == 0 and "ERROR: " not in out)
    if status != 0 or "ERROR: " in out:
        print(out)
    roomkit.finish()


if __name__ == "__main__":
    main()
