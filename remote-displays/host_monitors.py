"""Remote Displays' "Let Frametop control NAME's displays" switch, per Vibepollo host, and the
scale picker of its display panels (docs/remote-displays.md, "A PC's monitors").

With the switch on, the host's monitors follow its display panels (layout/ft_hostmonitors.py,
which ft-layout runs whenever the panels change): while one of them streams, the PC turns off
each monitor that has no connected panel here, and puts the others at their panel's stream
size and the scale picked for it. Here the PC is asked which monitors it has (for the page:
their scales, and whether it answers at all), and the switch and the picks are saved through
ft-layout.

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


def scale_arg(scale):
    """ft-layout remote set's scale= value."""
    return "own" if not scale else f"{float(scale):g}"

def _with_sizes(monitor):
    """The PC's monitor with `largest`, [width, height] (the most its panel's stream size boxes
    go to), and `sizes`, the stream sizes it lists, largest first. Neither when it lists none."""
    sizes = ft_hostmonitors.listed_sizes(monitor) if isinstance(monitor, dict) else []
    if not sizes:
        return monitor
    return dict(monitor, largest=list(ft_hostmonitors.largest_size(monitor)), sizes=[list(s) for s in sizes])


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
        self._answered.connect(self._done, Qt.QueuedConnection)

    @Property("QVariantMap", notify=supportedChanged)
    def supported(self):
        """Host name -> True once its Vibepollo answered that it turns off and sets its monitors,
        False once it answered that it can't (an older or upstream build)."""
        return dict(self._supported)

    @Property("QVariantMap", notify=listedChanged)
    def listed(self):
        """Host name -> {monitors: the PC's monitors as it answered (id, name, on, mode, modes,
        any_mode, scale, scales, own_scale, sideways, changed, wish, and largest: the biggest
        stream size it takes), note: what went wrong ("" nothing), stale: its sign-in can't
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
        monitors = [_with_sizes(m) for m in answer.get("monitors", [])] if not error else before.get("monitors", [])
        note = error or answer.get("error") or ""  # its last change that didn't take
        # A sign-in from before Frametop could: the page says to sign in again even with the
        # switch off, or nothing would show that the switch is there.
        self._listed[name] = {"monitors": monitors, "note": note, "stale": status in (401, 403)}
        self.listedChanged.emit()

    @Slot(str)
    def refresh(self, name):
        """Asks the host for its monitors (answer: listed, supported)."""
        host = self._host(name)
        if host is None or host.get("kind", "vibepollo") != "vibepollo":
            return self._answered.emit(name, -1 if host is None else 404, None)

        def work():
            self._answered.emit(name, *ft_hostmonitors.ask(host, timeout=5.0))
        threading.Thread(target=work, daemon=True).start()

    def refresh_of(self, display_id):
        """refresh() for the host of a display panel whose monitor follows it: after its stream
        size changed, for the PC's note (a size its monitor refused)."""
        for host in ft_layout.load_layout().get("hosts", []):
            if ft_hostmonitors.controls(host) and any(
                    d.get("id") == display_id and ft_hostmonitors.monitor_of(d) for d in host.get("displays", [])):
                return self.refresh(host.get("name"))

    @Slot(str, str, int, int, result=bool)
    def cantBe(self, name, monitor_id, width, height):
        """Whether the host's monitor (as last listed) can't be put at a stream size: its panel
        then crops or fits it as it is."""
        return any(m.get("id") == monitor_id and ft_hostmonitors.cant_be(m, width, height)
                   for m in self._listed.get(name, {}).get("monitors", []))

    @Slot(str, bool)
    def setControl(self, name, on):
        """The host's switch: its monitors follow its display panels, or go back as they were."""
        self._run(f"{'Letting Frametop control' if on else 'Giving back'} {name}'s displays",
                  "remote", "host", name, f"monitors={'on' if on else 'off'}", then=lambda: self.refresh(name))

    @Slot(str, str, "QVariant")
    def pickScale(self, name, display_id, scale):
        """The scale a display panel's monitor is put in on the PC (1.25: 125%; null: its own).
        The stream keeps its size: a scale changes the PC's desktop, not its pixels."""
        self._run("Setting the monitor's scale", "remote", "set", display_id, f"scale={scale_arg(scale)}",
                  then=lambda: self.refresh(name))
