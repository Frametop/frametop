#!/usr/bin/env python3
"""handsmode: when hand tracking runs, by HANDS_MODE in ~/.config/frametop.conf.

  frametop  ft-camd, ft-hands and the hand view run while the Frametop desktop does: the session
            (session/frametop-session.sh) says "desktop up" once ft-screens is up and "desktop
            down" as it ends.
  always    they're wanted by steamvr.service (systemctl add-wants), so they run whenever SteamVR
            does, in games and the SteamVR dashboard too, with or without the desktop. The input
            relay then leaves them running and unparked when Frametop pauses for a game.
  off       (the default, and anything else) nothing starts them. ft-handsctl on still starts hand
            tracking by hand, until SteamVR stops; choosing off stops it, and ft-cutouts' units too.

The units are the ones hands/run.sh install puts in ~/.config/systemd/user, and the hand view's
frametop-handview.service when it's installed; a unit that isn't installed is left out.
ft-camd needs its file capabilities (hands/run.sh caps, with sudo): without them this starts
nothing and says so ("problem"), and never asks for a password.

  python3 hands/handsmode.py status [--json]   # the mode, what runs, what's wrong
  python3 hands/handsmode.py apply             # start what HANDS_MODE wants now, fix the SteamVR wants
  python3 hands/handsmode.py set MODE          # write HANDS_MODE, then apply it, stopping what it
                                               # doesn't want (Frametop Input Settings, ft-handsctl mode)
  python3 hands/handsmode.py desktop up|down   # the session's hooks; only HANDS_MODE=frametop acts

On the host, system Python, standard library only (the settings app runs it through
distrobox-host-exec, and imports it for the settings' defaults).
"""
import json
import os
import re
import shutil
import subprocess
import sys

CONF_PATH = os.path.expanduser("~/.config/frametop.conf")
MODES = ("frametop", "always", "off")
DEFAULT_MODE = "off"
# The cutouts' margin (HANDS_MATTE_MARGIN, degrees seen from the eyes; ft-screens' default) and the
# range Frametop Input Settings offers. ft-screens takes 0-10.
MARGIN_DEFAULT = 1.5
MARGIN_MIN, MARGIN_MAX, MARGIN_STEP = 0.0, 3.0, 0.25
STEAMVR = "steamvr.service"
TRACKING = ("frametop-camd.service", "frametop-hands.service")
VIEW = "frametop-handview.service"  # draws the hands where Room View is off (hands-view)
CUTOUTS = ("frametop-cutouts-camd.service", "frametop-cutouts-hands.service")  # ft-cutouts' transient units
UNITS = TRACKING + (VIEW,)
CAMD = "frametop-camd.service"
WANTS_DIR = os.path.expanduser("~/.config/systemd/user/" + STEAMVR + ".wants")


def read_conf(path=CONF_PATH):
    """KEY=value lines, as the shell that sources the file would mostly see them."""
    conf = {}
    try:
        with open(path) as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                if "=" in line:
                    k, v = line.split("=", 1)
                    v = v.strip()
                    if len(v) >= 2 and v[0] == v[-1] and v[0] in "'\"":
                        v = v[1:-1]
                    conf[k.strip()] = v
    except OSError:
        pass
    return conf


def write_value(key, value, path=CONF_PATH):
    """Set KEY=value, keeping comments and the rest of the file (as Frametop Input Settings does)."""
    try:
        with open(path) as f:
            lines = f.read().splitlines()
    except OSError:
        lines = []
    for i, line in enumerate(lines):
        if line.split("#", 1)[0].strip().startswith(f"{key}="):
            comment = line[line.index("#"):] if "#" in line else ""
            lines[i] = f"{key}={value}" + (f"   {comment}" if comment else "")
            break
    else:
        lines.append(f"{key}={value}")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def mode_of(conf):
    v = conf.get("HANDS_MODE", "")
    return v if v in MODES else DEFAULT_MODE


def margin_of(conf):
    """HANDS_MATTE_MARGIN as ft-screens reads it: a number 0-10, else its default."""
    try:
        v = float(conf.get("HANDS_MATTE_MARGIN", ""))
    except ValueError:
        return MARGIN_DEFAULT
    return v if 0 <= v <= 10 else MARGIN_DEFAULT


