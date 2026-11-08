#!/usr/bin/env python3
"""Frametop Settings: the Frametop screens' count, resolution, size, scale, and layout.

A Kirigami (QML) app with a Python backend, like Frametop Settings. It runs in
the dev container:
  - Screens (ft-screens backend): each screen's resolution (any, portrait too), its width
    in VR in metres, its scale, and which one has the taskbar. Resolution and width
    apply at once (ft-screens' control socket, @ft_screens); adding or removing a screen
    when the desktop starts again. (gamescope backend: one shared resolution, at most
    1920x1080 worth of pixels, rotation for portrait.)
  - Visibility (ft-screens): when the screens show (always, only with the SteamVR
    dashboard open, while you look at a controller, or only when toggled), the wrist
    angle within which a pinned screen shows, pinning each screen to a wrist or your
    head, whether the desktop's notifications go to the Steam session
    (NOTIFY_FORWARD, session/ft-notifyfwd; its state from its log), and whether apps from
    the SteamVR dashboard's Launch a program list open as floating windows (DASHBOARD_APPS,
    float/ft-dashroute).
  - Layout: a preset (curved or flat, rows, distance, gap, height) or a named layout
    saved from where the screens are, with a preview; arrange now; save the current
    arrangement under a name; rename and delete; arrange automatically when the
    desktop starts.
  - Remote displays (other computers' monitors as screens) have their own app, Frametop
    Remote Displays (remote-displays/); a button opens it.
  - Power: how long the headset can go unused before ft-powerd turns its displays off
    (DISPLAY_OFF_MIN; the service's state comes from its control socket, @ft_powerd),
    whether the Frame stays awake while plugged in, which is Steam's own setting
    (steam_settings.py; the value from before is kept as STEAM_SLEEP_AC_BEFORE), and whether
    the desktop starts with SteamVR (AUTOSTART, session/autostart.sh).
Settings go to ~/.config/frametop.conf and ~/.config/frametop-layout.json. Anything
that touches SteamVR runs layout/ft-layout on the host.
Frametop Settings (settings/ft_settings.py) makes it, the first time one of its pages opens.
"""
import json
import os
import sys
import threading
import time

from PySide6.QtCore import Property, QObject, QProcess, Qt, QTimer, QUrl, Signal, Slot

HERE = os.path.dirname(os.path.abspath(__file__))
LAYOUT_DIR = os.path.join(HERE, "..", "layout")
sys.path.insert(0, LAYOUT_DIR)
import ft_layout  # noqa: E402  (pure Python: the same geometry ft-layout uses)
import ftipc  # noqa: E402  (questions to the datagram sockets; conf/, on the path from ft_layout)
from fthost import host_command  # noqa: E402  (commands on the SteamOS host)
from ft_layout import ft_conf  # noqa: E402  (frametop.conf's grammar, docs/config.md)
import steam_settings  # noqa: E402
import stereo_wallpaper  # noqa: E402  (DEE-317: the pictures here, the rest on the host)

FT_LAYOUT = os.path.join(LAYOUT_DIR, "ft-layout")
DESKTOPS = os.path.join(HERE, "..", "desktops.sh")
CONF_PATH = ft_layout.CONF_PATH
# gamescope's VR backend uploads a texture the size of a screen at start, through a
# 1920x1080x4-byte buffer: more pixels than 1920x1080 abort it (see the design notes).
MAX_PIXELS = 1920 * 1080
RESOLUTIONS = [(1280, 720), (1600, 900), (1920, 1080), (1728, 1080), (1920, 800), (2224, 928), (2560, 800)]
SCREEN_RESOLUTIONS = ft_layout.SCREEN_RESOLUTIONS
FT_SCREENS = "\0ft_screens"
FT_POWERD = "\0ft_powerd"
# Steam's default for "When Plugged In and Idle -> Sleep after", to go back to when
# nothing was saved.
STEAM_SLEEP_AC_DEFAULT = 3600
SCALES = [0.75, 1.0, 1.25, 4 / 3, 1.5, 1.75, 2.0]
NOTIFY_FORWARD = ("auto", "on", "off")  # session/ft-notifyfwd; frametop.conf's NOTIFY_FORWARD
DASHBOARD_APPS = ("steamvr", "frametop")  # float/ft-dashroute; frametop.conf's DASHBOARD_APPS
NOTIFYFWD_LOG = "/tmp/frametop-notifyfwd.log"  # its last line is its state (/tmp is the host's)
AUTOSTART_UNIT = os.path.expanduser("~/.config/systemd/user/frametop-autostart.service")
ROTATIONS = [("normal", "Landscape"), ("left", "Portrait"), ("right", "Portrait (flipped)")]


def write_conf_value(key, value):
    """Set KEY=value in frametop.conf, keeping comments and the rest of the file (shell-quoted,
    by a temp file and a rename: ft_conf.set_value)."""
    ft_conf.set_value(key, value, CONF_PATH)


