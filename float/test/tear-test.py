#!/usr/bin/env python3
"""ft-floatd's half of tearing a window off its screen, offline (DEE-246C): KWin's "screen-move" goes on to
ft-screens as "moving", ft-screens' "tear ID X Y" asks KWin to float the window, and once it floats,
ft-screens hears "torn PANEL GX GY": where in the window its laser held it. D-Bus and GLib are stand-ins;
KWin's outputs are made up.

  float/test/tear-test.py
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


class Screens:
    def __init__(self):
        self.sent = []

    def ask(self, text, quiet=False):
        self.sent.append(text)
        return "ok"


# One screen, WL-0: 2560x1440 px at scale 2 (1280x720 logical); the spare WL-3 at scale 1.
floatd.output_scales = lambda: {"WL-0": 2.0, "WL-3": 1.0}
floatd.kscreen = lambda *a: None
floatd.desktop_name = lambda app, cls: app
d = floatd.Daemon.__new__(floatd.Daemon)  # (no sockets, no KWin)
d.screens, d.places, d.screens_n, d.margin = Screens(), {}, 1, 4
d.gone, d.windows, d.floats, d.tear = set(), {}, {}, None
commands = []
d.command = lambda **kw: commands.append(kw)
slot = types.SimpleNamespace(index=4, output="WL-3", kscale=1.0, pos=(9000, 0), window=None, want=None)
d.free_slot = lambda: slot
d.set_size = d.rescaled = d.place_panel = lambda *a, **kw: None
d.app_mpp = lambda app, output: 0.0008
ev = {"ev": "window", "id": "w1", "app": "org.kde.dolphin", "cls": "dolphin", "output": "WL-0",
      "outputRect": {"x": 0, "y": 0, "w": 1280, "h": 720}, "frame": {"x": 400, "y": 300, "w": 600, "h": 400},
      "client": {"x": 400, "y": 330, "w": 600, "h": 370}}
d.windows["w1"] = ev

d.on_event({"ev": "screen-move", "id": "w1", "moving": True})
check("KWin moves it on its screen: ft-screens hears", d.screens.sent[-1], "moving w1 1")
d.on_event({"ev": "screen-move", "id": "w1", "moving": False})
check("...and when it stops", d.screens.sent[-1], "moving w1 0")
check("...which isn't the window's state", d.windows["w1"], ev)

check("torn off at 1300, 760 on its screen", d.request("tear w1 1300 760"), "ok")
check("...KWin's asked to float it", commands[-1], {"cmd": "request-float", "id": "w1"})
check("not a window on the screens", d.request("tear w9 1 1"), "error no window w9 on the screens")
check("not numbers", d.request("tear w1 x 1"), "error tear ID X Y")

d.screens.sent.clear()
f = d.float_window(ev)
torn = [t for t in d.screens.sent if t.startswith("torn")]
# (1300, 760) px is (650, 380) on the screen in KWin's units, (250, 80) into the window, 500, 160 of its pixels
check("floated: ft-screens carries it on, held where it was", torn, ["torn 4 500 160"])
check("...after its panel is set", d.screens.sent.index(torn[0]) > d.screens.sent.index(next(t for t in d.screens.sent if t.startswith("float"))), True)
check("...once", d.tear, None)
check("now floating: no tearing it again", d.request("tear w1 1 1"), "error no window w1 on the screens")

d.floats.clear(), d.screens.sent.clear()
slot.window = None
d.float_window(ev)
check("floated another way (the float key): no carry", [t for t in d.screens.sent if t.startswith("torn")], [])

sys.exit(1 if failed else 0)
