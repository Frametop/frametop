#!/usr/bin/env python3
"""ft-roomvoice between two real ft-together on this machine's loopback, with no sound devices:
each one's wearer hums a note of their own (--hum) and what would be played is measured
(--probe). One's voice reaches the other's ears as the note it was, from the side its owner
stands on, and stops when they mute, take the headset off or leave. Their mouth, as the others
are told it in their poses, moves with it.

    roomvoice-test.py FT-TOGETHER FT-ROOMVOICE
"""
import json
import math
import os
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import roomkit  # noqa: E402
from fakescreens import Screens, serve  # noqa: E402
from roomkit import check  # noqa: E402


class Frame:
    """One person's Frame: ft-together, a stand-in ft-screens that says where the head is, and
    ft-roomvoice humming at `hz`."""

    def __init__(self, programs, name, hz, head, more=()):
        self.end = roomkit.End(programs[0], name)
        self.screens = Screens()
        self.screens.head = head
        self.name = "ft_test_%d_%s" % (os.getpid(), name)
        serve(self.screens, self.name + "_screens")
        self.tool = roomkit.Tool([programs[1], "--socket", self.name + "_voice", "--together", self.end.sock, "--screens", self.name + "_screens",
                                  "--hum", str(hz), "--probe", *more])
        self.body = roomkit.Watcher(self.end)   # stands in for ft-avatars: it says where the head is
        self.at = None

    def ask(self, text):
        return self.end.client.ask(self.name + "_voice", text) or "none"

    def state(self):
        reply = self.ask("state")
        return json.loads(reply[3:]) if reply.startswith("ok {") else {"room": False, "mic": reply, "voices": [], "out": {"left": 0, "right": 0, "hz": 0, "frames": 0}}

    def until(self, what, seconds=8.0):
        """Whether what(state) comes true in time. Meanwhile the head's place is told, as a
        program that draws the room would."""
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            for f in Frame.all:
                if f.at:
                    f.body.send(roomkit.pose(f.at[0], z=f.at[1]))
            if what(self.state()):
                return True
            time.sleep(0.1)
        return False


def mouths(frame, seconds=1.5):
    """How wide `frame`'s wearer's mouth was in the poses the others got over that time, while a
    program of theirs said where their head is thirty times a second: [its shapes' sum]."""
    watcher = [f for f in Frame.all if f is not frame][0].body
    if watcher.ask("watch") != "ok":
        return []
    watcher.poses(0.2)   # (what waited there is from before)
    got = []
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        frame.body.send(roomkit.pose(0.0))
        got += [sum(data[26:31]) for _, data in watcher.poses(0.033)]
    return got


def hears(hz, left=True, right=True):
    """A state in which that note is played, in the ears named."""
    def what(state):
        out = state["out"]
        return abs(out["hz"] - hz) < 15 and (out["left"] > 0.02) == left and (out["right"] > 0.02) == right
    return what


