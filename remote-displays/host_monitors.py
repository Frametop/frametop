"""Remote Displays' "Monitors": a PC's own monitors turned on or off and put in another mode, from
Frametop's build of Vibepollo (/api/frametop/monitors; docs/remote-displays.md, "A PC's
monitors").

The PC keeps the plan and applies it only while Frametop streams from it; two minutes after the
last stream ends (or at a reboot) every monitor goes back as it was. Here the plan is sent, and
the panels of the PC's monitors follow it: a monitor's panel streams at its mode pixel for pixel
(ft-layout remote set ... size=), the panel of a monitor turned off is disconnected (it has
nothing to show, and a panel trying again would keep the PC "in use"), and connected again when
the monitor comes back on.

The backend (ft_remote_displays.py) makes one and shows it to the page as hostMonitors. Questions
to the PC run on a thread; changes to the panels go in the backend's queue of ft-layout runs.
"""
import threading

from PySide6.QtCore import Property, QObject, Qt, Signal, Slot

# conf/ and layout/, put on the path by the backend
import ft_hostapi
import ft_layout

PATH = "/api/frametop/monitors"
NO_BUILD = ("Its Vibepollo can't change its monitors: that needs Frametop's build of Vibepollo, "
            "a newer one than it has.")


def why(name, status, answer):
    """What to tell the user about an answer that isn't a plan: "" when it is one."""
    if status == 200 and isinstance(answer, dict) and answer.get("status") is True:
        return ""
    if status == 0:
        return f"Couldn't reach {name}"
    if status in (401, 403):
        return f"Sign in to {name} again to change its monitors (its sign-in is from before Frametop could)"
    if status == 404:
        return NO_BUILD
    error = answer.get("error") if isinstance(answer, dict) else None
    return error if isinstance(error, str) and error else f"{name} answered {status}"


def plan_body(wishes):
    """POST's body from the page's wishes: {id: {on, width, height, refresh_mhz}}, a mode only
    for a monitor that's on (width 0: as it is)."""
    monitors = {}
    for monitor_id, w in (wishes or {}).items():
        wish = {"on": bool(w.get("on"))}
        if wish["on"] and int(w.get("width") or 0) > 0:
            wish["mode"] = {"width": int(w["width"]), "height": int(w["height"]),
                            "refresh_mhz": int(w.get("refresh_mhz") or 0)}
        monitors[str(monitor_id)] = wish
    return {"monitors": monitors}


def _effective(monitor):
    """(on, mode) the monitor will have while Frametop streams: the wish, or as it is."""
    wish = monitor.get("wish")
    if not isinstance(wish, dict):
        return bool(monitor.get("on")), monitor.get("mode")
    return bool(wish.get("on")), wish.get("mode") or monitor.get("mode")


def panel_changes(host, state, parked):
    """ft-layout runs that make the host's monitor panels follow its plan: [(label, args)].
    parked: ids of panels disconnected here because their monitor went off; the ones connected
    again are taken out, the ones disconnected now put in."""
    monitors = {m.get("id"): m for m in (state or {}).get("monitors", []) if isinstance(m, dict)}
    runs = []
    for d in host.get("displays", []):
        app = str(d.get("app", ""))
        if d.get("window") or not app.startswith("display:"):
            continue
        monitor = monitors.get(app[len("display:"):])
        if monitor is None:
            continue
        on, mode = _effective(monitor)
        if not on:
            if not d.get("off"):
                runs.append((f"Disconnecting {d.get('label') or d['id']}", ("remote", "disconnect", d["id"])))
                parked.add(d["id"])
            continue
        if isinstance(mode, dict) and mode.get("width") and mode.get("height"):
            size = [int(mode["width"]), int(mode["height"])]
            if list(d.get("size", [])) != size:
                runs.append((f"Streaming {d.get('label') or d['id']} at its mode",
                             ("remote", "set", d["id"], f"size={size[0]}x{size[1]}")))
        if d.get("off") and d["id"] in parked:
            runs.append((f"Connecting {d.get('label') or d['id']}", ("remote", "connect", d["id"])))
            parked.discard(d["id"])
    return runs


class HostMonitors(QObject):
    # a host's name, its monitors' state ({monitors: [...], grace_seconds, streams, ...}), what
    # went wrong ("" nothing)
    monitorsReady = Signal(str, "QVariantMap", str)
    supportedChanged = Signal()
    _answered = Signal(str, int, object)  # name, HTTP status (0: no answer, -1: not asked), answer

    def __init__(self, run, parent=None):
        """run(label, *ft_layout_args): the backend's queue of ft-layout runs."""
        super().__init__(parent)
        self._run = run
        self._supported = {}  # host name -> whether its Vibepollo changes its monitors
        self._parked = set()  # panels disconnected here because their monitor went off
        self._answered.connect(self._done, Qt.QueuedConnection)

    @Property("QVariantMap", notify=supportedChanged)
    def supported(self):
        """Host name -> True once its Vibepollo answered that it changes its monitors (then its
        monitors' panels stream at their mode, and the page has no stream size for them), False
        once it answered that it can't (an older or upstream build)."""
        return dict(self._supported)

    @staticmethod
    def _host(name):
        return next((h for h in ft_layout.load_layout().get("hosts", []) if h.get("name") == name), None)

    def _ask(self, name, method, body=None):
        host = self._host(name)
        if host is None or host.get("kind", "vibepollo") != "vibepollo":
            return self._answered.emit(name, -1 if host is None else 404, None)

        def work():
            status, answer = ft_hostapi.request(host["address"], method, PATH, body=body,
                                                timeout=20.0 if body is not None else 5.0)
            self._answered.emit(name, status, answer)
        threading.Thread(target=work, daemon=True).start()

    def _done(self, name, status, answer):
        error = f"No host called {name}" if status == -1 else why(name, status, answer)
        # Known only from a plan (yes) or a 404 (no): a sign-in from before, or no answer, leaves
        # it as it was, so the page keeps the button that says to sign in again.
        known = True if not error else False if status == 404 else None
        if known is not None and self._supported.get(name) != known:
            self._supported[name] = known
            self.supportedChanged.emit()
        state = {} if error else answer
        if state:
            for label, args in panel_changes(self._host(name) or {}, state, self._parked):
                self._run(label, *args)
        self.monitorsReady.emit(name, state, error)

    @Slot(str)
    def refresh(self, name):
        """The host's monitors and its plan for them (answer: monitorsReady)."""
        self._ask(name, "GET")

    @Slot(str, "QVariantMap")
    def apply(self, name, wishes):
        """Sends the whole plan (a monitor left out goes back as it was) (answer: monitorsReady)."""
        self._ask(name, "POST", plan_body(wishes))

    @Slot(str)
    def putBack(self, name):
        """No plan: every monitor back as it was (answer: monitorsReady)."""
        self._ask(name, "POST", {"monitors": {}})
