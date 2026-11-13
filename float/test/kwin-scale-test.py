#!/usr/bin/env python3
"""ft-floatd's output scales and sizes, offline, as KWin 6.6 shows them: kscreen-doctor gives a
nested output's stored scale setting, 1 whatever its scale is, so a screen's scale comes from the
layout (ft_layout.kwin_scales) and a spare's from kscreen-doctor. D-Bus and GLib are stand-ins; no
KWin, no ft-screens.

  float/test/kwin-scale-test.py
"""
import importlib.util
import json
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


# One screen at 200% (3840x2160 px, 1920x1080 logical) and a spare, as KWin 6.6 shows them.
outputs = [{"name": "WL-0", "scale": 1, "pos": {"x": 0, "y": 0}, "size": {"width": 3840, "height": 2160}},
           {"name": "WL-1", "scale": 1, "pos": {"x": 0, "y": 4000}, "size": {"width": 640, "height": 480}}]
shown = json.dumps({"outputs": outputs})
floatd.kscreen = lambda *a: shown
floatd.ft_layout.load_layout = lambda: {"screens": [{"size": [3840, 2160], "metres": 2.8, "scale": 2.0}]}
floatd.ft_layout.backend = lambda: "screens"

check("a screen's scale is the layout's, a spare's as shown", floatd.output_scales(), {"WL-0": 2.0, "WL-1": 1.0})
rects = floatd.output_rects()
check("a screen's logical size is its pixels over the layout's scale", rects["WL-0"], (0, 0, 1920.0, 1080.0))
check("a spare's is its pixels over the scale shown", rects["WL-1"], (0, 4000, 640.0, 480.0))

shown = ""
check("without kscreen-doctor, the screens' scales still come from the layout", floatd.output_scales(), {"WL-0": 2.0})
check("...and there are no places", floatd.output_rects(), {})

shown = json.dumps({"outputs": outputs})
floatd.ft_layout.backend = lambda: "gamescope"
check("gamescope: the scales as shown", floatd.output_scales(), {"WL-0": 1.0, "WL-1": 1.0})

sys.exit(1 if failed else 0)