class Host:
    """systemctl --user, pgrep and getcap. Tests replace it."""

    def __init__(self):
        uid = os.getuid()
        # Inside the Frametop session XDG_RUNTIME_DIR is the session's own folder, and the bus is
        # its private one: systemctl needs the user manager's.
        self.env = dict(os.environ, XDG_RUNTIME_DIR=f"/run/user/{uid}",
                        DBUS_SESSION_BUS_ADDRESS=f"unix:path=/run/user/{uid}/bus")

    def run(self, cmd, timeout=20):
        try:
            return subprocess.run(cmd, env=self.env, stdin=subprocess.DEVNULL, capture_output=True,
                                  text=True, timeout=timeout)
        except (OSError, subprocess.TimeoutExpired) as e:
            return subprocess.CompletedProcess(cmd, 1, "", str(e))

    def systemctl(self, *args):
        return self.run(["systemctl", "--user", *args])

    def units(self, names):
        """{unit: {"load": LoadState, "active": ActiveState}} for each."""
        out = self.systemctl("show", "-p", "Id,LoadState,ActiveState", *names).stdout
        found = {}
        for block in out.strip().split("\n\n"):
            props = dict(line.split("=", 1) for line in block.splitlines() if "=" in line)
            if props.get("Id"):
                found[props["Id"]] = {"load": props.get("LoadState", ""), "active": props.get("ActiveState", "")}
        return {u: found.get(u, {"load": "not-found", "active": "inactive"}) for u in names}

    def exec_path(self, unit):
        m = re.search(r"path=(\S+)", self.systemctl("show", "-p", "ExecStart", "--value", unit).stdout)
        return m.group(1) if m else ""

    def wanted(self, unit):
        return os.path.lexists(os.path.join(WANTS_DIR, unit))

    def exists(self, path):
        return os.path.exists(path)

    def caps(self, path):
        """Does ft-camd have its capabilities? None: can't tell (no getcap)."""
        if not shutil.which("getcap"):
            return None
        return "cap_sys_ptrace" in self.run(["getcap", path]).stdout

    def desktop_up(self):
        return self.run(["pgrep", "-x", "ft-screens"]).returncode == 0

    def cameras(self):
        """camcheck's summary ("ok", "degraded: ...", "unknown: ...")."""
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        try:
            import camcheck
            return camcheck.check()["summary"]
        except Exception as e:  # a status line, not a crash
            return f"unknown: camcheck failed: {e}"


def status(host, conf=None, cameras=True):
    """What the settings app shows: the mode, what's installed and running, and "problem", a
    sentence saying why hand tracking can't run (empty when nothing's wrong)."""
    conf = read_conf() if conf is None else conf
    states = host.units((STEAMVR,) + UNITS + CUTOUTS)

    def loaded(u):
        return states[u]["load"] == "loaded"

    def running(u):
        return states[u]["active"] in ("active", "activating", "reloading")

    st = {"mode": mode_of(conf), "steamvr": running(STEAMVR), "desktop": host.desktop_up(),
          "installed": all(loaded(u) for u in TRACKING), "view": loaded(VIEW),
          "tracking": all(running(u) for u in TRACKING), "view_running": running(VIEW),
          "cutouts": any(running(u) for u in CUTOUTS), "wanted": [u for u in UNITS if host.wanted(u)],
          "active": [u for u in UNITS + CUTOUTS if running(u)],
          "camd": "", "built": False, "caps": None, "cameras": "", "problem": ""}
    if st["installed"]:
        st["camd"] = host.exec_path(CAMD)
        st["built"] = bool(st["camd"]) and host.exists(st["camd"])
        if st["built"]:
            st["caps"] = host.caps(st["camd"])
    repo = os.path.dirname(os.path.dirname(os.path.dirname(st["camd"]))) if st["camd"] else "the Frametop folder"
    if not st["installed"]:
        st["problem"] = ("Hand tracking isn't installed. To install it, run hands/run.sh install in the "
                         "Frametop folder, in a terminal (it asks for your password once).")
    elif not st["built"]:
        st["problem"] = f"ft-camd isn't built ({st['camd'] or 'no path'}). Run hands/run.sh install in {repo}."
    elif st["caps"] is False:
        st["problem"] = (f"ft-camd is missing the permissions it needs to read the cameras (a rebuild clears "
                         f"them). In a terminal, run {repo}/hands/run.sh caps (it asks for your password).")
    elif cameras and st["steamvr"]:
        st["cameras"] = host.cameras()
        if st["cameras"].startswith("degraded"):
            st["problem"] = "The headset's cameras aren't all running: " + st["cameras"].split(": ", 1)[-1]
    st["summary"] = summary(st)
    return st


