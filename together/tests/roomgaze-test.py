#!/usr/bin/env python3
"""ft-roomgaze next to a real ft-together in a room with another, with stand-ins for the pointer
helper and the gaze service: where ben looks reaches ada's end in his poses only while he shares
it, and while his eyes move his pointer and he doesn't, ada's end isn't told where it is.

    roomgaze-test.py FT-TOGETHER

The rules are tested without a clock or a link in gaze-test.cpp and test_roomgaze.py; this is
what only the real programs show.
"""
import json
import os
import struct
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import roomkit  # noqa: E402
from fakescreens import serve  # noqa: E402
from roomkit import Watcher, act, check, point, pose  # noqa: E402

MOVE, BUTTON = 1, 2
SCREEN = "1920 1080 0 1.5 -2 1 0 0 0 2 0 Screen"
LOOKS = 0x08   # in a pose's flags (proto/posewire.h)


class Helper:
    """The pointer helper, as far as ft-roomgaze asks it."""

    def __init__(self):
        self.reply, self.asked = "ok off", []

    def __call__(self, text):
        self.asked.append(text)
        return self.reply


class Gazed:
    """The gaze service, as far as ft-roomgaze asks it."""

    def __init__(self):
        self.last, self.age, self.asked = [10.0, -5.0, 11.0, -4.0], 0.02, []

    def __call__(self, text):
        self.asked.append(text)
        if text == "status":
            return json.dumps({"awake": True, "mode": "on", "sample_age_s": self.age, "last": self.last})
        return "ok" if text.startswith("wake ") else "error status|wake"


def gaze_of(data):
    """A pose datagram's gaze: (to their left, up) in degrees, or None when it has none."""
    if not data[25] & LOOKS:
        return None
    yaw, pitch = struct.unpack_from("<bb", data, 31)
    return yaw / 2, pitch / 2


