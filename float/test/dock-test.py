#!/usr/bin/env python3
"""ft-floatd's docking by carrying, offline: "dockat PANEL SCREEN X Y GX GY" from ft-screens puts
the window on that screen with the point it was held by at X, Y, at its size in pixels (smaller where the
screen is), all of it on the screen. D-Bus and GLib are stand-ins; KWin's outputs are made up.

  float/test/dock-test.py
"""
import importlib.util
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


def check(what, got, want):
    global failed
    ok = got == want
    failed += not ok
    print(f"{'ok  ' if ok else 'FAIL'}  {what}" + ("" if ok else f": got {got!r}, want {want!r}"))


# Two screens: WL-0 2560x1440 at scale 1, WL-1 at 3840 px wide, scale 2 (1920 logical), right of it.
floatd.output_rects = lambda: {"WL-0": (0, 0, 2560, 1440), "WL-1": (2560, 0, 1920, 1080), "WL-2": (9000, 0, 1000, 800)}
floatd.output_scales = lambda: {"WL-0": 1.0, "WL-1": 2.0, "WL-2": 1.25}
d = floatd.Daemon.__new__(floatd.Daemon)  # (no sockets, no KWin)
d.screens_n = 2
placed = []
d.dock = lambda f, frame=None, output=None, maximized=False: placed.append((output, frame))
f = floatd.Float("w1", types.SimpleNamespace(index=3), {"output": "WL-0"})
f.frame, f.scale, f.normal = {"x": 9000, "y": 0, "w": 800, "h": 500}, 1.25, (1000, 625)
f.move_from = {"x": 9000, "y": 0}
d.floats = {"w1": f}
d.by_panel = lambda n: f if n == 3 else None

check("onto screen 1", d.request("dockat 3 1 1000 700 500 20"), "ok")
check("...held at its title bar, there", placed[-1], ("WL-0", {"x": 500, "y": 680, "w": 1000, "h": 625}))
check("...and not back onto its spare after the carry", f.move_from, None)
d.request("dockat 3 1 100 1400 500 20")
check("near the corner: all of it on the screen", placed[-1], ("WL-0", {"x": 0, "y": 815, "w": 1000, "h": 625}))
d.request("dockat 3 2 1920 1080 500 20")
check("a scale 2 screen: the same pixels, half the logical size", placed[-1], ("WL-1", {"x": 3270, "y": 530, "w": 500, "h": 312}))
f.normal = (5000, 3000)
d.request("dockat 3 1 1280 720 2500 10")
check("bigger than the screen: as big as it", placed[-1], ("WL-0", {"x": 0, "y": 0, "w": 2560, "h": 1440}))
n = len(placed)
d.request("dockat 3 3 10 10 0 0")
check("not a screen (a spare output): nothing", len(placed), n)
check("no such panel", d.request("dockat 9 1 0 0 0 0"), "error no floating window on screen 9")
check("not numbers", d.request("dockat 3 1 a b 0 0"), "error unknown command")

sys.exit(1 if failed else 0)