def summary(st):
    """One plain line: what runs, or when it will."""
    if st["tracking"]:
        how = {"off": " (started by hand: ft-handsctl)"}.get(st["mode"], "")
        return "Hand tracking is running" + how
    if st["cutouts"]:
        return "The hand cutouts are running (started by hand: ft-cutouts)"
    if st["mode"] == "off":
        return "Hand tracking is off"
    when = "with the Frametop desktop" if st["mode"] == "frametop" else "with SteamVR"
    if st["problem"]:
        return "Hand tracking can't start " + when
    if not st["steamvr"]:
        return "Hand tracking starts " + when
    return "Hand tracking is starting" if st["mode"] == "always" or st["desktop"] else "Hand tracking starts " + when


def plan(mode, st, desktop=None, strict=False):
    """The systemctl calls that bring the units to MODE: a list of argument lists. Starting and the
    SteamVR wants always follow the mode; stopping only when the desktop just ended (desktop=False,
    the session's hook) or with strict (the mode was just chosen), so an install or a caps run
    doesn't stop what ft-handsctl or ft-cutouts started by hand.
    desktop: True/False from the session's hooks, None to look (st["desktop"])."""
    units = [u for u in TRACKING if st["installed"]] + ([VIEW] if st["view"] else [])
    up = st["desktop"] if desktop is None else desktop
    can_run = st["installed"] and st["built"] and st["caps"] is not False
    calls = []
    if mode == "always":
        missing = [u for u in units if u not in st["wanted"]]
        if missing:
            calls.append(["add-wants", STEAMVR, *missing])
    else:
        unwanted = [u for u in units if u in st["wanted"]]
        if unwanted:
            calls.append(["disable", *unwanted])
    run = st["steamvr"] and can_run and (mode == "always" or (mode == "frametop" and up))
    if run and units:
        calls.append(["start", "--no-block", *units])
    elif not (strict or desktop is False):
        pass
    elif mode == "off" and st["active"]:
        calls.append(["stop", "--no-block", *st["active"]])  # ft-handsctl's or ft-cutouts' too
    elif mode == "frametop" and not up:
        active = [u for u in units if u in st["active"]]
        if active:
            calls.append(["stop", "--no-block", *active])
    return calls


def apply(host, conf=None, desktop=None, strict=False):
    """Bring the units to HANDS_MODE (plan); returns the systemctl calls made, and the status they
    started from."""
    conf = read_conf() if conf is None else conf
    st = status(host, conf, cameras=False)
    calls = plan(st["mode"], st, desktop, strict)
    for args in calls:
        host.systemctl(*args)
    return calls, st


def main(argv):
    host = Host()
    cmd = argv[0] if argv else "status"
    if cmd == "status":
        st = status(host)
        if "--json" in argv:
            print(json.dumps(st))
        else:
            print(f"HANDS_MODE={st['mode']}: {st['summary']}")
            if st["problem"]:
                print(st["problem"])
        return 0
    if (cmd == "set" and len(argv) == 2 and argv[1] in MODES) or argv == ["apply"]:
        if cmd == "set":
            write_value("HANDS_MODE", argv[1])
        calls, st = apply(host, strict=cmd == "set")
        for args in calls:
            print("systemctl --user " + " ".join(args))
        if st["problem"]:
            print(st["problem"], file=sys.stderr)
        return 0
    if cmd == "desktop" and len(argv) == 2 and argv[1] in ("up", "down"):
        conf = read_conf()
        if mode_of(conf) == "frametop":
            calls, st = apply(host, conf, desktop=argv[1] == "up")
            for args in calls:  # into the session's log
                print("frametop: hand tracking (HANDS_MODE=frametop): systemctl --user " + " ".join(args))
            if st["problem"] and argv[1] == "up":
                print("frametop: hand tracking: " + st["problem"])
        return 0
    print("usage: handsmode.py status [--json] | apply | set frametop|always|off | desktop up|down", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
