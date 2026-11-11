#!/usr/bin/env python3
"""ft-roompanels between two real ft-together on this machine's loopback, each with a stand-in
for its Frame's ft-screens (fakescreens.py): what one shares stands at the other's where the
room says, both ways, and goes when it is taken back, when its owner leaves and when
ft-roompanels ends; and one of them moves through the room (go, pull) while everything in it
stays where the room has it for both. No video goes: that is panels-test.py.

    roompanels-test.py FT-TOGETHER
"""
import math
import os
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
import roomkit  # noqa: E402
from fakescreens import Screens, serve  # noqa: E402
from roomkit import check  # noqa: E402
from roompanels import moving, placing  # noqa: E402


class Frame:
    """One person's Frame: ft-together, a stand-in ft-screens, and ft-roompanels between them."""

    def __init__(self, program, name, head):
        self.end = roomkit.End(program, name)
        self.screens = Screens()
        self.screens.head = head
        self.name = "ft_test_%d_%s" % (os.getpid(), name)
        self.lock = serve(self.screens, self.name + "_screens")
        self.tool = None

    def keep(self):
        self.tool = roomkit.Tool([sys.executable, str(HERE.parent / "ft-roompanels"), "--socket", self.name + "_panels",
                                  "--together", self.end.sock, "--screens", self.name + "_screens"])

    def ask(self, text):
        return self.end.client.ask(self.name + "_panels", text) or "none"

    def floor(self):
        words = self.end.ask("floor").split()
        return tuple(float(w) for w in words[1:]) if len(words) == 4 else None

    def head(self):
        """The head in the room: (x, z, the way it faces)."""
        with self.lock:
            x, _, z, degrees = (float(w) for w in self.screens.head.split()[1:])
        floor = self.floor()
        at = placing.turned(floor[2], (x, 0.0, z))
        return (at[0] + floor[0], at[2] + floor[1], placing.wrap(math.radians(degrees) + floor[2]))

    def until(self, what, seconds=5.0):
        """Whether what(the stand-in ft-screens) comes true in time."""
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            with self.lock:
                if what(self.screens):
                    return True
            time.sleep(0.05)
        return False


def near(a, b, by=2e-3):
    return len(a) == len(b) and all(abs(x - y) <= by for x, y in zip(a, b))


def stands(viewer, number, owner, screen):
    """Whether the viewer's remote screen `number` stands where the owner's `screen` does: both
    said in the room, each through its own Frame's floor."""
    with viewer.lock:
        pose = list(viewer.screens.panels.get(number, {}).get("pose", []))
    with owner.lock:
        at, yaw = owner.screens.at[screen], owner.screens.yaw.get(screen, 0.0)
    if len(pose) != 12 or viewer.floor() is None or owner.floor() is None:
        return False
    want = placing.to_room(owner.floor(), {"at": at, "x": placing.turned(yaw, (1, 0, 0)), "y": (0, 1, 0), "z": placing.turned(yaw, (0, 0, 1)), "metres": 1.2, "curve": 0.0})
    got = placing.to_room(viewer.floor(), {"at": pose[3::4], "x": pose[0::4], "y": pose[1::4], "z": pose[2::4], "metres": 1.2, "curve": 0.0})
    turn = near(got["turn"], want["turn"], 5e-3) or near(got["turn"], [-v for v in want["turn"]], 5e-3)
    return near(got["at"], want["at"], 5e-3) and turn


def at_spot(frame, spot):
    """Whether the frame's head is over `spot` (x, z, yaw) of the room, facing its way."""
    x, z, facing = frame.head()
    return near((x, z), spot[:2], 5e-3) and abs(placing.wrap(facing - spot[2])) < 5e-3


def panel_in_room(end, ident):
    return next(([p["at"], p["turn"]] for p in end.state()["panels"] if p["id"] == ident), None)


