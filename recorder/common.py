"""What the recorders share that isn't about any one recorder: clocks, the dev container and the
host, small file and text helpers, and the versions a session records.

Standard library only: the recorders run it in the dev container and, standalone, on the host."""
import glob
import json
import os
import shutil
import time


def mono_ns():
    return time.clock_gettime_ns(time.CLOCK_MONOTONIC)


def clock_sample():
    """[CLOCK_MONOTONIC ns, CLOCK_MONOTONIC_RAW - CLOCK_MONOTONIC ns], as ft-hands' raw_minus_mono_ns().
    sets.bin's capture_ns is on the RAW clock and everything else on MONOTONIC; the two drift
    apart with NTP's corrections (0.8 s apart and ~10 ppm on 2026-10-03), so take.json keeps
    samples to turn capture_ns into MONOTONIC: capture_ns - offset, interpolated by time."""
    a = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
    r = time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)
    b = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
    mid = (a + b) // 2
    return [mid, r - mid]


def in_container():
    return os.path.exists("/run/.containerenv") or os.path.exists("/.dockerenv")


def host_command(*cmd):
    """argv to run a command on the SteamOS host from the dev container (as in
    display-settings: distrobox-host-exec needs the user's real session bus). It runs from the
    home folder: host-spawn starts the command in the caller's folder, and a container-only
    one such as /run/host/tmp doesn't exist on the host (every command then exits 127)."""
    exe = shutil.which("distrobox-host-exec")
    if not in_container() or not exe:
        return list(cmd)
    return ["env", "-C", os.path.expanduser("~"), "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/%d/bus" % os.getuid(),
            exe] + list(cmd)


def host_path(path):
    """A host file, from the container (/run/host) or the host itself."""
    for p in ("/run/host" + path, path):
        if os.path.exists(p):
            return p
    return None


def write_json(path, data):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(data, f, indent=1)
        f.write("\n")
    os.replace(tmp, path)


def clean_text(s):
    """Text for a panel command: one line; "|" is the panel's line break, so callers use it."""
    return " ".join(str(s).replace("\r", " ").replace("\n", " ").split())


def os_version():
    path = "/run/host/etc/os-release" if in_container() else "/etc/os-release"
    try:
        with open(path) as f:
            for line in f:
                if line.startswith("VERSION_ID="):
                    return line.split("=", 1)[1].strip().strip('"')
    except OSError:
        pass
    return ""


def steamvr_version():
    for db in ("/usr/lib/holo/pacmandb", "/var/lib/pacman"):   # SteamOS keeps it in the image
        found = sorted(glob.glob(("/run/host" if in_container() else "") + db + "/local/deckard-steamvr-rel-*"))
        if found:
            return os.path.basename(found[-1])[len("deckard-steamvr-rel-"):]
    p = host_path("/opt/steamvr/bin/version.txt")
    if p:
        try:
            with open(p) as f:
                return "build " + f.read().strip()
        except OSError:
            pass
    return ""
