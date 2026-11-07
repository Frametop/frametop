#!/usr/bin/env python3
"""ft-floatd's window filters, offline: a profile keeps app windows and leaves out Frametop's own,
Frametop Settings too (profiles are saved from it); spotlist lists them all. D-Bus and
GLib are stand-ins, as in scale-test.py.

  float/test/recordable-test.py
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


def window(app, cls="", **flags):
    return dict({"normal": True, "app": app, "cls": cls}, **flags)


check("a profile keeps an app's window", floatd.recordable(window("org.kde.dolphin")), True)
for app in ("ft-settings", "ft-tour", "ft-recorder", "frametop-panel", "org.kde.plasmashell", "org.kde.ksplashqml"):
    check(f"a profile leaves out {app}", floatd.recordable(window(app)), False)
check("a profile leaves out Frametop Settings by its class", floatd.recordable(window("", "ft-settings")), False)
check("spotlist lists Frametop Settings", floatd.listable(window("ft-settings")), True)

# A profile saved with one of the settings apps Frametop Settings replaced opens without it.
launched = []
me = types.SimpleNamespace(windows={}, window_key=lambda ev: "", place_entry=lambda ev, e: None,
                           launch_entries=lambda entries: launched.append([e.get("app") or e["cmd"] for e in entries]))
profile = {"windows": [{"app": "ft-input-settings"}, {"app": "ft-remote-displays"},
                       # Remote Access (GTK) matched no desktop file: saved by command line and class.
                       {"cmd": ["/usr/bin/python3", "/x/remote/ft-remote-settings"], "class": "org.frametop.RemoteAccess"},
                       {"app": "org.kde.dolphin", "screen": 1}]}
with mock.patch.object(floatd.ft_layout, "load_layout", return_value={"profiles": {"Desk": profile}}):
    check("a profile opens", floatd.Daemon.open_profile(me, "Desk"), "ok")
check("its retired settings apps stay closed", launched, [["org.kde.dolphin"]])

sys.exit(1 if failed else 0)
