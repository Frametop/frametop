"""SteamVR's travel mode (internal.enableTravelMode): tracking that holds up in a moving car,
train or plane. SteamVR has it but no switch for it, so Input Settings has one (DEE-220).

It goes through SteamVR's vrcmd on the host, and only while SteamVR runs: SteamVR keeps its
settings in memory and writes over its settings file, so the file isn't touched. Off is
SteamVR's own default, and nothing here changes it until the switch is used.

`run` runs a command on the host and returns a CompletedProcess (Input Settings' host());
tests pass their own, with a stub vrcmd."""
import re

VRCMD = "/opt/steamvr/bin/linuxarm64/vrcmd"
KEY = "internal.enableTravelMode"
# Exit codes of SCRIPT's own checks, ones sh, the host runner and vrcmd don't use (127 is any
# missing command or library).
MISSING, NOT_RUNNING = 90, 91
# One host call: is vrcmd there, is SteamVR running, then vrcmd with SteamVR's config folder
# (a terminal in the desktop has the desktop's own) and its libraries, as ft-dashpatch runs it.
SCRIPT = ('v=$1; shift; [ -x "$v" ] || exit 90; pgrep -x vrserver >/dev/null || exit 91; '
          'XDG_CONFIG_HOME="$HOME/.config" LD_LIBRARY_PATH="${v%/*}" exec "$v" "$@"')


def vrcmd(run, *args, timeout=15):
    return run("sh", "-c", SCRIPT, "sh", VRCMD, *args, timeout=timeout)


def why_not(code):
    """What to tell the user when vrcmd didn't run, or "" when it did."""
    if code == MISSING:
        return f"SteamVR's vrcmd isn't at {VRCMD}, so travel mode can't be changed here"
    if code == NOT_RUNNING:
        return "Start SteamVR to change travel mode (it's SteamVR's setting)"
    return ""


def get(run):
    """(on, problem): on is True or False, or None when it can't be read (problem says why)."""
    r = vrcmd(run, "--settings-bool", KEY)
    problem = why_not(r.returncode)
    if problem:
        return None, problem
    m = re.search(rf"^{re.escape(KEY)}=(\w+)", r.stdout or "", re.M)
    if not m:
        # Never set: SteamVR's default, off. vrcmd still answers, without the line.
        return (False, "") if r.returncode == 0 else (None, f"vrcmd couldn't read {KEY}")
    return m.group(1).lower() in ("true", "1"), ""


def put(run, on):
    """Turn it on or off. (ok, text, got): text is the message to show, got what SteamVR has
    now (get's, read back; None if unknown)."""
    # 1 or 0: vrcmd reads "true" as 0.
    r = vrcmd(run, "--set-settings-bool", KEY, "1" if on else "0")
    problem = why_not(r.returncode)
    if problem:
        return False, problem, None
    if r.returncode != 0:
        said = ((r.stderr or "") + (r.stdout or "")).strip().splitlines()
        return False, f"vrcmd couldn't set travel mode: {said[-1] if said else f'exit {r.returncode}'}", None
    got, problem = get(run)
    if got is None:
        return False, problem or "vrcmd ran, but SteamVR's travel mode couldn't be read back", None
    if got is not on:
        return False, f"SteamVR kept travel mode {'on' if got else 'off'}", got
    return True, ("Travel mode on: SteamVR tracks for a moving vehicle" if on
                  else "Travel mode off"), got
