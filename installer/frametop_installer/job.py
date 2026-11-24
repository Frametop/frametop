"""The install itself: get.sh --release, run on the host in a transient user service, and what
the window reads back from it.

The service (UNIT) is what makes the install outlive the window: closing the window, or the
window dying with a SteamVR restart, leaves it running, and opening the installer again finds it.
A reboot or a crash ends it; state.json still says what was picked, and with no result next to
it, the window offers to run the same install again. get.sh resumes a download cut short, and
install.sh finishes an install cut short, so running it again picks up where it stopped.

  ~/.cache/frametop-installer/state.json   what was picked, written when an install starts
  ~/.cache/frametop-installer/install.log  the install's output (the service's stdout)
  ~/.cache/frametop-installer/result       its exit status, written by run-install.sh at the end
"""
import json
import re
import time
from dataclasses import dataclass, field

from . import host

UNIT = "frametop-installer"
FRAMEDROP_UNIT = "frametop-framedrop-install"   # FrameDrop's window's install (framedrop/installer)
STATE = host.CACHE / "state.json"
LOG = host.CACHE / "install.log"
RESULT = host.CACHE / "result"
SOCKET = f"/run/user/{host.UID}/frametop-installer/askpass"


def unit_state(unit=UNIT):
    """The service's ActiveState, or None when systemctl couldn't say (a timeout, a failed
    flatpak-spawn). A unit that doesn't exist reads "inactive", so None never means it's gone."""
    code, out, _ = host.run(["systemctl", "--user", "show", "-P", "ActiveState", unit], timeout=15)
    state = out.decode("utf-8", "replace").strip()
    return state if code == 0 and state else None


def install_args(version, flags, dry_run=False):
    if dry_run:
        return ["/usr/bin/bash", str(host.BIN / "fake-install.sh")]
    return ["/usr/bin/bash", str(host.BIN / "get.sh"), "--release", "--version", version, *flags]


def start(version, channel, flags, restart, askpass, dry_run=False):
    """Start the install's service. Returns "" or why it didn't start. ("" too when systemd-run
    took too long to say: the install may be running, and the progress page finds out.)"""
    if unit_state(FRAMEDROP_UNIT) == "activating":
        return ("FrameDrop is installing Frametop. Let it finish, then open Frametop Installer "
                "again.")
    host.stage()
    was = [(f, f.read_bytes() if f.is_file() else None) for f in (STATE, RESULT, LOG)]
    host.run(["systemctl", "--user", "stop", f"{UNIT}.service"], timeout=30)
    host.run(["systemctl", "--user", "reset-failed", f"{UNIT}.service"], timeout=10)
    RESULT.unlink(missing_ok=True)
    LOG.write_bytes(b"")
    save_state({"version": version, "channel": channel, "flags": list(flags), "restart": bool(restart),
                "sudo": bool(askpass), "dry_run": bool(dry_run), "started": int(time.time()), "seen": False})
    cmd = ["systemd-run", "--user", f"--unit={UNIT}", "--description=Frametop install (Frametop Installer)",
           "--property=Type=oneshot", "--property=RemainAfterExit=yes",
           f"--property=StandardOutput=truncate:{LOG}", "--property=StandardError=inherit",
           f"--setenv=HOME={host.HOME}", "--setenv=PATH=/usr/local/bin:/usr/bin:/bin", "--setenv=TERM=dumb",
           f"--working-directory={host.HOME}", "--quiet", "--no-block"]
    if askpass:
        cmd += [f"--setenv=SUDO_ASKPASS={host.BIN / 'askpass'}", f"--setenv=FRAMETOP_ASKPASS_SOCKET={SOCKET}"]
    cmd += ["/usr/bin/bash", str(host.BIN / "run-install.sh"), str(host.CACHE),
            "1" if restart and not dry_run else "0", "--", *install_args(version, flags, dry_run)]
    code, _, err = host.run(cmd, timeout=30)
    if code in (0, 124):   # (124, host.run's timeout: it may have started, so its state stays)
        return ""
    for f, old in was:   # nothing started: the next open finds what the last install left (its
        f.unlink(missing_ok=True) if old is None else f.write_bytes(old)   # log too), not a new "unfinished" one
    return err.decode("utf-8", "replace").strip() or f"systemd-run failed ({code})"


