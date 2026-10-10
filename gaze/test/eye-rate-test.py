#!/usr/bin/env python3
"""Offline test of gaze/eye-rate.py (SteamVR's eye cameras at 90 fps): on, off, and the camera
rate, with SteamVR off (its settings file) and on (vrcmd). HOME is a temporary folder and
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


def file_value():
    return json.loads(settings.read_text()).get("steamvr", {}).get("gazeDepEyeToHeadEnable")


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

# Someone turned it on themselves: off leaves it.
settings.write_text(json.dumps({"steamvr": {"gazeDepEyeToHeadEnable": True}}))
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
live = {"value": False}
calls = []


def fake_vrcmd(*args):
    calls.append(args)
    if args[0] == "--settings-bool":
        return f"steamvr.gazeDepEyeToHeadEnable={'true' if live['value'] else 'false'}\n"
    if args[0] == "--set-settings-bool":
        live["value"] = args[2] == "1"  # as vrcmd does: "true" would read as 0
        return f"steamvr.gazeDepEyeToHeadEnable={args[2]}\n"
    return ""


er.steamvr_up = lambda: True
er.vrcmd = fake_vrcmd
code, _ = main_with("on")
check("SteamVR on: set through vrcmd", live["value"], True)
check("SteamVR on: sets 1, never 'true'", [c for c in calls if c[0] == "--set-settings-bool"][-1][2], "1")
check("SteamVR on: the file isn't touched", settings.exists(), False)
check("SteamVR on: marked", er.MARK.exists(), True)
code, _ = main_with("off")
check("SteamVR on: off through vrcmd", live["value"], False)
check("SteamVR on: mark gone", er.MARK.exists(), False)

# vrcmd that doesn't take it: on fails, no mark.
er.vrcmd = lambda *args: "" if args[0] == "--set-settings-bool" else "steamvr.gazeDepEyeToHeadEnable=false\n"
code, _ = main_with("on")
check("vrcmd didn't take it: on fails", code, 1)
check("vrcmd didn't take it: no mark", er.MARK.exists(), False)

# --- The camera rate, from eyetracking.txt ---
check("no eyetracking.txt: unknown", er.camera_rate(), None)
er.EYE_LOG.parent.mkdir(parents=True)
head = "Sat Oct 10 2026 22:35:43.000000 [Info] - "
er.EYE_LOG.write_text("\n".join([
    head + "eyetracking 2.18.2 startup with PID=1, config=x",
    head + "CStereoAdspCams: Set framerate 15.000000",
    head + "Eye tracking shutting down",
    head + "eyetracking 2.18.2 startup with PID=2, config=x",
    head + "Eye tracking started"]) + "\n")
check("no rate set since the last start: 90", er.camera_rate(), 90.0)
with er.EYE_LOG.open("a") as f:
    f.write(head + "CStereoAdspCams: Set framerate 15.000000\n")
check("15 set since the last start", er.camera_rate(), 15.0)

print("eye-rate-test:", "FAILED " + ", ".join(failures) if failures else "all passed")
raise SystemExit(1 if failures else 0)
