#!/usr/bin/env python3
"""ft-floatd's density, offline: scroll on a floating window's corner tab ("density N
STEPS") makes its panel bigger or smaller at the same pixels, 5% a notch, from half to twice its
screen's density, and sends the same crop again; an app floats again (the float key, the title bar
button) at the density it last had. D-Bus and GLib are stand-ins; ft-screens is a fake.

  float/test/density-test.py
"""
import importlib.util
import os
import sys
import types
from unittest import mock

HERE = os.path.dirname(os.path.realpath(__file__))
idle = []  # GLib.idle_add's callbacks, run by hand
glib = types.SimpleNamespace(idle_add=lambda fn, *a: idle.append(fn) or len(idle),
                             timeout_add=mock.MagicMock(), timeout_add_seconds=mock.MagicMock(),
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


def run_idle():
    while idle:
        idle.pop(0)()


d = floatd.Daemon.__new__(floatd.Daemon)  # (no sockets, no KWin)
d.scaling = False


class Screens:
    """ft-screens: one screen, 2000 px over 1.6 m (0.8 mm a pixel)."""
    def __init__(self):
        self.sent = []

    def ask(self, text, quiet=False):
        self.sent.append(text)
        return "ok 1 1:2000x1000:1.6" if text == "screens" else "ok"


BASE = 1.6 / 2000
d = floatd.Daemon.__new__(floatd.Daemon)  # (no sockets, no KWin)
d.screens, d.places, d.screens_n = Screens(), {}, 1
d.primary = lambda: 0
f = floatd.Float("w1", types.SimpleNamespace(index=2), {"output": "WL-0"})
f.mpp = BASE
d.floats = {"w1": f}
d.by_panel = lambda n: f if n == 2 else None
d.set_panel(f, (10, 10, 1200, 800), title=30)

d.screens.sent.clear()
check("a notch up", d.request("density 2 1"), "ok")
check("...is 5% bigger", round(f.mpp / BASE, 6), 1.05)
check("...the same crop, at the new density", d.screens.sent[-1], f"float 2 {f.mpp:.7f} 10 10 1200 800 30")
d.request("density 2 -2")
check("two down", round(f.mpp / BASE, 6), round(1.05 ** -1, 6))
for _ in range(40):
    d.request("density 2 1")
check("no more than twice the screen's", round(f.mpp / BASE, 6), 2.0)
for _ in range(80):
    d.request("density 2 -1")
check("no less than half", round(f.mpp / BASE, 6), 0.5)
d.screens.sent.clear()
d.request("density 2 -1")
check("at the end: nothing sent", [t for t in d.screens.sent if t.startswith("float")], [])
check("not a number", d.request("density 2 x"), "ok")
check("...changes nothing", round(f.mpp / BASE, 6), 0.5)
check("no such panel", d.request("density 7 1"), "error no floating window on screen 7")

d.places = {"org.kde.dolphin": {"mpp": BASE * 1.5}, "odd": {"mpp": BASE * 9}}
check("an app's remembered density", round(d.app_mpp("org.kde.dolphin", "WL-0") / BASE, 6), 1.5)
check("...kept to the range", round(d.app_mpp("odd", "WL-0") / BASE, 6), 2.0)
check("an app never floated: the screen's", round(d.app_mpp("new", "WL-0") / BASE, 6), 1.0)
check("no app: the screen's", round(d.app_mpp("", "WL-0") / BASE, 6), 1.0)

sys.exit(1 if failed else 0)
