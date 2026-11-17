#!/usr/bin/env python3
"""Offline test of gaze/eye-rate.py (SteamVR's eye cameras at 90 fps): on, off, and the camera
rate, with SteamVR off (its settings file) and on (vrcmd), and the setting's name from the
driver (SteamOS 0.5's gazeDepEyeToHeadEnable2, 0.4's gazeDepEyeToHeadEnable). HOME is a temporary folder and
vrcmd and pgrep are stand-ins, so the live SteamVR settings are never read or changed.

  gaze/test/eye-rate-test.py
"""
import contextlib
import importlib.machinery
import importlib.util
import io
import json
import os
import tempfile
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
tmp = Path(tempfile.mkdtemp(prefix="ft-eye-rate-test-"))
os.environ["HOME"] = str(tmp)  # eye-rate.py's paths come from it

loader = importlib.machinery.SourceFileLoader("eyerate", os.path.join(HERE, "..", "eye-rate.py"))
er = importlib.util.module_from_spec(importlib.util.spec_from_loader("eyerate", loader))
loader.exec_module(er)
assert str(er.HOME) == str(tmp), "eye-rate.py must take HOME from the environment"
NEW, OLD = er.NEW_KEY, er.OLD_KEY
driver = tmp / "driver_cv.so"
er.DRIVER = driver

failures = []


def check(what, got, want):
    if got != want:
        failures.append(what)
        print(f"FAIL {what}: got {got!r}, want {want!r}", flush=True)


def main_with(*args):
    """eye-rate.py's main() with these arguments: (exit code, what it printed)."""
    er.sys.argv = ["eye-rate.py", *args]
    out = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
        code = er.main()
    return code, out.getvalue()


settings = er.STEAMVR_SETTINGS[0]


def file_value(name=NEW):
    return json.loads(settings.read_text()).get("steamvr", {}).get(name)


# --- Which name the driver reads ---
check("no driver: 0.5's name", er.key(), NEW)
driver.write_bytes(b"\x7fELF\0eyeTrackingRateMin\0gazeDepEyeToHeadEnable\0gazeDepEyeToHeadDist\0")
check("0.4's driver: the old name", er.key(), OLD)
driver.write_bytes(b"\x7fELF\0eyeTrackingRateMin\0gazeDepEyeToHeadEnable2\0gazeDepEyeToHeadDist\0")
check("0.5's driver: the new name", er.key(), NEW)

# --- SteamVR off: the settings file ---
er.steamvr_up = lambda: False
settings.parent.mkdir(parents=True)
settings.write_text(json.dumps({"steamvr": {"analogGain": 0.25}, "dashboard": {"x": 1}}, indent=3))
check("off by default (not in the file)", er.get(), None)
code, _ = main_with("on")
check("on: exit 0", code, 0)
check("on: written to the file", file_value(), True)
check("on: SteamVR's other settings kept", json.loads(settings.read_text())["steamvr"]["analogGain"], 0.25)
check("on: other sections kept", json.loads(settings.read_text())["dashboard"], {"x": 1})
check("on: marked as Frametop's", er.MARK.exists(), True)
code, out = main_with("on")
check("on again: says it's on already", "already" in out, True)
code, _ = main_with("off")
check("off: written to the file", file_value(), False)
check("off: mark gone", er.MARK.exists(), False)
check("on/off: 0.4's name untouched", file_value(OLD), None)

# Turned on for SteamOS 0.4 (an older mark), then SteamOS 0.5: on sets the new name too, and
# off turns both off.
settings.write_text(json.dumps({"steamvr": {OLD: True}}))
er.MARK.parent.mkdir(parents=True, exist_ok=True)
er.MARK.write_text(f"steamvr.{OLD} was off; Frametop turned it on\n")
code, out = main_with("on")
check("after 0.4: on sets 0.5's name", file_value(), True)
check("after 0.4: both marked", er.marked(), [OLD, NEW])
code, out = main_with("off")
check("after 0.4: off turns 0.5's off", file_value(), False)
check("after 0.4: off turns 0.4's off", file_value(OLD), False)
check("after 0.4: mark gone", er.MARK.exists(), False)

# Someone turned it on themselves: off leaves it.
settings.write_text(json.dumps({"steamvr": {NEW: True}}))
code, out = main_with("on")
check("on, set by the user: no mark", er.MARK.exists(), False)
code, out = main_with("off")
check("off, set by the user: left on", file_value(), True)
check("off, set by the user: says so", "left as they are" in out, True)

# No settings file yet (SteamVR never ran): on makes one.
settings.unlink()
code, _ = main_with("on")
check("no file: on makes it", file_value(), True)
main_with("off")

# A broken file isn't overwritten.
settings.write_text("{not json")
code, _ = main_with("on")
check("broken file: on fails", code, 1)
check("broken file: left alone", settings.read_text(), "{not json")
check("broken file: no mark", er.MARK.exists(), False)
settings.unlink()

# --- SteamVR on: vrcmd ---
live = {}
calls = []


def fake_vrcmd(*args):
    calls.append(args)
    if args[0] == "--settings-bool":
        return f"{args[1]}={'true' if live.get(args[1]) else 'false'}\n"
    if args[0] == "--set-settings-bool":
        live[args[1]] = args[2] == "1"  # as vrcmd does: "true" would read as 0
        return f"{args[1]}={args[2]}\n"
    return ""


er.steamvr_up = lambda: True
er.vrcmd = fake_vrcmd
code, _ = main_with("on")
check("SteamVR on: set through vrcmd", live.get(f"steamvr.{NEW}"), True)
check("SteamVR on: 0.4's name untouched", f"steamvr.{OLD}" in live, False)
check("SteamVR on: sets 1, never 'true'", [c for c in calls if c[0] == "--set-settings-bool"][-1][2], "1")
check("SteamVR on: the file isn't touched", settings.exists(), False)
check("SteamVR on: marked", er.MARK.exists(), True)
code, _ = main_with("off")
check("SteamVR on: off through vrcmd", live.get(f"steamvr.{NEW}"), False)
check("SteamVR on: mark gone", er.MARK.exists(), False)

# vrcmd that doesn't take it: on fails, no mark.
er.vrcmd = lambda *args: "" if args[0] == "--set-settings-bool" else f"steamvr.{NEW}=false\n"
code, _ = main_with("on")
check("vrcmd didn't take it: on fails", code, 1)
check("vrcmd didn't take it: no mark", er.MARK.exists(), False)

# --- The camera rate, from eyetracking.txt ---
check("no eyetracking.txt: unknown", er.camera_rate(), None)
check("no eyetracking.txt: no start", er.last_start(), None)
er.EYE_LOG.parent.mkdir(parents=True)
head = "Sat Oct 10 2026 22:35:43.000000 [Info] - "
er.EYE_LOG.write_text("\n".join([
    head + "eyetracking 2.18.2 startup with PID=1, config=x",
    head + "CStereoAdspCams: Set framerate 15.000000",
    head + "Eye tracking shutting down",
    head + "eyetracking 2.18.2 startup with PID=2, config=x",
    head + "Eye tracking started"]) + "\n")
check("no rate set since the last start: 90", er.camera_rate(), 90.0)
check("the last start's time", er.last_start(), er.time.mktime((2026, 10, 10, 22, 35, 43, 0, 0, -1)))
with er.EYE_LOG.open("a") as f:
    f.write(head + "CStereoAdspCams: Set framerate 15.000000\n")
check("15 set since the last start", er.camera_rate(), 15.0)

print("eye-rate-test:", "FAILED " + ", ".join(failures) if failures else "all passed")
raise SystemExit(1 if failures else 0)