class Backend(QObject):
    changed = Signal()
    busyChanged = Signal()
    powerChanged = Signal()
    message = Signal(str, bool)  # text, is error
    _steamDone = Signal(object, object, str)  # Steam's sleep settings or None, error or None, what was done
    previewChanged = Signal()
    _roomRead = Signal(object, object)  # ft_layout.read_room's room or None, the floating windows [(panel, title)]
    _ipcDone = Signal(object, object)  # a callback and its answer, from the ftipc worker

    def __init__(self):
        super().__init__()
        self._busy = ""
        self._proc = None
        self._running = False
        self._running_count = 0
        # ft-screens and ft-powerd are asked on a thread of their own, never the UI's
        self._ipcDone.connect(lambda then, answer: then(answer), Qt.QueuedConnection)
        self._ipc = ftipc.Worker(self._ipcDone.emit)
        self._polling = False
        self._started = {}  # conf values the running desktop started with
        self._powerd = None  # ft-powerd's status: (state, seconds unused, timeout seconds); None: not running
        self._steam = None  # Steam's sleep settings: {"ac": seconds, "battery": seconds}
        self._steam_error = ""
        self._steam_busy = False
        self._steamDone.connect(self._steam_done, Qt.QueuedConnection)
        self._pins = []     # each running screen's pin: none | left | right | head
        self._notify_state = ""
        self.poll = QTimer(interval=3000, timeout=self._check_running)
        self.poll.start()
        self._check_running()
        # The Layout page's preview: the panels and the head now, read while it shows.
        self._room = None
        self._windows = []      # the floating windows, from ft-floatd's spotlist
        self._windows_at = 0.0  # when it was asked
        self._room_busy = False
        self._roomRead.connect(self._room_read, Qt.QueuedConnection)
        self.changed.connect(self.previewChanged)
        self.roomPoll = QTimer(interval=1000, timeout=self._read_room)

    # --- state ---
    def _conf(self):
        conf = ft_layout.read_conf()
        def num(key, default, cast=float):
            try:
                return cast(conf.get(key, default))
            except ValueError:
                return default
        notify = conf.get("NOTIFY_FORWARD", "auto")
        dash = conf.get("DASHBOARD_APPS", "steamvr")
        floats = num("FLOAT_SLOTS", 16, int)
        return {"dashboardApps": dash if dash in DASHBOARD_APPS else "steamvr", "floatSlots": floats,
                "screens": num("SCREENS", 2, int), "width": num("WIDTH", 1920, int),
                "height": num("HEIGHT", 1080, int), "physWidth": num("PHYS_WIDTH", 1.6),
                "notifyForward": notify if notify in NOTIFY_FORWARD else "off",
                "autostart": conf.get("AUTOSTART", "0") == "1"}

    def _check_running(self):
        """Reads the desktop's state on the worker; _polled gets it here (one read at a time)."""
        if not self._polling:
            self._polling = True
            self._ipc.run(self._poll, then=self._polled)

    def _poll(self, client):
        """On the worker's thread."""
        # The session's own Wayland socket (host processes' environments aren't readable
        # from the container, so ft_layout.nested_env() doesn't work here).
        running = os.path.exists(f"/run/user/{os.getuid()}/frametop/wayland-0")
        count = 0
        if running and ft_layout.backend() == "screens":
            reply = client.ask(FT_SCREENS, "screens")
            count = int(reply.split()[1]) if reply and reply.startswith("ok") else 0
        pins = []
        for i in range(count):
            reply = client.ask(FT_SCREENS, f"get {i + 1}")
            f = reply.split() if reply and reply.startswith("ok") else []
            pins.append(f[16] if len(f) > 16 else "none")
        notify_state = self._read_notify_state() if running else ""
        return running, count, pins, notify_state, self._powerd_status(client)

    def _polled(self, result):
        self._polling = False
        if result is None:
            return
        running, count, pins, notify_state, powerd = result
        if (running != self._running or count != self._running_count or pins != self._pins
                or notify_state != self._notify_state):
            if running != self._running or count != self._running_count:
                self._started = self._conf() if running else {}
            self._running = running
            self._running_count = count
            self._pins = pins
            self._notify_state = notify_state
            self.changed.emit()
        self._set_powerd(powerd)

    @staticmethod
    def _read_notify_state():
        """ft-notifyfwd's: forwarding | waiting (no usable server on the user bus) | "" (not running)."""
        try:
            with open(NOTIFYFWD_LOG) as f:
                lines = f.read().splitlines()
        except OSError:
            return ""
        # the last state line: errors it logs in between (a failed Notify) don't change it
        for line in reversed(lines):
            if line == "ft-notifyfwd: forwarding":
                return "forwarding"
            if line.startswith("ft-notifyfwd: not forwarding"):
                return "waiting"
        return ""

    def _ask_screens(self, text, then=None):
        """A question to ft-screens, on the worker; then(answer, or None if it isn't running)."""
        self._ipc.ask(FT_SCREENS, text, then)

    @Property(bool, notify=changed)
    def desktopRunning(self):
        return self._running

    @Property(str, notify=changed)
    def backend(self):
        return ft_layout.backend()

    @Property(bool, notify=changed)
    def restartNeeded(self):
        """ft-screens: screens added or removed since the desktop started; gamescope: count,
        resolution, or width changed."""
        if not self._running:
            return False
        if ft_layout.backend() == "screens":
            return self._running_count != ft_layout.screen_count(ft_layout.load_layout())
        return bool(self._started) and self._started != self._conf()

    @Property("QVariantList", constant=True)
    def screenResolutions(self):
        return [{"text": f"{w} × {h}" + (f"  ({t})" if t else ""), "width": w, "height": h}
                for w, h, t in SCREEN_RESOLUTIONS]

    @Property(int, notify=changed)
    def screens(self):
        return self._conf()["screens"]

    @Property(int, notify=changed)
    def screenCount(self):
        """The desktop's screens: the layout's with ft-screens, SCREENS with gamescope."""
        return ft_layout.screen_count(ft_layout.load_layout())

    @Property(int, notify=changed)
    def width(self):
        return self._conf()["width"]

    @Property(int, notify=changed)
    def height(self):
        return self._conf()["height"]

    @Property(float, notify=changed)
    def physWidth(self):
        return self._conf()["physWidth"]

    @Property("QVariantList", constant=True)
    def resolutions(self):
        return [{"text": f"{w} × {h}", "width": w, "height": h} for w, h in RESOLUTIONS]

    @Property("QVariantList", constant=True)
    def scales(self):
        return [{"text": f"{round(s * 100)}%", "value": round(s, 4)} for s in SCALES]

    @Property("QVariantList", constant=True)
    def rotations(self):
        return [{"text": t, "value": v} for v, t in ROTATIONS]

    @Property("QVariantList", notify=changed)
    def screenList(self):
        c, layout = self._conf(), ft_layout.load_layout()
        out = []
        if ft_layout.backend() == "screens":
            primary = ft_layout.primary_screen(layout)
            for i in range(ft_layout.screen_count(layout)):
                w, h = ft_layout.screen_pixels(layout, i)
                s = ft_layout.screen_scale(layout, i)
                out.append({"index": i, "width": w, "height": h, "metres": ft_layout.screen_metres(layout, i),
                            "scale": round(s, 4), "primary": i == primary,
                            "curved": float(ft_layout.screen_entry(layout, i).get("curve", 0)) > 0,
                            "effective": f"{round(w / s)} × {round(h / s)}"})
            return out
        for i in range(c["screens"]):
            s = ft_layout.screen_scale(layout, i)
            rot = ft_layout.screen_rotation(layout, i)
            w, h = (c["height"], c["width"]) if rot != "normal" else (c["width"], c["height"])
            out.append({"index": i, "scale": round(s, 4), "rotation": rot,
                        "effective": f"{round(w / s)} × {round(h / s)}"})
        return out

    @Property("QVariantMap", notify=changed)
    def layout(self):
        return ft_layout.load_layout()

    @Property("QVariantMap", notify=previewChanged)
    def preview(self):
        """The preview's panels and head (ft_layout.preview), live while the desktop runs."""
        layout = ft_layout.load_layout()
        if ft_layout.backend() != "screens":
            # Before any panel was measured: the width SteamVR floats a panel at, and the aspect.
            if "panel_size" not in layout or layout["panel_size"] == list(ft_layout.DEFAULT_PANEL):
                c = self._conf()
                layout["panel_size"] = [ft_layout.DEFAULT_PANEL[0], ft_layout.DEFAULT_PANEL[0] * c["height"] / c["width"]]
            return ft_layout.preview(layout)
        return ft_layout.preview(layout, self._room if self._running else None)

    @Slot(bool)
    def watchRoom(self, on):
        """The Layout page shows (or not): read where everything is every second meanwhile."""
        if on and ft_layout.backend() == "screens":
            self._windows_at = 0.0
            self.roomPoll.start()
            self._read_room()
        else:
            try:
                self.roomPoll.stop()
            except RuntimeError:
                pass  # the app is closing: the page goes after the timer

    def _read_room(self):
        """ft-screens' panels and head off the UI thread (and ft-floatd's windows, which waits
        for KWin, every few seconds); _room_read gets them."""
        if self._room_busy or not self._running:
            return
        self._room_busy = True
        layout = ft_layout.load_layout()
        windows = self._windows if time.monotonic() - self._windows_at < 3 else None

        def work():
            room, listed, clients = None, windows, []

            def asker(timeout):
                """ask(text, to) -> reply or None, on a client of its own. After one question
                times out it asks nothing more (ft-screens is busy: the rest would wait too)."""
                client = ftipc.Client(timeout)
                clients.append(client)

                def ask(text, to=FT_SCREENS):
                    if client.why == ftipc.TIMEOUT:
                        return None
                    return client.ask(to, text)
                return ask
            try:
                if listed is None:
                    reply = asker(4)("spotlist", ft_layout.FLOAT) or ""
                    try:
                        listed = [(int(e["panel"]), e.get("title") or e.get("app") or "window")
                                  for e in json.loads(reply[3:]) if e.get("floating") and e.get("panel")
                                  and not e.get("minimized")] if reply.startswith("ok ") else []
                    except (ValueError, TypeError, KeyError, AttributeError):
                        listed = []
                room = ft_layout.read_room(asker(0.5), layout, listed)
            except (ValueError, TypeError, KeyError, IndexError, OSError):
                room = None  # a reply it can't read: no live preview this time, the next read tries again
            finally:
                for client in clients:
                    client.close()
                try:
                    # Always, so _room_busy clears whatever went wrong.
                    self._roomRead.emit(room, listed if windows is None else None)
                except RuntimeError:
                    pass  # the window closed meanwhile

        threading.Thread(target=work, daemon=True).start()

    def _room_read(self, room, windows):
        self._room_busy = False
        if windows is not None:
            self._windows, self._windows_at = windows, time.monotonic()
        if room != self._room:  # nothing moved: no repaint
            self._room = room
            self.previewChanged.emit()

    @Property(str, notify=busyChanged)
    def busy(self):
        return self._busy

    # --- screens ---
    @Slot(int)
    def setScreens(self, n):
        write_conf_value("SCREENS", str(max(1, min(6, n))))
        self.changed.emit()

    @Slot(int, int)
    def setResolution(self, w, h):
        if w * h > MAX_PIXELS:
            self.message.emit(f"{w} × {h} is more than gamescope's VR mode can draw (1920 × 1080 worth of pixels, "
                              f"about {MAX_PIXELS // 1000} thousand); try {round((MAX_PIXELS * w / h) ** 0.5) // 8 * 8} × "
                              f"{round((MAX_PIXELS * h / w) ** 0.5) // 8 * 8}", True)
            return
        if w >= 640 and h >= 360:
            write_conf_value("WIDTH", str(w))
            write_conf_value("HEIGHT", str(h))
            self.changed.emit()

    @Slot(float)
    def setPhysWidth(self, w):
        write_conf_value("PHYS_WIDTH", f"{w:.2f}")
        self.changed.emit()

    @Slot(int, float)
    def setScale(self, i, s):
        layout = ft_layout.load_layout()
        screens = layout.setdefault("screens", [])
        while len(screens) <= i:
            screens.append({})
        screens[i]["scale"] = s
        if not self._save(layout):
            return
        self.changed.emit()
        if self._running:
            self._run("Applying scale", "scale")

    @Slot(int, str)
    def setRotation(self, i, rotation):
        layout = ft_layout.load_layout()
        screens = layout.setdefault("screens", [])
        while len(screens) <= i:
            screens.append({})
        screens[i]["rotation"] = rotation
        screens[i].pop("roll", None)  # a saved arrangement follows the new rotation
        if not self._save(layout):
            return
        self.changed.emit()
        if self._running:
            self._run("Rotating", "scale")

    # --- ft-screens: per-screen resolution and size ---
    def _edit_screen(self, i, fn):
        layout = ft_layout.load_layout()
        screens = layout.setdefault("screens", [])
        while len(screens) <= i:
            screens.append({"size": [1920, 1080], "metres": 1920 / ft_layout.PIXELS_PER_METRE})
        fn(screens[i])
        if not self._save(layout):
            return False
        self.changed.emit()
        return True

    @Slot(int, int, int)
    def setScreenSize(self, i, w, h):
        if not (320 <= w <= 16384 and 200 <= h <= 16384):
            self.message.emit("Width and height: 320 to 16384 pixels", True)
            return
        saved = self._edit_screen(i, lambda s: s.__setitem__("size", [w, h]))
        if saved and self._running and i < self._running_count:
            self._ask_screens(f"size {i + 1} {w} {h}")

    @Slot(int, float)
    def setScreenMetres(self, i, m):
        saved = self._edit_screen(i, lambda s: s.__setitem__("metres", round(m, 3)))
        if saved and self._running and i < self._running_count:
            self._ask_screens(f"width {i + 1} {m:.3f}")

    @Slot(int, bool)
    def setCurved(self, i, on):
        """Curve a screen into a cylinder around you (radius: your distance to it now, from
        ft-screens; the layout's distance if the desktop isn't running)."""
        radius = float(ft_layout.load_layout()["preset"].get("distance", 2.0)) if on else 0.0
        def curve(reply=None):
            r = float(reply.split()[1]) if reply and reply.startswith("ok") else radius
            self._edit_screen(i, lambda s: s.__setitem__("curve", round(r, 3)))
        if self._running and i < self._running_count:
            self._ask_screens(f"curve {i + 1} {'on' if on else 'off'}", then=curve)
        else:
            curve()

    @Slot(int)
    def setPrimary(self, i):
        layout = ft_layout.load_layout()
        layout["primary"] = i + 1
        if not self._save(layout):
            return
        self.changed.emit()
        if self._running:
            self._run("Moving the taskbar", "scale")

    @Slot()
    def addScreen(self):
        layout = ft_layout.load_layout()
        layout.setdefault("screens", []).append({"size": [1920, 1080], "metres": 1920 / ft_layout.PIXELS_PER_METRE})
        if not self._save(layout):
            return
        self.changed.emit()

    @Slot(int)
    def removeScreen(self, i):
        layout = ft_layout.load_layout()
        screens = layout.get("screens", [])
        if len(screens) <= 1 or i >= len(screens):
            return
        screens.pop(i)
        if layout.get("primary") == i + 1:
            layout.pop("primary")
        if not self._save(layout):
            return
        self.changed.emit()

    @Slot()
    def toggleScreens(self):
        self._ask_screens("toggle")

    # --- desktopless mode (docs/desktopless.md): every app window its own panel ---
    @Property(bool, notify=changed)
    def desktopless(self):
        s = ft_layout.load_layout().get("desktopless")
        return isinstance(s, dict) and bool(s.get("on"))

    @Slot(bool)
    def setDesktopless(self, on):
        self._run("Desktopless mode " + ("on" if on else "off"), "desktopless", "on" if on else "off")

    # --- the desktop's notifications in the Steam session (session/ft-notifyfwd) ---
    @Property(str, notify=changed)
    def notifyForward(self):
        return self._conf()["notifyForward"]

    @Property(str, notify=changed)
    def notifyForwardState(self):
        return self._notify_state

    @Property(bool, notify=changed)
    def notifyRestartNeeded(self):
        """The session writes ft-notifyfwd's autostart entry when it starts."""
        return self._running and bool(self._started) and self._started["notifyForward"] != self._conf()["notifyForward"]

    @Slot(str)
    def setNotifyForward(self, mode):
        if mode in NOTIFY_FORWARD:
            write_conf_value("NOTIFY_FORWARD", mode)
            self.changed.emit()

    # --- Start with SteamVR (session/autostart.sh, frametop-autostart.service) ---
    @Property(bool, notify=changed)
    def autostart(self):
        return self._conf()["autostart"]

    @Property(bool, notify=changed)
    def autostartInstalled(self):
        """desktops.sh install writes the unit; the home folder is the same in the container."""
        return os.path.exists(AUTOSTART_UNIT)

    @Slot(bool)
    def setAutostart(self, on):
        """The unit reads AUTOSTART each time SteamVR starts: nothing to enable or restart."""
        write_conf_value("AUTOSTART", "1" if on else "0")
        self.changed.emit()

    # --- apps from the SteamVR dashboard's Launch a program list (float/ft-dashroute) ---
    @Property(str, notify=changed)
    def dashboardApps(self):
        return self._conf()["dashboardApps"]

    @Property(int, notify=changed)
    def floatSlots(self):
        return max(0, min(16, self._conf()["floatSlots"]))

    @Slot(str)
    def setDashboardApps(self, mode):
        """ft-dashroute and ft-floatd read it as they go: no restart."""
        if mode in DASHBOARD_APPS:
            write_conf_value("DASHBOARD_APPS", mode)
            self.changed.emit()

    # --- ft-screens: visibility and pinning ---
    @Property("QVariantMap", notify=changed)
    def visibility(self):
        return ft_layout.visibility(ft_layout.load_layout())

    @Slot(str, "QVariant")
    def setVisibility(self, key, value):
        layout = ft_layout.load_layout()
        v = ft_layout.visibility(layout)
        v[key] = value
        layout["visibility"] = v
        if not self._save(layout):
            return
        self.changed.emit()
        if self._running:
            if key == "mode":
                self._ask_screens(f"visibility {value}")
            elif key == "wrist_angle":
                self._ask_screens(f"wrist {float(value):.1f}")
            elif key == "controllers":
                self._ask_screens(f"controllers {value}")
            elif key == "in_games":
                self._ask_screens(f"ingames {value}")
            else:
                self._ask_screens(f"gesture {v['gesture_hand']} {float(v['gesture_angle']):.1f}")

    @Property("QVariantList", notify=changed)
    def pins(self):
        return self._pins

    @Property("QVariantList", notify=changed)
    def screensShown(self):
        """For each screen, whether it shows (False: hidden on its own, ft-layout hide N)."""
        layout = ft_layout.load_layout()
        return [not ft_layout.screen_entry(layout, i).get("hidden") for i in range(ft_layout.screen_count(layout))]

    @Slot(int, bool)
    def setScreenShown(self, index, shown):
        """Hide screen `index` (0-based) on its own, whatever the visibility mode, or show it."""
        layout = ft_layout.load_layout()
        screens = layout.setdefault("screens", [])
        while len(screens) <= index:
            screens.append({})
        if shown:
            screens[index].pop("hidden", None)
        else:
            screens[index]["hidden"] = True
        if not self._save(layout):
            return
        self.changed.emit()
        def applied(reply):
            if not (reply and reply.startswith("ok")):
                self.message.emit("Saved; the desktop applies it when it next starts "
                                  "(its compositor is older than hiding screens one at a time)", False)
        if self._running:
            self._ask_screens(f"{'reveal' if shown else 'conceal'} {index + 1}", then=applied)

    # --- head pin modes (SCREEN_PIN_LEVEL; ft-screens takes it live, "handsetting") ---
    # 0 rides on the headset, 1 follows turning and walking but stays level, 2 (Walk) follows
    # walking only: no turning, looking up or down, or crouching.
    @Property(int, notify=changed)
    def pinMode(self):
        v = ft_layout.read_conf().get("SCREEN_PIN_LEVEL", "0")
        return int(v) if v in ("1", "2") else 0

    @Slot(int)
    def setPinMode(self, mode):
        """How screens on your head follow it. Pins already on the head change over where they
        are now."""
        mode = mode if mode in (1, 2) else 0
        write_conf_value("SCREEN_PIN_LEVEL", str(mode))
        if self._running and ft_layout.backend() == "screens":
            self._ask_screens(f"handsetting SCREEN_PIN_LEVEL {mode}")
        self.changed.emit()

    @Slot(str, str)
    def pin(self, which, where):
        """Pin screen `which` (1-based, or "all") to "left", "right", or "head" as it is
        now, or take it off ("none")."""
        cmd = f"unpin {which}" if where == "none" else f"pin {which} {where}"
        if self._running:
            self._ask_screens(cmd, then=lambda reply: self._pinned(which, where, reply))
        else:
            self._pinned(which, where, None)

    def _pinned(self, which, where, reply):
        if not (reply and reply.startswith("ok")):
            self.message.emit(f"Couldn't {'unpin' if where == 'none' else 'pin'}: "
                              f"{reply or 'the desktop is not running'}", True)
        elif which == "all" and where != "none":
            place = "on your head" if where == "head" else f"on your {where} wrist"
            self.message.emit(f"All screens ride {place} now. Save as profile… (Layout & profiles) keeps it.", False)
        self._check_running()

    # --- stereo wallpaper (DEE-317, stereo_wallpaper.py) ---
    @Property(bool, notify=changed)
    def stereoWallpaper(self):
        return ft_layout.read_conf().get("STEREO_WALLPAPER", "0") == "1"

    @Property(bool, notify=changed)
    def stereoWallpaperReady(self):
        """Both eyes' pictures are there."""
        return stereo_wallpaper.ready()

    @Slot(str, str)
    def chooseStereoWallpaper(self, left, right):
        """Pictures from the file dialog (file:// URLs or paths): a left and a right, or `left`
        alone as one side-by-side picture (right empty). With the stereo wallpaper on, the
        screens show the new ones at once."""
        def local(u):
            return QUrl(u).toLocalFile() if u.startswith("file:") else u
        err = stereo_wallpaper.prepare(local(left), local(right) or None)
        if err:
            self.message.emit(err, True)
            return
        self.changed.emit()
        if self.stereoWallpaper and self._running:
            self._run("New stereo wallpaper", argv=self._wallpaper_argv("reload"))
        else:
            self.message.emit("Stereo wallpaper pictures ready.", False)

    @Slot(bool)
    def setStereoWallpaper(self, on):
        """Plasma's wallpaper becomes the key colour and ft-screens draws the pictures in it, or
        both go back. Needs the desktop running (Plasma is scripted live)."""
        if on and not stereo_wallpaper.ready():
            self.message.emit("Pick the pictures first.", True)
            return
        if not self._running:
            self.message.emit("Start the Frametop desktop first: the wallpaper is set in it.", True)
            return
        self._run("Stereo wallpaper " + ("on" if on else "off"), argv=self._wallpaper_argv("on" if on else "off"))

    @staticmethod
    def _wallpaper_argv(verb):
        return host_command("python3", os.path.abspath(stereo_wallpaper.__file__), verb)

    # --- power: ft-powerd and Steam's sleep setting ---
    def _check_powerd(self):
        self._ipc.run(self._powerd_status, then=self._set_powerd)

    @staticmethod
    def _powerd_status(client):
        """On the worker's thread: (state, seconds unused, timeout seconds), or None."""
        reply = (client.ask(FT_POWERD, "status", timeout=0.5) or "").split()
        try:
            return (reply[1], float(reply[2]), float(reply[3])) if reply[:1] == ["ok"] else None
        except (IndexError, ValueError):
            return None

    def _set_powerd(self, status):
        if status != self._powerd:
            self._powerd = status
            self.powerChanged.emit()

    @Property("QVariantMap", notify=powerChanged)
    def power(self):
        try:
            off_min = float(ft_layout.read_conf().get("DISPLAY_OFF_MIN") or 0)
        except ValueError:
            off_min = 0.0
        state, unused, _ = self._powerd or ("", 0, 0)
        return {"offMinutes": off_min, "service": self._powerd is not None, "state": state, "unused": unused,
                "steam": self._steam is not None, "steamBusy": self._steam_busy, "steamError": self._steam_error,
                "acSleep": self._steam["ac"] if self._steam else -1,
                "batterySleep": self._steam["battery"] if self._steam else -1}

    @Slot(float)
    def setDisplayOffMinutes(self, minutes):
        """ft-powerd re-reads frametop.conf within 2 s."""
        write_conf_value("DISPLAY_OFF_MIN", f"{max(0.0, minutes):g}")
        self.powerChanged.emit()

    @Slot()
    def displaysOffNow(self):
        self._ipc.ask(FT_POWERD, "off", then=self._displays_off, timeout=0.5)

    def _displays_off(self, reply):
        reply = reply or "error the power service isn't running"
        if not reply.startswith("ok"):
            self.message.emit(f"Couldn't turn the displays off: {reply.split(' ', 1)[-1]}", True)
        self._check_powerd()

    def _steam_call(self, what, fn):
        """Runs fn, which talks to Steam (up to a few seconds), off the UI thread, then reads
        Steam's sleep settings; _steam_done gets them on the UI thread."""
        if self._steam_busy:
            return
        self._steam_busy = True
        self.powerChanged.emit()

        def work():
            try:
                fn()
                self._steamDone.emit(steam_settings.sleep_settings(), None, what)
            except (steam_settings.SteamUnreachable, OSError, ValueError) as e:
                self._steamDone.emit(None, str(e), what)

        threading.Thread(target=work, daemon=True).start()

    def _steam_done(self, settings, error, what):
        self._steam_busy = False
        if settings is not None:
            self._steam, self._steam_error = settings, ""
        else:
            self._steam, self._steam_error = None, error
            if what:
                self.message.emit(f"Couldn't change Steam's sleep setting: {error}", True)
        self.powerChanged.emit()

    @Slot()
    def refreshPower(self):
        self._check_powerd()
        self._steam_call("", lambda: None)

    @Slot(bool)
    def setStayAwake(self, on):
        """Steam's "When Plugged In and Idle -> Sleep after" is Never while this is on. The value
        from before is kept in frametop.conf and goes back when it's turned off."""
        def change():
            ac = steam_settings.sleep_settings()["ac"]
            if on:
                if ac > 0:
                    write_conf_value("STEAM_SLEEP_AC_BEFORE", str(ac))
                steam_settings.set_sleep_setting("system_idle_suspend_ac_sec", 0)
            elif ac == 0:
                try:
                    before = int(ft_layout.read_conf().get("STEAM_SLEEP_AC_BEFORE") or STEAM_SLEEP_AC_DEFAULT)
                except ValueError:
                    before = STEAM_SLEEP_AC_DEFAULT
                steam_settings.set_sleep_setting("system_idle_suspend_ac_sec", before if before > 0 else STEAM_SLEEP_AC_DEFAULT)
        self._steam_call("stay awake" if on else "sleep", change)

    @Slot()
    def restartDesktop(self):
        """Restart the Frametop desktop (this app closes with it) to apply count and resolution."""
        argv = host_command("systemd-run", "--user", "--collect", "--quiet", os.path.abspath(DESKTOPS), "restart")
        QProcess.startDetached(argv[0], argv[1:])
        self.message.emit("Restarting the desktop…", False)

    # --- layout ---
    def _save(self, layout):
        """Save the layout; False, said in the window, when it can't be (a frametop-layout.json
        that doesn't read is never saved over)."""
        try:
            ft_layout.save_layout(layout)
            return True
        except (OSError, ft_layout.LayoutFileError) as e:
            self.message.emit(str(e), True)
            return False

    def _edit_layout(self, fn):
        layout = ft_layout.load_layout()
        fn(layout)
        if not self._save(layout):
            return False
        self.changed.emit()
        return True

    @Slot(str)
    def setMode(self, mode):
        def edit(layout):
            layout["mode"] = mode
            layout.pop("active", None)
        self._edit_layout(edit)

    @Property("QVariantList", notify=changed)
    def layoutNames(self):
        return ft_layout.layout_names(ft_layout.load_layout())

    @Slot(str)
    def useLayout(self, name):
        """A named layout as the arrangement (Arrange now puts the screens there)."""
        try:
            self._edit_layout(lambda l: ft_layout.use_named(l, name))
        except RuntimeError as e:
            self.message.emit(str(e), True)

    @Slot(str)
    def saveLayout(self, name):
        try:
            ft_layout.check_name(name)
        except RuntimeError as e:
            return self.message.emit(str(e), True)
        self._run(f"Saving the arrangement as {' '.join(name.split())}", "save", name)

    @Slot(str, str)
    def renameLayout(self, old, new):
        try:
            self._edit_layout(lambda l: ft_layout.rename_named(l, old, new))
            ft_layout.write_launchers(ft_layout.load_layout())
        except (RuntimeError, OSError) as e:
            self.message.emit(str(e), True)

    @Slot(str)
    def deleteLayout(self, name):
        try:
            self._edit_layout(lambda l: ft_layout.delete_named(l, name))
            ft_layout.write_launchers(ft_layout.load_layout())
        except (RuntimeError, OSError) as e:
            self.message.emit(str(e), True)

    # --- profiles (docs/profiles.md): a named layout's apps and hidden screens ---
    @Slot(str, result="QVariantList")
    def profileWindows(self, name):
        """A profile's windows, as "app", "where", and "index" (its place in the saved list, for
        removeProfileWindow) for the list. The retired settings apps aren't listed: opening the
        profile leaves them out (ft_layout.retired_entry)."""
        out = []
        for i, e in enumerate(ft_layout.load_layout().get("profiles", {}).get(name, {}).get("windows", [])):
            if ft_layout.retired_entry(e):
                continue
            app = e.get("app") or os.path.basename((e.get("cmd") or ["?"])[0])
            app = app.rsplit(".", 1)[-1] if "." in app and not e.get("cmd") else app
            where = "floating" if "float" in e else f"screen {e.get('screen', 1)}" + (", maximized" if e.get("maximized") else "")
            out.append({"app": app, "where": where, "index": i})
        return out

    @Slot(str, result="QVariantList")
    def profileRemotes(self, name):
        """The remote displays a profile connects, by name."""
        layout = ft_layout.load_layout()
        saved = layout.get("profiles", {}).get(name, {}).get("remote", {})
        return [ft_layout.remote_label(host, d) + (" (hidden)" if saved[d["id"]].get("hidden") else "")
                for _, host, d in ft_layout.remote_displays(layout) if d.get("id") in saved]

    @Slot(str, result="QVariantList")
    def profileHidden(self, name):
        return ft_layout.load_layout().get("profiles", {}).get(name, {}).get("hidden", [])

    @Slot(str, int)
    def removeProfileWindow(self, name, index):
        def edit(layout):
            windows = layout.get("profiles", {}).get(name, {}).get("windows", [])
            if 0 <= index < len(windows):
                windows.pop(index)
        self._edit_layout(edit)

    @Property(str, notify=changed)
    def defaultProfile(self):
        return ft_layout.load_layout().get("default_profile", "")

    @Slot(str)
    def setDefaultProfile(self, name):
        def edit(layout):
            if name:
                layout["default_profile"] = name
            else:
                layout.pop("default_profile", None)
        self._edit_layout(edit)

    @Slot(str, "QVariant")
    def setPreset(self, key, value):
        def edit(layout):
            layout["mode"] = "preset"
            layout["preset"][key] = value
        self._edit_layout(edit)

    @Slot(bool)
    def setAuto(self, on):
        self._edit_layout(lambda l: l.__setitem__("auto", bool(on)))

    @Slot()
    def arrange(self):
        """Arrange the screens; in a profile, also open its apps (ft-layout use)."""
        layout = ft_layout.load_layout()
        name = layout.get("active")
        if layout.get("mode") == "custom" and name in layout.get("layouts", {}):
            self._run(f"Opening {name}", "use", name)
        else:
            self._run("Arranging the screens", "apply")

    @Slot()
    def capture(self):
        self._run("Saving the current arrangement", "capture")

    # --- remote displays: their own app ---
    # --- ft-layout on the host ---
    def _run(self, label, *args, argv=None):
        """ft-layout with args on the host (or argv instead), one at a time."""
        if self._proc is not None:
            self.message.emit(f"Still busy: {self._busy}", True)
            return
        self._busy = label
        self.busyChanged.emit()
        proc = QProcess(self)
        proc.setProcessChannelMode(QProcess.MergedChannels)
        argv = argv or host_command(os.path.abspath(FT_LAYOUT), *args)
        proc.finished.connect(lambda code, _status: self._done(proc, label, code))
        self._proc = proc
        proc.start(argv[0], argv[1:])

    def _done(self, proc, label, code):
        out = bytes(proc.readAllStandardOutput()).decode(errors="replace").strip()
        self._proc = None
        self._busy = ""
        self.busyChanged.emit()
        self.changed.emit()
        last = out.splitlines()[-1] if out else ""
        if code == 0:
            self.message.emit(f"{label}: done" + (f" ({last})" if last and not last.startswith("ok") else ""), False)
        else:
            self.message.emit(f"{label} failed: {last or 'exit code ' + str(code)}", True)
