#!/usr/bin/env python3
"""Moving shared panels through the real programs: two ft-together on this machine's loopback,
each with a real ft-roompanels and a stand-in for its Frame's ft-screens (fakescreens.py). The
room's owner moves a guest's panel; a guest pulls a panel they may not move, is flown to it,
and it goes back; given control of it they move it; with that taken back they don't; and
someone flies to a panel asked through ft-room.

    roomcarry-test.py FT-TOGETHER

A hand that carries a panel is the test's own (fakescreens.py's `carry`). The rules without
programs are test_roomcarrying.py's, test_roomgliding.py's and moves-test.cpp's.
"""
import json
import math
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
import roomkit  # noqa: E402
from roomframe import Frame, at_spot, near, panel_in_room, stands  # noqa: E402
from roomkit import check  # noqa: E402
from roompanels import moving, placing  # noqa: E402


def until(what, seconds=5.0):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if what():
            return True
        time.sleep(0.05)
    return False


def carry(frame, by=(0.0, 0.0, 0.0), yaw=0.0):
    """The frame's person takes the room's panel that stands there as screen 116 and puts it
    elsewhere. Returns where: the twelve numbers of its pose."""
    with frame.lock:
        frame.screens.carry(116, by, yaw)
        return list(frame.screens.panels[116]["pose"])


def poses(frame, screen):
    with frame.lock:
        return frame.screens.count("pose %d " % screen)


def stood(frame):
    with frame.lock:
        return list(frame.screens.panels[116]["pose"])


def reading(frame, ident):
    return moving.reading(next(p for p in frame.end.state()["panels"] if p["id"] == ident))


def held(end):
    reply = end.ask("control")
    return [h["panel"] for h in json.loads(reply[3:])["held"]] if reply.startswith("ok {") else None