def moves(ada, ben):
    """Ben moves through the room. Ada's screen 1 is the room's panel 0, ben's screen 2 its panel 8."""
    told = panel_in_room(ada.end, 8)
    with ben.lock:
        placed = ben.screens.count("pose 116")
    check("ben goes to where ada's panel reads well", ben.ask("go panel 0").startswith("ok "))
    check("his head is there, facing it", at_spot(ben, moving.reading(next(p for p in ben.end.state()["panels"] if p["id"] == 0))))
    check("ada's panel stands at ben's where her screen stands, as before", stands(ben, 116, ada, 1))
    watcher = roomkit.Watcher(ada.end)
    watcher.send(roomkit.pose(2.0, z=-1.0))
    check("ft-together knows where ada is from her poses", ben.end.until(lambda s: ben.end.ask("where") == "ok 1 0:2.000:1.600:-1.000:0.0000"))
    check("ben goes to ada", ben.ask("go person 0").startswith("ok "))
    check("he stands in front of her, facing her", at_spot(ben, moving.beside((2.0, -1.0), 0.0)))
    was, before = ben.floor(), ben.head()
    check("ben pulls the room past him", ben.ask("pull 0.5 -0.25 20").startswith("ok "))
    check("his floor lies where the pull's way back puts it", near(ben.floor(), placing.then(was, placing.back((0.5, -0.25, math.radians(20)))), 1e-4))
    check("so he faces the other way round by as much", abs(placing.wrap(ben.head()[2] - before[2] + math.radians(20))) < 1e-3)
    time.sleep(0.8)    # a few looks of each ft-roompanels
    now = panel_in_room(ada.end, 8)
    check("ben's own shared screen is where the room had it, for all his moving",
          near(now[0], told[0], 3e-3) and (near(now[1], told[1], 3e-3) or near(now[1], [-v for v in told[1]], 3e-3)))
    check("it stands at ada's where his screen stands", stands(ada, 116, ben, 2))
    check("ada's panel still stands at ben's where her screen stands", stands(ben, 116, ada, 1))
    with ben.lock:
        again = ben.screens.count("pose 116")
    check("and was moved with the room, not placed again", again == placed)
    check("a move that isn't one is refused", ben.ask("go there").startswith("error go ") and ben.ask("pull 9 9 0") == "error that is more than a pull")
    watcher.close()


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip())
    ada = Frame(sys.argv[1], "ada", "ok 0.2000 1.6000 0.1000 30.00")
    ben = Frame(sys.argv[1], "ben", "ok -0.4000 1.7000 0.3000 -120.00")
    frames = (ada, ben)
    try:
        # Something an earlier ft-roompanels left, and a remote display of ben's own.
        ben.screens.remotes = {116: ["peer-3", "1280x720"], 101: ["mac", "1920x1080"]}
        ben.screens.shares = {2: (1, "1920x1080")}
        for f in frames:
            f.keep()
        check("each ft-roompanels starts in no room", all(f.tool.said("in no room") for f in frames))
        check("and clears what an earlier one left, but no remote display",
              ben.until(lambda s: s.remotes == {101: ["mac", "1920x1080"]} and s.shares == {}))
        check("nothing is shared outside a room", ada.ask("share 1") == "error not in a room")

        check("ada opens a room and ben comes in", ada.end.ask("open") == "ok" and roomkit.let_in(ada.end, ben.end)
              and ada.end.until(roomkit.linked) and ben.end.until(roomkit.linked))
        check("each ft-roompanels sees the room", all(f.tool.said("in a room") for f in frames))
        check("and lays its Frame's floor from where the head is", ada.end.until(lambda s: ada.floor() is not None)
              and ben.end.until(lambda s: ben.floor() is not None))
        spot = [p["spot"] for p in ben.end.state()["people"] if p["name"] == "ben"][0]
        check("ben's floor puts his head on his spot", near(ben.floor(), placing.lay(spot, (-0.4, 0.3, math.radians(-120))), 1e-3))

        check("ada shares her screen 1", ada.ask("share 1") == "ok 0")
        check("it is in the room, as a 1920x800 video", ben.end.until(lambda s: [(p["id"], p["size"], p["name"]) for p in s["panels"]] == [(0, [1920, 800], "Screen 1")]))
        check("ada's ft-screens sends its pictures", ada.until(lambda s: s.shares == {1: (0, "1920x800")}))
        check("ben's ft-screens has a panel for it, the last remote screen", ben.until(lambda s: s.remotes.get(116) == ["peer-0", "1920x800"] and "curve" in s.panels[116]))
        check("standing where ada's screen stands", stands(ben, 116, ada, 1))
        with ben.lock:
            panel = dict(ben.screens.panels[116])
        check("as wide as hers, flat, with its name", (panel["width"], panel["curve"], panel["label"]) == ([1.2], [0.0], "Screen 1"))
        check("ada isn't shown her own panel", ada.until(lambda s: s.remotes == {}, 1.0))

        check("ben shares his screen 2", ben.ask("share 2") == "ok 0")
        check("ada's ft-screens has a panel for it", ada.until(lambda s: s.remotes.get(116) == ["peer-8", "1920x1080"] and "curve" in s.panels[116]))
        check("standing where ben's screen stands", stands(ada, 116, ben, 2))
        check("each ft-roompanels says which screen here shows each of the room's panels, its own as large as the desktop has it",
              ada.end.until(lambda s: ada.ask("panels") == "ok 2 0:1:3440x1440 8:116:1920x1080")
              and ben.end.until(lambda s: ben.ask("panels") == "ok 2 8:2:1920x1080 0:116:1920x800"))

        with ada.lock:
            ada.screens.at[1] = (0.8, 1.2, -1.5)
        check("ada moves her screen and it moves at ben's", ben.end.until(lambda s: stands(ben, 116, ada, 1)))
        with ben.lock:
            asked = sum(1 for a in ben.screens.asked if a.startswith("remote 116 peer"))
        check("on the same remote screen", asked == 1)

        moves(ada, ben)

        state = ada.ask("state")
        check("state tells it", state.startswith('ok {"room": true') and '"shares": [{"screen": 1, "panel": 0, "told": true}]' in state
              and '"panels": [{"id": 8, "screen": 116, "placed": true}]' in state)
        check("a question it doesn't know is refused", ada.ask("share one").startswith("error") and ada.ask("remote 101 stop").startswith("error"))

        check("ada takes her screen back", ada.ask("unshare 1") == "ok")
        check("its panel goes at ben's, his remote display stays", ben.until(lambda s: sorted(s.remotes) == [101]))
        check("and ada's ft-screens stops sending", ada.until(lambda s: s.shares == {}))

        check("ada shares it again", ada.ask("share 1") == "ok 0" and ben.until(lambda s: 116 in s.remotes))
        check("ben leaves", ben.end.ask("leave") == "ok")
        check("his ft-roompanels takes the room's panels away and shares nothing", ben.tool.said("in no room", since=2)
              and ben.until(lambda s: sorted(s.remotes) == [101] and s.shares == {}))
        check("and his screens stand where they stood before he moved through the room",
              ben.until(lambda s: s.shifted == ["back"] and near(s.at[1], (0.0, 1.5, -2.0), 1e-3) and near(s.at[2], (1.5, 1.5, -1.5), 1e-3)))
        check("and his panel goes at ada's", ada.until(lambda s: s.remotes == {}))
        check("ada's own share stays", ada.until(lambda s: s.shares == {1: (0, "1920x800")}, 1.0))

        check("ada's ft-roompanels ends cleanly", ada.tool.end() == 0)
        check("and leaves nothing behind in ft-screens", ada.until(lambda s: s.shares == {} and s.remotes == {}, 1.0))
        check("ben's ends cleanly", ben.tool.end() == 0)
        for f in frames:
            trouble = [line for line in f.tool.lines if "Traceback" in line or "Error" in line]
            check("%s's ft-roompanels printed no error" % f.end.name, not trouble)
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
