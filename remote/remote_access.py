"""Frametop Settings' Remote access page: see and use the Frametop desktop from another computer on
your tailnet, over VNC (session/remote-desktop.sh and session/vnc-bridge.sh).

The page's backend (RemoteAccessPages.qml; settings/ft_settings.py makes it). It turns remote
access on and off (REMOTE in ~/.config/frametop.conf, applied now through session/remote-ctl.sh
on the host when this desktop allows it), shows the address to connect to, and shows, copies, or
replaces the VNC password (~/.config/frametop-remote/vnc-password). Nothing is stored anywhere
else, and no password is in the code: the first start makes a random one.

remote-ctl.sh runs on the host (bash, curl, and python3 for the tailnet name), so each check is
a process of its own, never on the UI's thread: every 5 seconds while the page shows, and after
a change.
"""
import os
import secrets
import string
import sys

from PySide6.QtCore import Property, QObject, QProcess, QTimer, Signal, Slot
from PySide6.QtGui import QGuiApplication

REPO = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
sys.path.insert(0, os.path.join(REPO, "conf"))
import ft_conf  # noqa: E402  (frametop.conf's grammar, docs/config.md)
from fthost import host_command  # noqa: E402  (commands on the SteamOS host)

CTL = os.path.join(REPO, "session", "remote-ctl.sh")
CONF = os.path.expanduser("~/.config/frametop.conf")
CREDS = os.path.expanduser("~/.config/frametop-remote")
VNC_PASSWORD = os.path.join(CREDS, "vnc-password")
HIDDEN = "••••••••"
NOT_CAPABLE = 3  # remote-ctl.sh start: this desktop didn't start with remote access on


def read_password(path=None):
    try:
        with open(path or VNC_PASSWORD) as f:
            return f.read().strip()
    except OSError:
        return ""


def new_password(path=None):
    """8 random letters and digits: VNC's own authentication takes at most 8 characters."""
    path = path or VNC_PASSWORD
    os.makedirs(os.path.dirname(path), mode=0o700, exist_ok=True)
    pw = "".join(secrets.choice(string.ascii_letters + string.digits) for _ in range(8))
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        f.write(pw + "\n")
    return pw


def parse_status(out):
    """remote-ctl.sh status's key=value lines."""
    return dict(line.split("=", 1) for line in (out or "").splitlines() if "=" in line)


def address_text(status):
    port = status.get("port", "5900")
    name, addr = status.get("name"), status.get("address")
    if name and addr:
        return f"{name}:{port}  ({addr})" if port != "5900" else f"{name}  ({addr})"
    return f"{addr}:{port}" if addr and port != "5900" else addr or ""


def state_text(status, wanted):
    if status.get("failed"):
        return "Couldn't check remote access: session/remote-ctl.sh status didn't run on the host"
    if not status.get("address"):
        return "The tailnet (tailscale0) has no address: remote access can't start"
    if status.get("running") == "1":
        return "On: waiting for viewers"
    if wanted and status.get("capable") != "1":
        return "Turns on at the next desktop restart"
    return "Starting…" if wanted else "Off"