def keep(say):
    """Says it each second from now on, as the programs that ask ft-together for something do."""
    def again():
        while True:
            time.sleep(1.0)
            try:
                say()
            except OSError:
                return
    threading.Thread(target=again, daemon=True).start()


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
    ada, ben = roomkit.End(sys.argv[1], "ada"), roomkit.End(sys.argv[1], "ben")
    helper, gazed = Helper(), Gazed()
    names = ["ft_test_%d_%s" % (os.getpid(), n) for n in ("helper", "gazed")]
    serve(helper, names[0])
    serve(gazed, names[1])
    tool = roomkit.Tool([str(HERE.parent / "ft-roomgaze"), "--together", ben.sock, "--helper", names[0], "--gazed", names[1]])
    sees, points, desk, body = Watcher(ada), Watcher(ada), Watcher(ada), Watcher(ben)

    def gaze(end=ben):
        reply = end.ask("gaze")
        return json.loads(reply[3:]) if reply.startswith("ok {") else {"why": reply}

    def looks(seconds=0.6):
        """Ben's body says where he is: the gazes ada's end gets in his poses in that time."""
        sees.poses(0.05)
        deadline = time.monotonic() + seconds
        got = []
        while time.monotonic() < deadline:
            body.send(pose(0.5))
            got += [gaze_of(data) for _, data in sees.poses(0.1)]
        return got

    def near(got, yaw, pitch):
        return bool(got) and all(g is not None and abs(g[0] - yaw) <= 0.5 and abs(g[1] - pitch) <= 0.5 for g in got)

    def pointed(seconds=0.4):
        """Where ada's end was told ben points in that time: [(panel, x, y, pressed)]."""
        return [struct.unpack("<BHBHHB", data)[2:] for _, data in points.got(b"\x84", seconds)]

    try:
        check("with nothing shared it says so, and that his hand moves his pointer", tool.said("your hand moves the pointer; where you look isn't shared"))
        check("...and ft-together has it so", until(lambda: gaze() == {"shared": False, "fresh": False, "quiet": False}))
        check("outside a room nothing is shared", ben.ask("gaze on") == "error you aren't in a room" and not gaze()["shared"])
        check("ada opens a room and ben comes in", ada.ask("open") == "ok" and roomkit.let_in(ada, ben) and ada.until(roomkit.linked) and ben.until(roomkit.linked))
        who = {p["name"]: p["id"] for p in ada.state()["people"]}
        panel = who["ada"] * 8
        ok = ada.ask("share 0 " + SCREEN) == "ok" and ben.until(lambda s: [p["id"] for p in s["panels"]] == [panel])
        ok = ok and sees.ask("watch") == "ok" and points.ask("points") == "ok" and desk.ask("inputs") == "ok"
        for program, asks in ((sees, b"watch"), (points, b"points"), (desk, b"inputs")):
            keep(lambda program=program, asks=asks: program.send(asks))
        here = roomkit.ftipc.Client(timeout=1.0)
        keep(lambda: here.ask(ada.sock, "control here"))
        ok = ok and ben.ask("control ask %d" % panel) == "ok" and ada.ask("control here") == "ok"
        ok = ok and until(lambda: ada.ask("control give 0 %d" % who["ben"]) == "ok") and until(lambda: '"held":[{' in ben.ask("control"))
        check("ada shares a panel and gives ben control of it", ok)

        # nothing is shared
        got = looks(1.5)
        check("ben's poses reach ada's end with no gaze, and the gaze service was asked nothing", len(got) >= 5 and set(got) == {None} and gazed.asked == [])
        check("...whatever his own programs' poses claim", ben.ask("gaze say 20 5") == "error you don't share where you look" and set(looks()) == {None})

        # shared
        check("ben shares where he looks: it is told", ben.ask("gaze on") == "ok" and tool.said("where you look is shared, and told"))
        check("...and reaches ada's end in his poses", until(lambda: near(looks(0.3), 10, -5)) and gaze()["shared"] and gaze()["fresh"])
        check("the gaze service is asked to keep running, and for where he looks", "wake 3" in gazed.asked and gazed.asked.count("status") >= 2)
        check("ada's own poses have none: she shares nothing", not gaze(ada)["shared"])
        gazed.last = [-20.0, 15.0, 0.0, 0.0]
        seen = looks(0.5)
        check("he looks elsewhere: it follows, smoothed", until(lambda: near(looks(0.3), -20, 15)) and any(g is not None and -19 < g[0] < 9 for g in seen))
        gazed.age = 3.0
        check("the gaze service has nothing new: a second later his poses have no gaze", until(lambda: set(looks(0.3)) == {None}) and not gaze()["fresh"]
              and tool.said("where you look is shared, and the gaze service has no direction"))
        gazed.age = 0.02
        check("...and it is back when the service has", until(lambda: near(looks(0.3), -20, 15)) and tool.said("and told", since=len(tool.lines) - 1))

        # away
        lines = len(tool.lines)
        check("ben's headset comes off: the gaze service is left alone", ben.ask("away off") == "ok" and tool.said("and you aren't there", since=lines))
        asked = len(gazed.asked)
        time.sleep(2.5)
        check("...for as long as it is off, and no gaze goes out", len(gazed.asked) == asked and set(looks()) == {None})
        check("back on, it is told again", ben.ask("away back") == "ok" and until(lambda: near(looks(0.3), -20, 15)))

        # the pointer, while the eyes move it
        helper.reply = "ok on"
        check("gaze mode on: it says his eyes move his pointer", tool.said("your eyes move the pointer; where you look is shared, and told"))
        body.send(point(panel, 1000, 2000))
        check("shared, where he points goes to the room as ever", pointed() == [(panel, 1000, 2000, 0)])
        lines = len(tool.lines)
        check("ben stops sharing: no gaze goes out", ben.ask("gaze off") == "ok" and tool.said("where you look isn't shared", since=lines) and until(lambda: set(looks(0.3)) == {None}))
        check("...and his pointer is quiet", gaze() == {"shared": False, "fresh": False, "quiet": True})
        asked = len(gazed.asked)
        body.send(point(panel, 3000, 4000))
        check("a pointer that only moves tells the room it is nowhere, once", pointed() == [(255, 0, 0, 0)])
        body.send(point(panel, 3100, 4100))
        check("...and then nothing", pointed() == [])
        ok = ben.ask("control act %d move 3100 4100" % panel) == "ok" and ben.ask("control act %d move 3200 4200" % panel) == "ok"
        check("its moves on the panel he was given wait too", ok and desk.got(b"\x85", 0.4) == [])
        ok = ben.ask("control act %d button 1 1" % panel) == "ok"
        check("a click goes with the last place it was at", ok and desk.got(b"\x85", 0.4) == [(who["ben"], act(panel, MOVE, 3200, 4200)), (who["ben"], act(panel, BUTTON, 1, 1))])
        body.send(point(panel, 3300, 4300, pressed=True))
        ok = ben.ask("control act %d move 3300 4300" % panel) == "ok"
        check("while the button is held the drag is seen and given", ok and pointed() == [(panel, 3300, 4300, 1)] and desk.got(b"\x85", 0.3) == [(who["ben"], act(panel, MOVE, 3300, 4300))])
        ok = ben.ask("control act %d button 1 0" % panel) == "ok" and desk.got(b"\x85", 0.3) == [(who["ben"], act(panel, BUTTON, 1, 0))]
        body.send(point(panel, 3400, 4400))
        check("let go, it is nowhere again", ok and pointed() == [(255, 0, 0, 0)])
        check("the gaze service was asked nothing since he stopped sharing", len(gazed.asked) == asked)
        helper.reply = "ok off"
        check("gaze mode off: his hand moves it, and it is told again", tool.said("your hand moves the pointer; where you look isn't shared", since=len(tool.lines) - 1)
              and until(lambda: not gaze()["quiet"]))
        body.send(point(panel, 5000, 6000))
        ok = ben.ask("control act %d move 5000 6000" % panel) == "ok"
        check("...moves and all", ok and pointed() == [(panel, 5000, 6000, 0)] and desk.got(b"\x85", 0.3) == [(who["ben"], act(panel, MOVE, 5000, 6000))])
        helper.reply = None
        check("a helper that stops answering: after a few seconds the pointer is quiet, for nobody says what moves it",
              tool.said("nobody says what moves the pointer") and until(lambda: gaze()["quiet"], 9))
        helper.reply = "ok off"
        check("...until it answers", until(lambda: not gaze()["quiet"]))

        # shared for a room
        lines = len(tool.lines)
        check("shared again, then ben leaves", ben.ask("gaze on") == "ok" and tool.said("and told", since=lines) and ben.ask("leave") == "ok")
        check("out of the room nothing is shared", until(lambda: not gaze()["shared"]) and tool.said("where you look isn't shared", since=lines))
        check("...nor when he comes in again", roomkit.let_in(ada, ben) and ben.until(roomkit.linked) and not gaze()["shared"])
        asked = len(gazed.asked)
        time.sleep(1.5)
        check("...and the gaze service is left alone", len(gazed.asked) == asked)
        lines = len(tool.lines)
        ok = ben.ask("gaze on") == "ok" and tool.said("and told", since=lines) and ada.ask("leave") == "ok"
        check("shared once more, and ada closes her room: it ends for ben, and nothing is shared", ok and ben.until(lambda s: s["mode"] == "idle") and until(lambda: not gaze()["shared"])
              and tool.said("where you look isn't shared", since=lines))
        check("told to end, it ends cleanly", tool.end() == 0 and not any("Traceback" in line for line in tool.lines))
    finally:
        tool.end()
        for program in (sees, points, desk, body):
            program.close()
        ends = [ada.end(), ben.end()]
    check("both ft-together end cleanly, with nothing from the sanitizers", all(status == 0 and "ERROR: " not in out for status, out in ends))
    roomkit.finish()


if __name__ == "__main__":
    main()
