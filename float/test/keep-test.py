#!/usr/bin/env python3
"""ft-floatd keeps a floating window's size, density and scale a second after they change, not only
when its panel goes (a desktop that stops closes it without saying so). Not when it opens, and not for
a window that closed meanwhile. D-Bus and GLib are stand-ins; ft-screens is a fake.

  float/test/keep-test.py
"""
import importlib.util
import os
import sys
import types
from unittest import mock

HERE = os.path.dirname(os.path.realpath(__file__))
timers = {}  # GLib.timeout_add's callbacks still due, run by hand
added = []   # ...and how long each was set for


def timeout_add(ms, fn):
    added.append(ms)
    timers[len(added)] = fn
    return len(added)


glib = types.SimpleNamespace(idle_add=mock.MagicMock(), timeout_add=timeout_add, timeout_add_seconds=mock.MagicMock(),
                             source_remove=lambda n: timers.pop(n), io_add_watch=mock.MagicMock())
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


def second():
    for n, fn in list(timers.items()):
        timers.pop(n)
        check("a timer runs once", fn(), False)


d = floatd.Daemon.__new__(floatd.Daemon)  # (no sockets, no KWin)
d.screens = types.SimpleNamespace(ask=lambda text, quiet=False:
                                  "ok 1 1:2000x1000:1.6" if text == "screens" else "ok 0 1 -2 1 0 0 0 1 0 0 0 1 1 1 0 none")
d.margin, d.places, d.screens_n, d.pending, d.waiter = 300, {}, 1, [], None
d.primary = lambda: 0
d.command = lambda **cmd: None
floatd.kscreen = lambda *a: None
saves = []
floatd.save_places = lambda places: saves.append({k: (v["pixels"], v["scale"], v["mpp"]) for k, v in places.items()})

slot = floatd.Slot(1, 1)
f = floatd.Float("w1", slot, {"output": "WL-0"})
f.app, f.mpp = "org.kde.dolphin", 0.0008
slot.window, d.floats = f, {"w1": f}


def sized(w, h):
    x, y = slot.pos[0] + 300, slot.pos[1] + 300
    d.follow(f, {"frame": {"x": x, "y": y, "w": w, "h": h}, "client": {"x": x, "y": y + 30, "w": w, "h": h - 30},
                 "outputRect": {"x": slot.pos[0], "y": slot.pos[1], "w": w + 600, "h": h + 600}})


sized(800, 600)
check("opening keeps nothing", len(timers), 0)
for w in (810, 900, 1000):
    sized(w, 700)
check("a resize: one timer, at its last size", (len(timers), added[-1]), (1, floatd.KEEP_MS))
check("...nothing kept on the way", saves, [])
second()
check("...kept", saves, [{"org.kde.dolphin": ([1000, 700], 1.0, 0.0008)}])
sized(1000, 700)
check("the same size again: nothing to keep", len(timers), 0)
d.redensity(f, 1)
second()
check("a notch on the corner tab", saves[-1:], [{"org.kde.dolphin": ([1000, 700], 1.0, round(0.0008 * 1.05, 8))}])
d.set_scale(f, 1.5)
second()
check("a scale", saves[-1]["org.kde.dolphin"][1], 1.5)
n = len(saves)
f.scale = 1.0
sized(1200, 700)
del d.floats["w1"]
second()
check("gone before the second was up: not kept here (release kept it)", len(saves), n)

sys.exit(1 if failed else 0)
