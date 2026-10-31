#!/usr/bin/env python3
"""ft-floatd's settings from frametop.conf (DEE-297): FLOAT_SLOTS="8", quoted as bash allows, is 8
(it stopped ft-floatd before it looked at --slots), and a value that isn't a number warns and
gives the default. HOME is a temporary folder; D-Bus and GLib are stand-ins.

  float/test/conf-test.py
"""
import importlib.util
import os
import sys
import tempfile
import types
from unittest import mock

HOME = tempfile.mkdtemp()
os.environ["HOME"] = HOME
os.makedirs(os.path.join(HOME, ".config"))
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


def args(conf_text, argv=("--screens", "2")):
    with open(floatd.ft_layout.CONF_PATH, "w") as f:
        f.write(conf_text)
    a = floatd.parse_args(list(argv))
    return a.slots, a.margin


check("the conf's path is in the temporary HOME", floatd.ft_layout.CONF_PATH.startswith(HOME), True)
check("no settings: the defaults", args(""), (16, 300))
check('FLOAT_SLOTS="8" (quoted): 8', args('FLOAT_SLOTS="8"\n'), (8, 300))
check("FLOAT_SLOTS='4', FLOAT_MARGIN=120", args("FLOAT_SLOTS='4'   # c\nFLOAT_MARGIN=120\n"), (4, 120))
check("not numbers: the defaults", args("FLOAT_SLOTS=eight\nFLOAT_MARGIN=0x10\n"), (16, 300))
check("FLOAT_SLOTS=8.5: the default", args("FLOAT_SLOTS=8.5\n"), (16, 300))
check("a line bash can't read: left out", args("FLOAT_SLOTS = 8\n"), (16, 300))
check("over 16: 16, as the session", args("FLOAT_SLOTS=40\n"), (16, 300))
check("--slots wins over the conf", args('FLOAT_SLOTS="8"\n', ("--screens", "2", "--slots", "3")), (3, 300))
sys.exit(1 if failed else 0)
