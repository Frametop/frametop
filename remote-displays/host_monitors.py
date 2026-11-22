"""Remote Displays' "Turn off and set its monitors" switch, per Vibepollo host, and the mode
picker of its display panels (docs/remote-displays.md, "A PC's monitors").

With the switch on, the host's monitors follow its display panels (layout/ft_hostmonitors.py,
which ft-layout runs whenever the panels change): while one of them streams, the PC turns off
each monitor that has no connected panel here, and puts the others in the mode picked for
their panel. Here the PC is asked which monitors it has and their modes (for the pickers), the
switch and the picks are saved through ft-layout, and the panels of a controlled host stream at
their monitor's mode, pixel for pixel (ft-layout remote set ... size=).

The backend (ft_remote_displays.py) makes one and shows it to the page as hostMonitors.
Questions to the PC run on a thread; changes go in the backend's queue of ft-layout runs.
"""
import threading

from PySide6.QtCore import Property, QObject, Qt, Signal, Slot

# conf/ and layout/, put on the path by the backend
import ft_hostmonitors
import ft_layout

NO_BUILD = ("Its Vibepollo can't turn off or set its monitors: that needs Frametop's build of "
            "Vibepollo, a newer one than it has.")


def why(name, status, answer):
    """What to tell the user about an answer that isn't the PC's monitors: "" when it is."""
    if status == 200 and isinstance(answer, dict) and answer.get("status") is True:
        return ""
    if status == 0:
        return f"Couldn't reach {name}"
    if status in (401, 403):
        return f"Sign in to {name} again to turn off or set its monitors (its sign-in is from before Frametop could)"
    if status == 404:
        return NO_BUILD
    error = answer.get("error") if isinstance(answer, dict) else None
    return error if isinstance(error, str) and error else f"{name} answered {status}"


def stream_size(monitor, mode):
    """[width, height] a stream of the monitor in `mode` has: a monitor turned a quarter lists
    its modes unrotated (Hyprland, KDE)."""
    size = [int(mode["width"]), int(mode["height"])]
    return size[::-1] if monitor.get("sideways") else size


def mode_arg(mode):
    """ft-layout remote set's mode= value."""
    return "own" if not mode else f"{int(mode['width'])}x{int(mode['height'])}@{int(mode['refresh_mhz'])}"


def scale_arg(scale):
    """ft-layout remote set's scale= value."""
    return "own" if not scale else f"{float(scale):g}"


def size_changes(host, monitors):
    """ft-layout runs that make a controlled host's display panels stream at their monitor's
    mode: the one picked for the panel, else the one the monitor has now. [(label, args)]"""
    if not ft_hostmonitors.controls(host):
        return []
    by_id = {m.get("id"): m for m in monitors if isinstance(m, dict)}
    runs = []
    for d in host.get("displays", []):
        monitor = by_id.get(ft_hostmonitors.monitor_of(d))
        if monitor is None:
            continue
        mode = ft_hostmonitors.mode_of(d) or (monitor.get("mode") if monitor.get("on") else None)
        if not isinstance(mode, dict) or not mode.get("width") or not mode.get("height"):
            continue
        size = list(ft_layout.stream_size(*stream_size(monitor, mode)))  # as ft-layout keeps it
        if list(d.get("size", [])) != size:
            runs.append((f"Streaming {d.get('label') or d['id']} at its monitor's mode",
                         ("remote", "set", d["id"], f"size={size[0]}x{size[1]}")))
    return runs


