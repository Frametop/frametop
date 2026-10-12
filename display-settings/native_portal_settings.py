"""Asynchronous native portal settings. ft-screens, not this GUI, owns On intent."""
import copy
import math
import os
import sys
import subprocess
import threading
import time
from collections import deque

from PySide6.QtCore import Property, QCoreApplication, QObject, Qt, QTimer, Signal, Slot

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "layout"))
import ft_layout
from native_portal_client import ADDRESS, Client, PortalError

def status_summary(value):
    if not value["wanted"]:
        return "Desk passthrough off"
    return "Desk passthrough requested" if value["connected"] else "Passthrough publisher unavailable"


def unknown(reason):
    return {"known": False, "wanted": None, "summary": "Native portal state unknown: " + reason,
                "reason": reason}


class PortalSettings(QObject):
    changed = Signal()
    message = Signal(str, bool)
    _done = Signal(int, str, object)

    def __init__(self, parent=None, *, address=ADDRESS, timeout=1.0, tracker_command=None):
        super().__init__(parent)
        self._client = Client(address, timeout)
        self._tracker_command = tracker_command or [os.path.join(os.path.dirname(__file__), "..", "hands", "ft-cutouts"), "ensure"]
        self._hands = {"known": False, "wanted": False, "summary": "Hand passthrough state unknown"}
        self._portal = unknown("awaiting backend status")
        self._requested = None
        self._volume = ft_layout.passthrough(ft_layout.load_layout())
        self._geometry_known = False
        self._geometry_summary = "Saved placement; live placement unknown"
        layout = ft_layout.load_layout()
        self._preferences = {key: {"known": False, "enabled": None, "saved": saved, "summary": "Live preference unknown"}
                             for key, saved in (("controls", ft_layout.passthrough_controls(layout)),
                                                ("layer", ft_layout.portal_above_steam(layout)))}
        self._queue = deque()
        self._queue_lock = threading.Lock()
        self._running = False
        self._pending = 0
        self._closed = False
        self._generation = 0
        self._done.connect(self._finish, Qt.QueuedConnection)
        self._timer = QTimer(self, interval=1500, timeout=self.refresh)
        app = QCoreApplication.instance()
        if app is not None:
            app.aboutToQuit.connect(self.close)
        self._timer.start()
        self.refresh()

    @Property("QVariantMap", notify=changed)
    def portal(self):
        return dict(self._portal, requested=self._requested, busy=self.busy)

    @Property("QVariantMap", notify=changed)
    def hands(self):
        return dict(self._hands)

    @Property("QVariantMap", notify=changed)
    def volume(self):
        return copy.deepcopy(self._volume)

    @Property("QVariantMap", notify=changed)
    def preferences(self):
        return copy.deepcopy(self._preferences)

    @Property(bool, notify=changed)
    def busy(self):
        return self._pending != 0

    @Property(bool, notify=changed)
    def geometryKnown(self):
        return self._geometry_known

    @Property(str, notify=changed)
    def geometrySummary(self):
        return self._geometry_summary

    @Slot()
    def close(self):
        """Stop polling and new submissions, not accepted work. No implicit On/Off."""
        self._closed = True
        self._timer.stop()

    @Slot()
    def refresh(self):
        if self._closed or self.busy or ft_layout.backend() != "screens":
            return
        self._enqueue("poll", None)

    @Slot(bool)
    def setWanted(self, enabled):
        if self._closed or ft_layout.backend() != "screens":
            return
        self._requested = enabled
        self._portal = unknown("request pending; acknowledgement and readback required")
        self._enqueue("intent", enabled)

    @Slot(bool)
    def setHands(self, enabled):
        self._enqueue("hands", enabled)

    @Slot(bool)
    def setControls(self, enabled):
        self._enqueue("controls", enabled)

    @Slot(bool)
    def setAboveSteam(self, enabled):
        self._enqueue("layer", enabled)

    @Slot()
    def placeAtDesk(self):
        self._enqueue("place", None)

    def _enqueue(self, kind, value):
        if self._closed or ft_layout.backend() != "screens":
            return
        if kind != "poll":
            self._generation += 1
        self._pending += 1
        with self._queue_lock:
            self._queue.append((self._generation, kind, value))
        self._start()
        self.changed.emit()

    def _start(self):
        with self._queue_lock:
            if self._running or not self._queue:
                return
            self._running = True
            # Python waits for this finite queue at interpreter exit. Advancing it
            # must not depend on Qt delivering a signal or the QObject surviving.
            threading.Thread(target=self._work, daemon=False).start()

    def _work(self):
        while True:
            with self._queue_lock:
                if not self._queue:
                    self._running = False
                    return
                generation, kind, value = self._queue.popleft()
            try:
                result = self._perform(kind, value)
            except (OSError, RuntimeError, ValueError) as exc:
                result = {"error": str(exc)}
            try:
                self._done.emit(generation, kind, result)
            except RuntimeError:
                pass  # UI destruction must not discard the next accepted command.

    def _status(self):
        try:
            value = self._client.status()
            return dict(value, known=True, summary=status_summary(value))
        except PortalError as exc:
            return unknown(str(exc))

    def _save_field(self, key, value):
        # Keep the existing raw-JSON writer and shared layout lock, but do not let
        # another layout process stall queued portal Off forever.
        deadline = time.monotonic() + self._client.timeout
        with open(ft_layout.LOCK_PATH, "w") as lock:
            while True:
                try:
                    ft_layout.fcntl.flock(lock, ft_layout.fcntl.LOCK_EX | ft_layout.fcntl.LOCK_NB)
                    break
                except BlockingIOError:
                    if time.monotonic() >= deadline:
                        raise RuntimeError("Layout is busy; preference was not saved")
                    time.sleep(.01)
            ft_layout._write_layout_field(key, value)

    def _preference(self, key, enabled=None):
        parse = ft_layout.parse_passthrough_controls if key == "controls" else ft_layout.parse_portal_above_steam
        field = "passthrough_controls" if key == "controls" else "portal_above_steam"
        saved = (ft_layout.passthrough_controls if key == "controls" else ft_layout.portal_above_steam)(ft_layout.load_layout())
        try:
            if enabled is not None:
                self._save_field(field, enabled)
                saved = enabled
            deadline = time.monotonic() + self._client.timeout
            if enabled is not None:
                ack = parse(self._client.ask(f"passthrough {key} {int(enabled)}", deadline=deadline))
                if ack != enabled:
                    raise ValueError("acknowledgement did not match")
            actual = parse(self._client.ask(f"passthrough {key} state", deadline=deadline))
            if enabled is not None and actual != enabled:
                raise ValueError("readback did not match")
            label = ("Controls shown" if actual else "Controls hidden") if key == "controls" else (
                "Passthrough above Steam menus" if actual else "Passthrough behind Steam menus")
            return {"known": True, "enabled": actual, "saved": saved, "summary": label}
        except (OSError, RuntimeError, ValueError) as exc:
            label = "Saved preference; not applied: " if enabled is not None and saved == enabled else "Live preference unknown: "
            return {"known": False, "enabled": None, "saved": saved, "summary": label + str(exc)}

    def _geometry(self, kind):
        live = False
        try:
            volume, _ = ft_layout.parse_passthrough_state(self._client.ask("passthrough state3d"))
            live = True
        except (PortalError, ValueError):
            volume = ft_layout.passthrough(ft_layout.load_layout())
        if kind == "poll":
            return {"volume": volume, "geometryKnown": live,
                        "geometrySummary": "Live placement" if live else "Saved placement; live placement unknown"}
        if kind == "place":
            fields = self._client.ask("head").split()
            if len(fields) != 5 or fields[0] != "ok":
                raise ValueError("No valid head pose for Place at desk")
            x, y, z, yaw = map(float, fields[1:])
            if not all(math.isfinite(v) for v in (x, y, z, yaw)):
                raise ValueError("Head pose needs finite numbers")
            angle = math.radians(yaw)
            volume["yaw"] = yaw
            ft_layout.set_passthrough_rectangle_center(volume,
                [x - .65 * math.sin(angle), y - .55, z - .65 * math.cos(angle)])
        volume = ft_layout.validate_passthrough(volume)
        self._save_field("passthrough", volume)
        if not live:
            # The full setter would replace unedited live axes with saved values.
            return {"volume": volume, "geometryKnown": False,
                    "geometrySummary": "Saved placement; not applied: live placement unavailable"}
        try:
            deadline = time.monotonic() + self._client.timeout
            ack = self._client.ask(ft_layout.passthrough_command({"passthrough": volume}), deadline=deadline)
            acknowledged, _ = ft_layout.parse_passthrough_state(ack)
            actual, _ = ft_layout.parse_passthrough_state(self._client.ask("passthrough state3d", deadline=deadline))
            # The set acknowledgement is legacy yaw-only, even for a full 3D setter.
            for key in ("enabled", "center", "size", "yaw"):
                left = acknowledged[key] if isinstance(acknowledged[key], list) else [acknowledged[key]]
                right = volume[key] if isinstance(volume[key], list) else [volume[key]]
                if not all(math.isclose(a, b, abs_tol=1e-5, rel_tol=1e-5) for a, b in zip(left, right)):
                    raise ValueError("placement acknowledgement did not match")
            # state3d rounds native floats for the wire; compare within native output precision.
            numbers = lambda v: [*v["center"], *v["size"], v["yaw"], v.get("pitch", 0), v.get("roll", 0)]
            if actual["enabled"] != volume["enabled"] or not all(math.isclose(a, b, abs_tol=1e-5, rel_tol=1e-5)
                                                                for a, b in zip(numbers(actual), numbers(volume))):
                raise ValueError("placement readback did not match")
            return {"volume": actual, "geometryKnown": True, "geometrySummary": "Saved and applied placement"}
        except (PortalError, ValueError) as exc:
            return {"volume": volume, "geometryKnown": False, "geometrySummary": "Saved placement; not applied: " + str(exc)}

    def _hand_intent(self, enabled=None):
        # Like desk intent, hand intent is live only; reopening settings never starts cameras.
        try:
            if enabled:
                try:
                    result = subprocess.run(self._tracker_command, capture_output=True, text=True, timeout=20)
                except subprocess.TimeoutExpired as exc:
                    raise RuntimeError("Hand tracker startup timed out") from exc
                if result.returncode:
                    raise RuntimeError((result.stderr or result.stdout).strip() or "Hand tracker could not start")
            def parse(reply):
                if reply not in ("ok hands 0", "ok hands 1"):
                    raise ValueError("Hand passthrough reply invalid: " + reply)
                return reply.endswith("1")
            if enabled is not None and parse(self._client.ask(f"passthrough hands {int(enabled)}")) != enabled:
                raise ValueError("Hand passthrough acknowledgement did not match")
            actual = parse(self._client.ask("passthrough hands state"))
            if enabled is not None and actual != enabled:
                raise ValueError("Hand passthrough readback did not match")
            connected = self._client.status()["connected"] if actual else None
            return {"known": True, "wanted": actual, "connected": connected,
                    "summary": ("Hand passthrough requested" if connected else "Hand passthrough publisher unavailable")
                               if actual else "Hand passthrough off"}
        except (OSError, RuntimeError, ValueError) as exc:
            return {"known": False, "wanted": False, "summary": str(exc)}

    def _perform(self, kind, value):
        if kind == "poll":
            return dict(portal=self._status(), hands=self._hand_intent(), preferences={key: self._preference(key) for key in ("controls", "layer")},
                        **self._geometry("poll"))
        if kind == "hands":
            return {"hands": self._hand_intent(value)}
        if kind == "intent":
            actual = self._client.set_wanted(value)
            return {"portal": dict(actual, known=True, summary=status_summary(actual))}
        if kind in ("controls", "layer"):
            if type(value) is not bool:
                raise ValueError("Portal preference must be true or false")
            return {"preferences": {kind: self._preference(kind, value)}}
        return self._geometry(kind)

    @Slot(int, str, object)
    def _finish(self, generation, kind, result):
        self._pending -= 1
        # Commands are always sent serially. A pre-command poll cannot overwrite newer UI intent.
        if not self._closed and (kind != "poll" or generation == self._generation):
            if "error" in result:
                if kind == "intent" and generation == self._generation:
                    self._portal = unknown(result["error"])
                    self._requested = None
                self.message.emit(result["error"], True)
            if "portal" in result and generation == self._generation:
                self._portal = result["portal"]
                self._requested = self._portal["wanted"]
            if "hands" in result:
                self._hands = result["hands"]
            self._preferences.update(result.get("preferences", {}))
            if "volume" in result:
                self._volume = result["volume"]
                self._geometry_known = result["geometryKnown"]
                self._geometry_summary = result["geometrySummary"]
        self.changed.emit()
