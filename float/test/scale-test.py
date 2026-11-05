#!/usr/bin/env python3
"""ft-floatd's Meta+scroll, offline: notches that come in before the idle call add up into one
scale change (scale_soon, scale_now), and the ones that sum to nothing change nothing. D-Bus and
GLib are stand-ins (the dev container has no dbus-python); no KWin, no ft-screens.

  float/test/scale-test.py
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
a, b = floatd.Float("a", None, {}), floatd.Float("b", None, {})
d.floats = {"a": a, "b": b}
done = []
d.rescale = lambda f, steps: done.append((f.id, steps))

for _ in range(3):
    d.scale_soon(a, 1)
check("three notches wait for one idle call", len(idle), 1)
run_idle()
check("...and make one change of 3 steps", done, [("a", 3)])
check("...and nothing is left over", (a.steps, d.scaling), (0, False))

done.clear()
d.scale_soon(a, 1)
d.scale_soon(a, -1)
d.scale_soon(b, -2)
run_idle()
check("up and down again: no change; another window's notches are its own", done, [("b", -2)])

done.clear()
d.scale_soon(a, 1)
run_idle()
d.scale_soon(a, 1)
run_idle()
check("notches after the idle call are a change of their own", done, [("a", 1), ("a", 1)])

sys.exit(1 if failed else 0)