class HostMonitors(QObject):
    supportedChanged = Signal()
    listedChanged = Signal()
    _answered = Signal(str, int, object)  # name, HTTP status (0: no answer, -1: not asked), answer

    def __init__(self, run, parent=None):
        """run(label, *ft_layout_args, then=None): the backend's queue of ft-layout runs; then()
        once that one has run."""
        super().__init__(parent)
        self._run = run
        self._supported = {}  # host name -> whether its Vibepollo turns off and sets its monitors
        self._listed = {}     # host name -> {"monitors": [...], "note": str}
        self._sizing = set()  # ft-layout size runs queued and not run yet: each answer asks for them
        self._answered.connect(self._done, Qt.QueuedConnection)

    @Property("QVariantMap", notify=supportedChanged)
    def supported(self):
        """Host name -> True once its Vibepollo answered that it turns off and sets its monitors,
        False once it answered that it can't (an older or upstream build)."""
        return dict(self._supported)

    @Property("QVariantMap", notify=listedChanged)
    def listed(self):
        """Host name -> {monitors: the PC's monitors as it answered (id, name, on, mode, modes,
        scale, scales, sideways, changed, wish), note: what went wrong ("" nothing), stale: its sign-in can't
        reach them (401, 403)}."""
        return dict(self._listed)

    @staticmethod
    def _host(name):
        return next((h for h in ft_layout.load_layout().get("hosts", []) if h.get("name") == name), None)

    def _done(self, name, status, answer):
        error = f"No host called {name}" if status == -1 else why(name, status, answer)
        # Known only from an answer (yes) or a 404 (no): a sign-in from before, or no answer,
        # leaves it as it was.
        known = True if not error else False if status == 404 else None
        if known is not None and self._supported.get(name) != known:
            self._supported[name] = known
            self.supportedChanged.emit()
        before = self._listed.get(name, {})
        monitors = answer.get("monitors", []) if not error else before.get("monitors", [])
        note = error or answer.get("error") or ""  # its last change that didn't take
        # A sign-in from before Frametop could: the page says to sign in again even with the
        # switch off, or nothing would show that the switch is there.
        self._listed[name] = {"monitors": monitors, "note": note, "stale": status in (401, 403)}
        self.listedChanged.emit()
        if not error:
            # Answers that come before the first run read the same old sizes: one run each (a
            # second would restart the stream again).
            for label, args in size_changes(self._host(name) or {}, monitors):
                if args not in self._sizing:
                    self._sizing.add(args)
                    self._run(label, *args, then=lambda args=args: self._sizing.discard(args))

    @Slot(str)
    def refresh(self, name):
        """Asks the host for its monitors (answer: listed, supported)."""
        host = self._host(name)
        if host is None or host.get("kind", "vibepollo") != "vibepollo":
            return self._answered.emit(name, -1 if host is None else 404, None)

        def work():
            self._answered.emit(name, *ft_hostmonitors.ask(host, timeout=5.0))
        threading.Thread(target=work, daemon=True).start()

    @Slot(str, bool)
    def setControl(self, name, on):
        """The host's switch: its monitors follow its display panels, or go back as they were."""
        self._run(f"{'Turning off and setting' if on else 'Leaving'} {name}'s monitors",
                  "remote", "host", name, f"monitors={'on' if on else 'off'}", then=lambda: self.refresh(name))

    @Slot(str, str, "QVariant")
    def pickMode(self, name, display_id, mode):
        """The mode a display panel's monitor is put in ({width, height, refresh_mhz}; null: its
        own), and the panel's stream size with it."""
        args = ["remote", "set", display_id, f"mode={mode_arg(mode)}"]
        monitors = self._listed.get(name, {}).get("monitors", [])
        host = self._host(name) or {}
        d = next((d for d in host.get("displays", []) if d.get("id") == display_id), {})
        monitor = next((m for m in monitors if m.get("id") == ft_hostmonitors.monitor_of(d)), None)
        if mode and monitor:
            w, h = stream_size(monitor, mode)
            args.append(f"size={w}x{h}")
        self._run("Setting the monitor's mode", *args, then=lambda: self.refresh(name))

    @Slot(str, str, "QVariant")
    def pickScale(self, name, display_id, scale):
        """The scale a display panel's monitor is put in on the PC (1.25: 125%; null: its own).
        The stream keeps its size: a scale changes the PC's desktop, not its pixels."""
        self._run("Setting the monitor's scale", "remote", "set", display_id, f"scale={scale_arg(scale)}",
                  then=lambda: self.refresh(name))
