#!/usr/bin/env python3
"""Offline characterization test of ft-floatd's socket requests (Daemon.request): for each
request, what it answers, which of the daemon's parts it reaches and with what, and whether it
starts a tear-off.

The expected results are in request-golden.json, recorded from ft-floatd before its requests
were split into one handler per command (docs/refactor/PROGRESS.md). A change that means to
change a request records them again with --record, and the diff of that file shows what
changed. D-Bus and GLib are stand-ins, and the daemon's parts are recorders: no KWin, sockets or
windows.

  float/test/request-test.py [--record] [FLOATD]   (default: float/ft_floatd.py next to this folder)
"""
import importlib.util
import json
import os
import sys
import types
from unittest import mock

HERE = os.path.dirname(os.path.realpath(__file__))
GOLDEN = os.path.join(HERE, "request-golden.json")
args = sys.argv[1:]
record = "--record" in args
paths = [a for a in args if a != "--record"]
FLOATD = paths[0] if paths else os.path.join(HERE, "..", "ft_floatd.py")
glib = types.SimpleNamespace(idle_add=mock.MagicMock(), timeout_add=mock.MagicMock(), timeout_add_seconds=mock.MagicMock(),
                             source_remove=mock.MagicMock(), io_add_watch=mock.MagicMock())
repository = types.SimpleNamespace(GLib=glib, Gio=mock.MagicMock())
for name, module in {"dbus": mock.MagicMock(), "dbus.mainloop": mock.MagicMock(),
                     "dbus.mainloop.glib": mock.MagicMock(), "dbus.service": mock.MagicMock(),
                     "gi": types.SimpleNamespace(repository=repository), "gi.repository": repository}.items():
    sys.modules[name] = module
sys.path.insert(0, os.path.dirname(HERE))  # (ft_apps, next to it)
spec = importlib.util.spec_from_file_location("ft_floatd", FLOATD)
floatd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(floatd)

# The daemon's methods a request hands off to: recorded, not run. Each returns what the real
# one's caller reads: launch a (reply, ...) tuple, the rest a reply.
HANDOFFS = {"launch": ("ok launched", None), "dash_launch": "ok dash", "capture": "ok captured",
            "open_profile": "ok profile", "command": None, "dock": None, "dock_at": None, "scale_soon": None,
            "redensity": None, "hold": None}


def plain(v):
    if isinstance(v, dict):
        return {str(k): plain(x) for k, x in v.items()}
    if isinstance(v, (list, tuple)):
        return [plain(x) for x in v]
    if isinstance(v, floatd.Float):
        return f"<float {v.id}>"
    if isinstance(v, types.SimpleNamespace):  # a slot
        return plain(vars(v))
    if isinstance(v, float):
        return round(v, 6)
    return v


def new_daemon():
    d = floatd.Daemon.__new__(floatd.Daemon)  # (no sockets, no KWin)
    calls = []
    for name, ret in HANDOFFS.items():
        setattr(d, name, (lambda n, r: lambda *a, **k: calls.append([n, plain(list(a)), plain(k)]) or r)(name, ret))
    d.screens_n = 2
    d.windows = {"w1": {"id": "w1"}, "w2": {"id": "w2"}, "w3": {"id": "w3"}}
    f = floatd.Float("w2", types.SimpleNamespace(index=3), {"output": "WL-0"})
    f.frame, f.scale = {"x": 10, "y": 20, "w": 800, "h": 500}, 1.25
    nf = floatd.Float("w3", types.SimpleNamespace(index=4), {"output": "WL-1"})
    nf.frame, nf.scale = None, 1.0
    d.floats = {"w2": f, "w3": nf}
    d.by_panel = lambda n: {3: f, 4: nf}.get(n)
    d.slots = [types.SimpleNamespace(output="FT-0", window=types.SimpleNamespace(id="w2")),
               types.SimpleNamespace(output="FT-1", window=None)]
    d.loop = types.SimpleNamespace(quit="loop.quit")
    d.tear = None
    return d, calls


def run(text):
    d, calls = new_daemon()
    glib.idle_add.reset_mock()
    try:
        reply, raised = d.request(text, sender="SENDER"), None
    except Exception as e:
        reply, raised = None, type(e).__name__
    idle = [plain(list(c.args)) for c in glib.idle_add.call_args_list]
    return {"reply": reply, "calls": calls, "idle_add": idle, "tear": plain(d.tear), "raised": raised}


CASES = [
    "", "  ", "nonsense", "launch", "launch app.desktop", "launch app.desktop action=new", "launch app.desktop screen=2",
    "launch app.desktop screen=3", "launch app.desktop screen=0", "launch app.desktop screen=x",
    "launch app.desktop action=a screen=1", "launch app.desktop bogus=1", "launch app.desktop noequals",
    "run", 'run ["firefox", "--new-window"]', "run []", 'run [1]', "run {}", "run not json", 'run "x"',
    "dashlaunch", "dashlaunch steam://rungameid/1  extra", "windows", "spotlist", "windows extra",
    "activate", "activate w1", "activate w9", "activate w1 w2", "profile", "profile Work Desk",
    "list", "list extra", "quit", "float", "dock", "float active", "dock active", "float pointer", "dock all",
    "front 3", "front 1", "front x", "front", "tear w1 1300 760", "tear w2 1 1", "tear w9 1 1", "tear w1 x 1",
    "tear w1 1", "dockat 3 1 1000 700 500 20", "dockat 9 1 0 0 0 0", "dockat 3 1 a b 0 0", "dockat 3 -1 -5 7 0 0",
    "dockat 3 1 1 1 1", "dock 3", "dock 9", "close 3", "close 4", "scale 3 2", "scale 3", "scale 3 x",
    "density 3 -2", "density 3 +1", "density 3 x", "density 3", "resize 3 1000 600", "resize 3 800 500",
    "resize 3 100 100", "resize 4 1000 600", "resize 3 1000", "resize 3 a b", "dock w1", "dock w2", "dock w9",
    "float w1", "float w9", "close w1", "close w9", "float w1 extra",
    "float pointer x", "dock all x", "close", "resize w1", "resize w1 1000 600", "scale", "density", "resize",
    "float active x", "dock active x", "tear", "dockat", "front 3 4", "profile  ", "dashlaunch  ",
]


def results():
    return {text: run(text) for text in CASES}


got = json.loads(json.dumps(results()))
if record:
    with open(GOLDEN, "w") as f:
        json.dump(got, f, indent=1, sort_keys=True)
        f.write("\n")
    print(f"recorded {len(got)} cases in {GOLDEN}")
    sys.exit(0)
with open(GOLDEN) as f:
    want = json.load(f)
failed = 0
for text in sorted(set(want) | set(got)):
    ok = want.get(text) == got.get(text)
    failed += not ok
    print(f"{'ok  ' if ok else 'FAIL'}  {text!r}" + ("" if ok else f"\n  got  {got.get(text)}\n  want {want.get(text)}"))
print(f"{len(got) - failed} of {len(set(want) | set(got))} requests as recorded")
sys.exit(1 if failed else 0)