def room_tool(frame, *words):
    done = subprocess.run([sys.executable, str(HERE.parent / "ft-room"), "--socket", frame.end.sock, "--panels", frame.name + "_panels", *words],
                          capture_output=True, text=True, timeout=10)
    return done.returncode, (done.stdout + done.stderr).strip()


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip())
    ada = Frame(sys.argv[1], "ada", "ok 0.2000 1.6000 0.1000 30.00")
    ben = Frame(sys.argv[1], "ben", "ok -0.4000 1.7000 0.3000 -120.00")
    frames = (ada, ben)
    try:
        for f in frames:
            f.keep()
        ok = all(f.tool.said("in no room") for f in frames) and ada.end.ask("open") == "ok" and roomkit.let_in(ada.end, ben.end)
        ok = ok and ada.end.until(roomkit.linked) and ben.end.until(roomkit.linked)
        ok = ok and ada.end.until(lambda s: ada.floor() is not None) and ben.end.until(lambda s: ben.floor() is not None)
        ok = ok and ada.ask("share 1") == "ok 0" and ben.ask("share 2") == "ok 0"
        ok = ok and until(lambda: stands(ben, 116, ada, 1)) and until(lambda: stands(ada, 116, ben, 2))
        if not check("ada has a room, ben is in it, and each sees the other's screen as a panel", ok):
            return
        who = {p["name"]: p["id"] for p in ada.end.state()["people"]}

        # the room's owner moves a guest's panel
        put = carry(ada, by=(0.3, 0.1, -0.2), yaw=0.4)
        check("ada, whose room it is, puts ben's panel elsewhere, and his screen goes there", until(lambda: poses(ben, 2) >= 1 and stands(ada, 116, ben, 2)))
        want = placing.to_room(ada.floor(), {"at": put[3::4], "x": put[0::4], "y": put[1::4], "z": put[2::4], "metres": 1.2, "curve": 0.0})
        check("the room has it where she put it", until(lambda: near(panel_in_room(ben.end, 8)[0], want["at"], 3e-3)))
        time.sleep(0.8)    # a few looks of each ft-roompanels
        check("and it was never taken out of her hand: it stands where she put it, placed once", near(stood(ada), put, 1e-3) and poses(ada, 116) == 1)
        check("ben's screen was put once", poses(ben, 2) == 1)

        # a guest pulls a panel they may not move
        with ben.lock:
            head = [float(w) for w in ben.screens.head.split()[1:4]]
        way = [h - a for h, a in zip(head, stood(ben)[3::4])]
        before, home = ben.head(), reading(ben, 0)
        check("ben doesn't stand where ada's panel reads well", not at_spot(ben, home))
        carry(ben, by=tuple(v / math.hypot(*way) for v in way))        # a metre nearer
        check("ben pulls ada's panel toward him: he is flown to where it reads well", until(lambda: at_spot(ben, home)))
        check("the panel is back where the room has it", until(lambda: stands(ben, 116, ada, 1)))
        with ada.lock:
            unmoved = near(ada.screens.at[1], (0.0, 1.5, -2.0), 1e-6)
        check("and ada's screen never moved", unmoved and poses(ada, 1) == 0 and ada.end.ask("moves") == "ok 0")
        check("ada's panel of ben's stands where his screen does, for all his flying", stands(ada, 116, ben, 2))
        check("he went there over many steps, not in one", sum(1 for s in ben.screens.shifted if s != "back") > 5 and not at_spot(ben, before))

        # given control of it, they move it
        placed = poses(ben, 116)
        ok = ben.end.ask("control ask 0") == "ok" and until(lambda: json.loads(ada.end.ask("control")[3:])["asks"] == [{"number": 0, "person": who["ben"]}])
        ok = ok and ada.end.ask("control here") == "ok" and ada.end.ask("control give 0 %d" % who["ben"]) == "ok" and until(lambda: held(ben.end) == [0])
        check("ada gives ben control of her panel", ok)
        put = carry(ben, by=(-0.2, 0.0, 0.1), yaw=-0.3)
        check("now ben puts it elsewhere, and ada's screen goes there", until(lambda: poses(ada, 1) >= 1 and stands(ben, 116, ada, 1)))
        time.sleep(0.8)
        check("it stays where he put it", near(stood(ben), put, 1e-3) and poses(ben, 116) == placed and poses(ada, 1) == 1)

        # taken back
        check("ada takes control back, and ben's end hears", ada.end.ask("control take 0") == "ok" and until(lambda: held(ben.end) == []))
        before = ben.head()
        carry(ben, by=(-0.3, 0.0, 0.0))
        check("ben puts it elsewhere again: it goes back", until(lambda: poses(ben, 116) == placed + 1 and stands(ben, 116, ada, 1)))
        check("ada's screen stays, and ben, who pushed it aside, flies nowhere", poses(ada, 1) == 1 and near(ben.head(), before, 1e-6))

        # asked through ft-room
        home = reading(ada, 8)
        check("ada asks to fly to ben's panel", room_tool(ada, "fly", "panel", "8") == (0, "ok") and until(lambda: at_spot(ada, home)))
        status, said = room_tool(ada, "fly", "panel", "99")
        check("to a panel that isn't there nobody flies", status != 0 and "no panel 99 in the room" in said)
        check("and only to panels", ada.ask("fly person 0").startswith("error ") and ada.ask("fly panel").startswith("error ") and ada.ask("fly panel x").startswith("error "))
        time.sleep(0.8)
        check("everything in the room stands where it stood, for both", stands(ben, 116, ada, 1) and stands(ada, 116, ben, 2))

        for f in frames:
            check("%s's ft-roompanels ends cleanly" % f.end.name, f.tool.end() == 0)
            trouble = [line for line in f.tool.lines if "Traceback" in line or "Error" in line]
            check("...and printed no error", not trouble)
    finally:
        for f in frames:
            if f.tool:
                f.tool.end()
        ends = [f.end.end() for f in frames]
    check("both ft-together end cleanly", all(status == 0 for status, _ in ends))
    for (status, out), f in zip(ends, frames):
        if status != 0 or "ERROR: " in out:
            print("--- %s ---\n%s" % (f.end.name, out))
    check("with nothing from the sanitizers", not any("ERROR: " in out for _, out in ends))
    roomkit.finish()


if __name__ == "__main__":
    main()