def save_state(state):
    tmp = STATE.with_suffix(".tmp")
    tmp.write_text(json.dumps(state))
    tmp.replace(STATE)


def load_state():
    try:
        s = json.loads(STATE.read_text())
        return s if isinstance(s, dict) and isinstance(s.get("version"), str) else None
    except (OSError, ValueError):
        return None


def result():
    """The install's exit status, or None while it hasn't finished (or never will: a reboot)."""
    try:
        return int(RESULT.read_text().strip())
    except (OSError, ValueError):
        return None


def where_things_are(state, unit, rc, now=None):
    """What the window opens on: "running", "done", "failed", "unfinished" (stopped with no result:
    a restart or crash), or "new". unit None (systemctl couldn't say) counts as running until a
    result says otherwise, so one failed check doesn't end a live install."""
    if unit == "activating" or (unit is None and state and rc is None):
        return "running"
    if not state:
        return "new"
    if rc is None:
        # systemd-run --no-block: the service can show inactive for a moment after it starts
        if unit in ("inactive", "") and (now or time.time()) - state.get("started", 0) < 5:
            return "running"
        return "unfinished"
    if rc == 0:
        return "new" if state.get("seen") else "done"
    return "new" if state.get("seen") else "failed"


# --- reading the log

STEP = re.compile(r"^== (\d+)/(\d+) (.*)$")
# curl's progress meter: % Total % Received % Xferd Dload Upload Total Spent Left Speed
METER = re.compile(r"^\s*(\d{1,3})\s+([\d.]+[kMGT]?)\s+\d{1,3}\s+([\d.]+[kMGT]?)\s+\d")
METER_HEAD = re.compile(r"^\s*(% Total|Dload\s+Upload)")
ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
DOWNLOAD_SHARE = 0.35    # of the bar: the 1.2 GB download
LOAD_AT = 0.40           # the image loading into podman (install-release.sh's step 0)


def _text(line):
    return ANSI.sub("", line.decode("utf-8", "replace"))


@dataclass
class Progress:
    fraction: float = 0.0
    step: str = "Starting"
    download: str = ""
    lines: list = field(default_factory=list)   # log lines to show, without curl's meter
    _partial: bytes = b""   # the line a read stopped in: decoded whole, once the rest of it comes

    def feed(self, data):
        """Take more of the log (bytes). Returns the new lines to show."""
        *chunks, self._partial = re.split(rb"[\r\n]", self._partial + data)
        return self._take([_text(c) for c in chunks])

    def flush(self):
        rest, self._partial = self._partial, b""
        return self._take([_text(rest)]) if rest else []

    def _take(self, chunks):
        new = []
        for line in chunks:
            if not line.strip():
                continue
            m = METER.match(line)
            if m:
                pct = int(m[1])
                cols = line.split()
                left = cols[10] if len(cols) >= 12 and ":" in cols[10] else ""
                self.download = f"Downloaded {m[3]}B of {m[2]}B ({pct}%)" + (f", {left} left" if left else "")
                self.fraction = max(self.fraction, DOWNLOAD_SHARE * pct / 100)
                continue
            if METER_HEAD.match(line):
                continue
            new.append(line)
            s = STEP.match(line)
            if s:
                n, total, what = int(s[1]), max(int(s[2]), 1), s[3]
                if n == 0:
                    self.fraction = max(self.fraction, LOAD_AT)
                    self.step = what[:1].upper() + what[1:]
                else:
                    self.fraction = max(self.fraction, LOAD_AT + (1 - LOAD_AT - 0.04) * (n - 1) / total)
                    self.step = f"Step {n} of {total}: {what}"
                self.download = ""
            elif line.startswith("Downloading "):
                self.step = "Downloading Frametop (about 1.2 GB)"
            elif line.startswith("Checked Frametop.zip"):
                self.fraction = max(self.fraction, DOWNLOAD_SHARE + 0.01)
                self.step, self.download = "Checked the download", ""
            elif line.startswith("Unpacking "):
                self.fraction = max(self.fraction, DOWNLOAD_SHARE + 0.03)
                self.step = "Unpacking the download"
            elif line.startswith("== Done"):
                self.fraction = max(self.fraction, 0.98)
                self.step = "Finishing"
            elif line.startswith("== Restarting SteamVR"):
                self.fraction, self.step = 1.0, "Restarting SteamVR"
        self.lines += new
        return new
