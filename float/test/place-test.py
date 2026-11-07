#!/usr/bin/env python3
"""ft-floatd's place for a window floated from a screen, offline: where it was on the screen, a little in
front of it, facing you as the screen does at its centre (DEE-336), not the screen's own way, which a
window near a big screen's edge would show turned away from you. D-Bus and GLib are stand-ins.

  float/test/place-test.py
"""
import importlib.util
import math
import os
import sys
import types
from unittest import mock

HERE = os.path.dirname(os.path.realpath(__file__))
glib = types.SimpleNamespace(idle_add=mock.MagicMock(), timeout_add=mock.MagicMock(), timeout_add_seconds=mock.MagicMock(),
                             source_remove=mock.MagicMock(), io_add_watch=mock.MagicMock())
repository = types.SimpleNamespace(GLib=glib, Gio=mock.MagicMock())
for name, module in {"dbus": mock.MagicMock(), "dbus.mainloop": mock.MagicMock(),
                     "dbus.mainloop.glib": mock.MagicMock(), "dbus.service": mock.MagicMock(),
                     "gi": types.SimpleNamespace(repository=repository), "gi.repository": repository}.items():
    sys.modules[name] = module
sys.path.insert(0, os.path.dirname(HERE))  # (ft_apps, next to it)
spec = importlib.util.spec_from_file_location("ft_floatd", os.path.join(HERE, "..", "ft_floatd.py"))
floatd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(floatd)

failed = 0


def check(what, ok, detail=""):
    global failed
    failed += not ok
    print(f"{'ok  ' if ok else 'FAIL'}  {what}" + ("" if ok else f": {detail}"))


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def unit(v):
    n = math.sqrt(dot(v, v))
    return [x / n for x in v]


def close(a, b, tol=1e-6):
    return all(abs(x - y) < tol for x, y in zip(a, b))


EYE = (0.0, 1.6, 0.0)


class Screens:
    def __init__(self, screen, head=True):
        self.screen, self.head, self.posed = screen, head, None

    def ask(self, text, quiet=False):
        if text == "head":
            return f"ok {EYE[0]} {EYE[1]} {EYE[2]} 0 0" if self.head else "error no head pose"
        if text.startswith("get "):
            c, x, y, z = self.screen
            return "ok " + " ".join(f"{v:.6f}" for v in (*c, *x, *y, *z)) + " 2.4 1.35 0 none"
        if text.startswith("pose "):
            self.posed = [float(v) for v in text.split()[2:]]
        return "ok"


def place(screen, frame_x, head=True):
    """Float a 600x400 window from x = frame_x on a 2560x1440 screen (scale 1, 1 mm a pixel): its pose."""
    d = floatd.Daemon.__new__(floatd.Daemon)  # (no sockets, no KWin)
    d.screens = Screens(screen, head)
    f = types.SimpleNamespace(slot=types.SimpleNamespace(index=4), scale=1.0, mpp=0.001)
    ev = {"output": "WL-0", "outputRect": {"x": 0, "y": 0, "w": 2560, "h": 1440},
          "frame": {"x": frame_x, "y": 520, "w": 600, "h": 400}}
    d.place_panel(f, ev)
    r = d.screens.posed  # rows: x y z centre
    return [r[3], r[7], r[11]], [r[0], r[4], r[8]], [r[1], r[5], r[9]], [r[2], r[6], r[10]]


# A screen 2 m ahead at eye height, facing you.
facing = ((0.0, 1.6, -2.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))
c, x, y, z = place(facing, 0)   # the window at the screen's left edge
check("it stays where it was on the screen, a little in front", close(c, [-0.98, 1.6, -2.0 + floatd.PULL_OUT], 1e-3), c)
check("it faces you, not the screen's way", close(z, unit([EYE[k] - c[k] for k in range(3)]), 1e-5), z)
check("...level, upright", abs(x[1]) < 1e-6 and y[1] > 0.99, (x, y))
check("...where the screen's way would leave it 30 degrees off",
      abs(math.degrees(math.acos(dot((0, 0, 1), unit([EYE[k] - c[k] for k in range(3)])))) - 30) < 0.5, c)
c, x, y, z = place(facing, 980)   # in the middle
check("in the middle of the screen it keeps the screen's way", close(z, [0, 0, 1], 1e-6) and close(x, [1, 0, 0], 1e-6), z)

# A screen tipped back 15 degrees (it looks up at you a little more): the window keeps that tip.
t = math.radians(15)
tipped = ((0.0, 1.6, -2.0), (1.0, 0.0, 0.0), (0.0, math.cos(t), -math.sin(t)), (0.0, math.sin(t), math.cos(t)))
c, x, y, z = place(tipped, 0)
to_eye = unit([EYE[k] - c[k] for k in range(3)])
check("a tip the screen has stays", abs(math.degrees(math.acos(dot(z, to_eye))) - 15) < 1e-3, z)
check("...about a level right edge", abs(x[1]) < 1e-6, x)
check("...rigid (to the 5 places a pose is sent with)", all(abs(dot(a, b) - (i == j)) < 1e-4 for i, a in enumerate((x, y, z)) for j, b in enumerate((x, y, z))),
      (x, y, z))

# No head pose: the screen's way, as before.
c, x, y, z = place(facing, 0, head=False)
check("no head pose: the screen's own way", close(z, [0, 0, 1]) and close(x, [1, 0, 0]), z)

sys.exit(1 if failed else 0)