class Backend(QObject):
    changed = Signal()
    message = Signal(str, bool)

    def __init__(self, conf=None, password_path=None):
        super().__init__()
        self._conf = conf or CONF
        self._pw_path = password_path or VNC_PASSWORD
        self._status = {}
        self._known = False   # a status has come back
        self._revealed = False
        self._asking = None   # the status check running
        self._doing = None    # the start, stop or restart running
        self.poll = QTimer(interval=5000, timeout=self.refresh)

    # --- state ---
    def _wanted(self):
        return ft_conf.read(self._conf).get("REMOTE", "0") == "1"

    @Property(bool, notify=changed)
    def enabled(self):
        return self._wanted()

    @Property(str, notify=changed)
    def state(self):
        return state_text(self._status, self._wanted()) if self._known else "Checking…"

    @Property(str, notify=changed)
    def address(self):
        if self._status.get("failed"):
            return "unknown"
        return address_text(self._status) or ("no tailnet address" if self._known else "…")

    @Property(bool, notify=changed)
    def hasPassword(self):
        return bool(read_password(self._pw_path))

    @Property(bool, notify=changed)
    def revealed(self):
        return self._revealed

    @Property(str, notify=changed)
    def password(self):
        pw = read_password(self._pw_path)
        if not pw:
            return "made when remote access first starts"
        return pw if self._revealed else HIDDEN

    @Property(bool, notify=changed)
    def busy(self):
        return self._doing is not None

    # --- checks ---
    @Slot(bool)
    def setActive(self, on):
        """Check now and every 5 seconds while the page shows; not at all while it doesn't."""
        if on:
            self.refresh()
            self.poll.start()
        else:
            self.poll.stop()
            if self._revealed:  # shown again only when asked again
                self._revealed = False
                self.changed.emit()

    @Slot()
    def refresh(self):
        if self._asking is not None:
            return
        proc = self._asking = QProcess(self)
        argv = host_command(CTL, "status")
        proc.finished.connect(lambda *_: self._status_done(proc))
        proc.errorOccurred.connect(lambda err: err == QProcess.FailedToStart and self._status_done(proc))
        proc.start(argv[0], argv[1:])

    def _status_done(self, proc):
        if self._asking is not proc:
            return
        self._asking = None
        try:
            failed = proc.error() == QProcess.FailedToStart or proc.exitCode() != 0
            out = bytes(proc.readAllStandardOutput()).decode(errors="replace")
        except RuntimeError:  # the process went with this backend (the app is quitting)
            return
        # A check that couldn't run on the host (distrobox-host-exec failing) isn't a tailnet
        # without an address: say so instead.
        self._status = {"failed": "1"} if failed and not out.strip() else parse_status(out)
        self._known = True
        proc.deleteLater()
        self.changed.emit()

    def _refresh_soon(self):
        """Again in a moment: remote access takes a second or two to start or stop."""
        self.refresh()
        QTimer.singleShot(2000, self, self.refresh)  # not after this backend is gone

    def _ctl(self, verb, done):
        """remote-ctl.sh VERB on the host; done(exit code) when it ends."""
        proc = self._doing = QProcess(self)
        argv = host_command(CTL, verb)

        def finished(code=1, *_):
            if self._doing is not proc:
                return
            self._doing = None
            proc.deleteLater()
            done(code)
            self._refresh_soon()
        proc.finished.connect(finished)
        proc.errorOccurred.connect(lambda err: err == QProcess.FailedToStart and finished(1))
        proc.start(argv[0], argv[1:])
        self.changed.emit()

    # --- actions ---
    @Slot(bool)
    def setEnabled(self, on):
        if self._doing is not None:
            self.message.emit("Still busy: try again in a moment", True)
            self.changed.emit()
            return
        ft_conf.set_value("REMOTE", "1" if on else "0", self._conf)
        if not on:
            self._ctl("stop", lambda code: self.message.emit("Remote access off", False))
        else:
            self._ctl("start", lambda code: self.message.emit(
                "Remote access turns on at the next desktop restart" if code == NOT_CAPABLE
                else "Remote access on" if code == 0 else f"Remote access didn't start (exit code {code})",
                code not in (0, NOT_CAPABLE)))

    @Slot(bool)
    def setRevealed(self, on):
        self._revealed = bool(on)
        self.changed.emit()

    @Slot()
    def copyAddress(self):
        self._copy(address_text(self._status), "Address copied", "No tailnet address to copy")

    @Slot()
    def copyPassword(self):
        self._copy(read_password(self._pw_path), "Password copied", "No password yet: turn remote access on")

    def _copy(self, text, done, none):
        if not text:
            self.message.emit(none, True)
            return
        QGuiApplication.clipboard().setText(text)
        self.message.emit(done, False)

    @Slot()
    def changePassword(self):
        """A new password; viewers connected now are disconnected (remote access restarts)."""
        new_password(self._pw_path)
        self.changed.emit()
        if self._status.get("running") == "1" and self._doing is None:
            self._ctl("restart", lambda code: self.message.emit("New password set: remote access restarted", False))
        else:
            self.message.emit("New password set", False)
