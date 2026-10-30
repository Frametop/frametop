#!/usr/bin/env python3
"""ft-floatd's window filters, offline: a profile keeps Frametop's settings apps (Input
Settings, Remote Displays, Remote Access) like any app, and leaves out Display Settings, where
profiles are saved from, and Frametop's other windows; spotlist lists them all. D-Bus and
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


for app in floatd.PROFILE_APPS:
    check(f"a profile keeps {app}", floatd.recordable(window(app)), True)
check("a profile keeps an app's window", floatd.recordable(window("org.kde.dolphin")), True)
check("a profile keeps a settings app by its class", floatd.recordable(window("", "ft-remote-displays")), True)
check("a profile keeps Remote Access", floatd.recordable(window("org.frametop.RemoteAccess")), True)
for app in ("ft-display-settings", "ft-tour", "ft-recorder", "frametop-panel", "org.kde.plasmashell", "org.kde.ksplashqml"):
    check(f"a profile leaves out {app}", floatd.recordable(window(app)), False)
check("a profile leaves out a settings app's dialog",
      floatd.recordable(window("ft-input-settings", transient=True)), False)
check("spotlist lists the settings apps", floatd.listable(window("ft-input-settings")), True)
check("spotlist lists Display Settings", floatd.listable(window("ft-display-settings")), True)

sys.exit(1 if failed else 0)
