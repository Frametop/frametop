"""One person's Frame for the tests that run ft-roompanels between real ft-together programs
(roompanels-test.py, roomcarry-test.py): ft-together, a stand-in for the Frame's ft-screens
(fakescreens.py) and ft-roompanels between them, and where things stand in the room as each
Frame has them.
"""
import math
import os
import sys
import time
from pathlib import Path

import roomkit
from fakescreens import Screens, serve
from roompanels import placing

HERE = Path(__file__).resolve().parent


class Frame:
    """One person's Frame: ft-together, a stand-in ft-screens, and ft-roompanels between them."""

    def __init__(self, program, name, head):
        self.end = roomkit.End(program, name)
        self.screens = Screens()
        self.screens.head = head
        self.name = "ft_test_%d_%s" % (os.getpid(), name)
        self.lock = serve(self.screens, self.name + "_screens")
        self.tool = None

    def keep(self, layout="/nowhere/layout.json", hosts="/nowhere/hosts"):
        """Runs its ft-roompanels: with no saved layout and no PC's token, unless a test has them."""
        self.tool = roomkit.Tool([sys.executable, str(HERE.parent / "ft-roompanels"), "--socket", self.name + "_panels",
                                  "--together", self.end.sock, "--screens", self.name + "_screens", "--layout", layout, "--hosts", hosts])

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
