#!/usr/bin/env python3
"""Offline test of how Frametop's tools read and set SteamVR's settings: through vrcmd while
SteamVR runs, in its steamvr.vrsettings file while it's down. It runs ft-dashpatch's get and put
(the dashboard debugger's two settings) both ways; gaze/test/eye-rate-test.py does the same for
eye-rate.py. HOME is a temporary folder and vrcmd and pgrep are stand-ins, so the live SteamVR
settings are never read or changed.

  conf/test/vrsettings-test.py
"""
import contextlib
import importlib.util
import io
import json
import os
import sys
import tempfile
import types
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
tmp = Path(tempfile.mkdtemp(prefix="ft-vrsettings-test-"))
os.environ["HOME"] = str(tmp)  # the settings paths come from it
sys.modules.setdefault("steamui", types.SimpleNamespace(_WebSocket=object))  # (ft-dashpatch's CDP client)

spec = importlib.util.spec_from_file_location("dashpatch", REPO / "pointer" / "dashpatch" / "ft-dashpatch.py")
dp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dp)

failures = []


def check(what, got, want):
    ok = got == want
    print(("ok    " if ok else "FAIL  ") + what + ("" if ok else f": got {got!r}, want {want!r}"), flush=True)
    if not ok:
        failures.append(what)


def quiet(fn, *args):
    err = io.StringIO()
    with contextlib.redirect_stderr(err):
        return fn(*args), err.getvalue()


settings = tmp / ".config/openvr/config/steamvr.vrsettings"
other = tmp / ".steam/steam/config/steamvr.vrsettings"

# ---- SteamVR down: the file
dp.steamvr_up = lambda: False
check("which file: the first when neither exists", dp.settings_file(), settings)
check("no file: nothing known", dp.get(), (None, None))
check("on writes the file", dp.put(True), True)
check("...both settings, three-space JSON", json.loads(settings.read_text()),
      {"VRWebHelper": {"DebuggerEnabled": True, "DebuggerPort": 8089}})
check("...indented by 3, ending in a newline", settings.read_text().splitlines()[1], '   "VRWebHelper": {')
check("get reads them", dp.get(), (True, 8089))
settings.write_text(json.dumps({"steamvr": {"x": 1}, "VRWebHelper": {"DebuggerEnabled": True, "DebuggerPort": 8089,
                                                                     "Other": "kept"}}))
check("off keeps the port and the rest", (dp.put(False), json.loads(settings.read_text())),
      (True, {"steamvr": {"x": 1}, "VRWebHelper": {"DebuggerEnabled": False, "DebuggerPort": 8089, "Other": "kept"}}))
check("no temporary file left", sorted(p.name for p in settings.parent.iterdir()), ["steamvr.vrsettings"])
settings.write_text(json.dumps({"VRWebHelper": {"DebuggerEnabled": "yes", "DebuggerPort": "8089"}}))
check("values of the wrong type: unknown", dp.get(), (None, None))
settings.write_text("{not json")
check("a file that doesn't read: unknown", dp.get(), (None, None))
ok, err = quiet(dp.put, True)
check("...and put leaves it alone", (ok, settings.read_text(), "can't read" in err), (False, "{not json", True))
settings.write_text("[1, 2]")
check("a file that isn't an object: unknown", dp.get(), (None, None))
settings.write_text(json.dumps({"steamvr": {}}))
check("no section: unknown", dp.get(), (None, None))
other.parent.mkdir(parents=True)
other.write_text("{}")
check("which file: the first that exists", dp.settings_file(), settings)
os.replace(settings, tmp / "moved.vrsettings")
check("...the second when only it exists", dp.settings_file(), other)
os.replace(tmp / "moved.vrsettings", settings)

# ---- SteamVR up: vrcmd
calls = []
live = {"bool": "true", "int": "8089"}


def fake_vrcmd(*args):
    calls.append(args)
    if args[0] == "--settings-bool":
        return f"{args[1]}={live['bool']}\n" if live["bool"] is not None else "no such setting\n"
    if args[0] == "--settings-int":
        return f"{args[1]}={live['int']}\n" if live["int"] is not None else ""
    return ""


dp.steamvr_up = lambda: True
dp.vrcmd = fake_vrcmd
before = settings.read_text()
check("get asks vrcmd", (dp.get(), calls), ((True, 8089), [("--settings-bool", "VRWebHelper.DebuggerEnabled"),
                                                         ("--settings-int", "VRWebHelper.DebuggerPort")]))
live.update(bool="0", int="-1")
check("0 and a negative port", dp.get(), (False, -1))
live.update(bool=None, int=None)
check("nothing printed: unknown", dp.get(), (None, None))
calls.clear()
live.update(bool="1", int="8089")
check("on sets 1 and the port, and checks", (dp.put(True), calls[:2]),
      (True, [("--set-settings-bool", "VRWebHelper.DebuggerEnabled", "1"),
              ("--set-settings-int", "VRWebHelper.DebuggerPort", "8089")]))
calls.clear()
live.update(bool="false")
check("off sets 0 only", (dp.put(False), calls[0], [c for c in calls if c[0] == "--set-settings-int"]),
      (True, ("--set-settings-bool", "VRWebHelper.DebuggerEnabled", "0"), []))
live.update(bool="true", int="1234")
check("on with the port not taken: False", dp.put(True), False)
check("the file isn't touched while SteamVR runs", settings.read_text(), before)

print("all passed" if not failures else f"{len(failures)} failed")
sys.exit(1 if failures else 0)