def quiet(state):
    return state["out"]["left"] < 0.002 and state["out"]["right"] < 0.002


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip())
    ada = Frame(sys.argv[1:], "ada", 220, "ok 0.2000 1.6000 0.1000 30.00")
    ben = Frame(sys.argv[1:], "ben", 330, "ok -0.4000 1.7000 0.3000 -120.00", ["--muted"])
    Frame.all = (ada, ben)
    try:
        check("each ft-roomvoice starts in no room", all(f.tool.said("in no room") or f.until(lambda s: not s["room"], 3) for f in Frame.all))
        time.sleep(1.0)
        s = ada.state()
        check("outside a room nothing listens and nothing is played", (s["mic"], s["sent"], s["playing"], s["voices"]) == ("closed", 0, False, []))

        check("ada opens a room and ben comes in", ada.end.ask("open") == "ok" and roomkit.let_in(ada.end, ben.end) and ada.end.until(roomkit.linked) and ben.end.until(roomkit.linked))
        check("each sees the room", all(f.tool.said("in a room") for f in Frame.all))
        check("ada's microphone opens; ben's, who started muted, doesn't", ada.tool.said("the microphone is open") and ada.until(lambda s: s["mic"] == "open" and s["sent"] > 20)
              and ben.state()["mic"] == "closed" and ben.state()["muted"] is True)
        check("ben hears ada's note, in the middle while nobody knows where she is", ben.until(hears(220)))
        out = ben.state()["out"]
        check("...as loud in both ears", abs(out["left"] - out["right"]) < 0.1 * out["left"])
        ids = {p["name"]: p["id"] for p in ada.end.state()["people"]}
        told = ben.state()["voices"]
        check("and is told who speaks", len(told) == 1 and told[0]["person"] == ids["ada"] and told[0]["speaking"] and not told[0]["placed"] and told[0]["level"] > 0.1
              and told[0]["played"] > 20 and told[0]["late"] == 0)
        wide = mouths(ada)
        check("ada's mouth moves with her voice in the poses ben gets", len(wide) > 20 and max(wide) > 100 and max(wide) <= 260)
        check("...and ben's, who is muted, stays shut", set(mouths(ben, 0.7)) == {0})
        check("ada hears nothing: ben is muted", ada.until(lambda s: s["playing"] and s["out"]["frames"] >= 50 and quiet(s)) and ada.state()["voices"][0]["played"] == 0)

        # where ada stands: ben's head is on his spot, facing the way the spot faces
        spot = [p["spot"] for p in ben.end.state()["people"] if p["name"] == "ben"][0]
        aside = (math.cos(spot[3]), -math.sin(spot[3]))   # to his right, in the room
        ada.at = (spot[0] + 1.5 * aside[0], spot[2] + 1.5 * aside[1])
        check("with ada to his right, ben hears her on the right", ben.until(lambda s: s["voices"][0]["placed"] and s["out"]["right"] > 2 * s["out"]["left"] > 0.01))
        ada.at = (spot[0] - 1.5 * aside[0], spot[2] - 1.5 * aside[1])
        check("she walks round to his left, and so does her voice", ben.until(lambda s: s["out"]["left"] > 2 * s["out"]["right"] > 0.01))
        ada.at = (spot[0] - 6 * aside[0], spot[2] - 6 * aside[1])
        near = ben.state()["out"]["left"]
        check("further away she is quieter", ben.until(lambda s: 0.005 < s["out"]["left"] < 0.5 * near))
        ada.at = None
        check("when nobody says where she is any more, she is back in the middle", ben.until(lambda s: not s["voices"][0]["placed"] and hears(220)(s)
                                                                                      and abs(s["out"]["left"] - s["out"]["right"]) < 0.1 * s["out"]["left"]))

        check("ben unmutes, and ada hears his note", ben.ask("unmute") == "ok" and ben.tool.said("the microphone is open") and ada.until(hears(330)))
        check("...and sees his mouth move", max(mouths(ben, 1.0), default=0) > 100)
        check("ben mutes: his microphone closes and ada hears nothing", ben.ask("mute") == "ok" and ben.until(lambda s: s["mic"] == "closed" and s["muted"]) and ada.until(quiet))
        check("...and his mouth is shut", set(mouths(ben, 0.7)[3:]) == {0})
        check("a question it doesn't know is refused", ben.ask("louder").startswith("error") and ben.ask("unmute") == "ok" and ada.until(hears(330)))

        check("ben takes the headset off: nothing listens and nothing is played there", ben.end.ask("away off") == "ok"
              and ben.until(lambda s: s["mic"] == "closed" and not s["playing"]) and ada.until(quiet))
        check("away in a game his voice stays on", ben.end.ask("away game") == "ok" and ben.until(lambda s: s["mic"] == "open" and s["playing"]) and ada.until(hears(330)))
        check("and back", ben.end.ask("away back") == "ok" and ben.until(hears(220)))

        lines = len(ben.tool.lines)
        check("ben leaves: his microphone closes", ben.end.ask("leave") == "ok" and ben.tool.said("in no room", since=lines) and ben.until(lambda s: s["mic"] == "closed" and not s["playing"]))
        check("and ada has nobody to hear", ada.until(lambda s: s["voices"] == [] and quiet(s)))

        check("asked to quit, it ends cleanly", ben.ask("quit") == "ok" and ben.tool.wait(5) == 0)
        check("and so does one that is told to end", ada.tool.end() == 0)
        for f in Frame.all:
            trouble = [line for line in f.tool.lines if "ERROR" in line or "runtime error" in line]
            check("%s's ft-roomvoice had nothing from the sanitizers" % f.end.name, not trouble)
            if trouble:
                print("\n".join(f.tool.lines[-40:]))
    finally:
        for f in Frame.all:
            f.tool.end()
        ends = [f.end.end() for f in Frame.all]
    check("both ft-together end cleanly, with nothing from the sanitizers", all(status == 0 and "ERROR: " not in out for status, out in ends))
    roomkit.finish()


if __name__ == "__main__":
    main()
