"""systemd user units for the recorders: start a helper as a transient unit that stops with
SteamVR, and pause other units (an overlay, the pointer) for a session and start them again,
even after a crash. Commands go to the host's systemd from the dev container (common.host_command)."""
import json
import os
import subprocess
import time

from recorder.common import host_command


def unit_active(unit):
    return subprocess.run(host_command("systemctl", "--user", "-q", "is-active", unit),
                          capture_output=True, timeout=30).returncode == 0


def start_unit(unit, what, argv, log=lambda line: None, app="Frametop hand recorder"):
    """Start argv as a transient user unit that stops with SteamVR. True if it started it,
    False if it was running already; raises RuntimeError if it couldn't."""
    if unit_active(unit):
        log("%s is running already" % unit)
        return False
    cmd = host_command("systemd-run", "--user", "--quiet", "--collect", "--unit=" + unit,
                       "--description=" + app + ": " + what,
                       "-p", "PartOf=steamvr.service", "-p", "After=steamvr.service",
                       "-p", "Restart=on-failure", "-p", "RestartSec=3", "-p", "TimeoutStopSec=5", *argv)
    for attempt in range(3):   # distrobox-host-exec has failed once, silently, and worked again
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        if r.returncode == 0 or unit_active(unit):
            log("started %s" % unit)
            return True
        log("starting %s failed (exit %d): %s" % (unit, r.returncode, (r.stderr or r.stdout).strip()))
        time.sleep(0.5)
    raise RuntimeError("couldn't start %s (exit %d): %s" % (unit, r.returncode, (r.stderr or r.stdout).strip()))


def stop_unit(unit):
    subprocess.run(host_command("systemctl", "--user", "stop", unit), capture_output=True, timeout=30)


def pause_units(units, path, log=lambda line: None):
    """Stop those of units that are running, listed in path first so resume_units starts them again
    even after a crash. Returns the ones stopped."""
    paused = [u for u in units if unit_active(u)]
    if not paused:
        return []
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(paused, f)
    for unit in paused:
        stop_unit(unit)
        log("paused %s for the session" % unit)
    return paused


def resume_units(path, log=lambda line: None):
    """Start the units pause_units listed in path, and forget them. Returns them."""
    try:
        with open(path) as f:
            units = [u for u in json.load(f) if isinstance(u, str) and u.endswith(".service")]
    except (OSError, ValueError, TypeError):
        return []
    for unit in units:
        r = subprocess.run(host_command("systemctl", "--user", "start", unit), capture_output=True, text=True, timeout=60)
        log("started %s again%s" % (unit, "" if r.returncode == 0 else
                                     " FAILED (exit %d): %s" % (r.returncode, (r.stderr or r.stdout).strip())))
    try:
        os.remove(path)
    except OSError:
        pass
    return units


def restart_units(units):
    """Restart those that are running, in order."""
    for unit in units:
        subprocess.run(host_command("systemctl", "--user", "try-restart", unit), capture_output=True, timeout=60)
