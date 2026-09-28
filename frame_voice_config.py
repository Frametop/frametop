#!/usr/bin/env python3
"""Frame Voice settings (GTK 4 / libadwaita): pick push-to-talk buttons by pressing them.

"Add" waits for a controller button (read from vrserver's input stream, like
frame-voice-vr-input) or a keyboard key / mouse button (read from evdev, like frame-voice)
and saves ~/.config/frame-voice/settings.json. Both services pick changes up on their
own. The Service switches start, stop, enable and disable the three systemd user units.
Run through the `frame-voice-config` launcher: GTK comes from the system Python, evdev
and websockets from the project venv.
"""
import asyncio
import json
import os
import secrets
import select
import socket
import subprocess
import sys
import threading
import time
import urllib.request
from pathlib import Path

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
import evdev  # noqa: E402
import websockets  # noqa: E402
from evdev import ecodes as e  # noqa: E402
from gi.repository import Adw, GLib, Gtk  # noqa: E402

import settings  # noqa: E402

# systemd's runtime dir, where the services and their socket live. Not $XDG_RUNTIME_DIR: Frametop's
# desktop points that (and the session bus) at its own, where systemctl --user can't reach them.
USER_RUNTIME = Path(f"/run/user/{os.getuid()}")
SOCKET_PATH = USER_RUNTIME / "frame-voice/control.sock"
VRSERVER = "http://127.0.0.1:27062"
VR_HEADERS = {"Referer": f"{VRSERVER}/dashboard/controllerbinding.html"}
TYPER_NAME = "frame-voice keyboard"
CAPTURE_SECONDS = 15

# Clicking "Add" would otherwise bind the mouse button that clicked it.
IGNORED_KEYS = {e.BTN_LEFT, e.BTN_RIGHT, e.BTN_TOUCH, e.BTN_TOOL_FINGER, e.BTN_TOOL_PEN,
                e.BTN_TOOL_DOUBLETAP, e.BTN_TOOL_TRIPLETAP, e.BTN_STYLUS}
# The system button opens the SteamVR dashboard; ft_pointer is Frametop's virtual pointer (its 3D mouse).
IGNORED_INPUTS = {"/input/system/click"}
IGNORED_CONTROLLERS = {"ft_pointer"}

INPUT_NAMES = {"dpad_up": "D-pad up", "dpad_down": "D-pad down", "dpad_left": "D-pad left",
               "dpad_right": "D-pad right", "thumbstick": "Thumbstick press", "trackpad": "Trackpad press",
               "trigger": "Trigger", "grip": "Grip", "bumper": "Bumper", "thumbrest": "Thumb rest",
               "view": "View", "menu": "Menu", "system": "System"}
KEY_NAMES = {"CTRL": "Ctrl", "ALT": "Alt", "SHIFT": "Shift", "META": "Super", "SIDE": "side button",
             "EXTRA": "extra button", "FORWARD": "forward button", "BACK": "back button",
             "MIDDLE": "middle button", "CAPSLOCK": "Caps Lock", "SCROLLLOCK": "Scroll Lock",
             "PAUSE": "Pause", "ESC": "Esc", "SPACE": "Space", "ENTER": "Enter"}


def key_label(name):
    base = name.split("_", 1)[1] if "_" in name else name
    if name.startswith("BTN_"):
        return "Mouse " + KEY_NAMES.get(base, base.lower().replace("_", " "))
    for side in ("LEFT", "RIGHT"):
        rest = base[len(side):]
        if base.startswith(side) and rest in KEY_NAMES:
            return f"{side.title()} {KEY_NAMES[rest]}"
    return KEY_NAMES.get(base, base.title() if len(base) > 1 else base)


def controller_label(device, input_path):
    hand = device.rsplit("/", 1)[-1]
    part = input_path.split("/")[2] if input_path.count("/") >= 2 else input_path
    name = INPUT_NAMES.get(part, part.upper() if len(part) == 1 else part.replace("_", " ").capitalize())
    return f"{hand.title() if hand in ('left', 'right') else device} controller: {name}"


MODE_LABELS = {"toggle": "Tap to start, tap to stop", "hold": "Hold to talk"}
MODE_HINTS = {"toggle": "Tap to start, tap again to stop.", "hold": "Hold to talk, release to stop."}


def key_note(name, mode):
    if name in settings.MODIFIERS:
        if mode == "hold":
            return "While it's held, text waits for release (it would turn letters into shortcuts)"
        return "Shortcuts with it (like Ctrl+C) don't start or stop dictation"
    if name.startswith("KEY_") and (len(name) == 5 or name in ("KEY_SPACE", "KEY_ENTER")):
        return "This key also types into apps"
    return name


def device_label(root_path, controller_type):
    name = controller_type.replace("_", " ").title().replace("Frame", "Steam Frame")
    hand = root_path.rsplit("/", 1)[-1]
    # A controller without a hand role (Frametop's pointer holds one) is known by its serial.
    return f"{hand.title()} {name}" if hand in ("left", "right") else f"{name} ({hand})"


def vr_devices():
    """Connected controllers as SteamVR reports them, or None when SteamVR is not running."""
    req = urllib.request.Request(f"{VRSERVER}/input/getstate.json", headers=VR_HEADERS)
    try:
        with urllib.request.urlopen(req, timeout=3) as resp:
            state = json.load(resp)
    except OSError:
        return None
    return [{"device": d["root_path"], "controller_type": d["controller_type"],
             "label": device_label(d["root_path"], d["controller_type"])}
            for d in state.get("devices", [])
            if d.get("class") == "TrackedDeviceClass_Controller" and d.get("root_path")
            and d.get("controller_type") not in IGNORED_CONTROLLERS]


UNIT_DIR = Path(__file__).resolve().parent / "systemd"
UNITS = ("frame-voice-whisper.service", "frame-voice.service", "frame-voice-vr-input.service")


def systemctl(*args):
    env = {**os.environ, "XDG_RUNTIME_DIR": str(USER_RUNTIME), "DBUS_SESSION_BUS_ADDRESS": f"unix:path={USER_RUNTIME}/bus"}
    try:
        return subprocess.run(["systemctl", "--user", *args], capture_output=True, text=True, timeout=90, env=env)
    except (OSError, subprocess.TimeoutExpired) as err:
        return subprocess.CompletedProcess(args, 1, "", str(err))


def service_state():
    """unit -> {"LoadState", "ActiveState", "UnitFileState"}, in UNITS order."""
    out = systemctl("show", "-p", "Id,LoadState,ActiveState,UnitFileState", *UNITS).stdout
    blocks = [dict(line.split("=", 1) for line in block.splitlines() if "=" in line) for block in out.split("\n\n")]
    return {unit: next((b for b in blocks if b.get("Id") == unit), {}) for unit in UNITS}


def service_action(action):
    """systemctl start, stop, enable or disable all three units. Returns an error or None.

    SteamVR only sees the typing keyboard if it existed when SteamVR started. It lives in
    frame-voice's fd store, which systemd keeps across a stop only while the unit is enabled,
    and drops when the unit file goes missing, even while running. The units are linked from
    the repo and `systemctl disable` removes those links too, so they are linked again before
    systemd reloads."""
    steps = [(action, *UNITS)]
    if action in ("start", "enable"):  # links missing (never installed, or removed by hand)
        state = service_state()
        missing = [str(UNIT_DIR / u) for u in UNITS if state[u].get("LoadState") != "loaded"]
        steps[:0] = [("link", *missing)] if missing else []
    elif action == "disable":
        steps = [("disable", "--no-reload", *UNITS), ("link", "--no-reload", *(str(UNIT_DIR / u) for u in UNITS)),
                 ("daemon-reload",)]
    for args in steps:
        result = systemctl(*args)
        if result.returncode:
            return (result.stderr.strip() or f"systemctl {args[0]} failed").splitlines()[0]
    return None


def notify_daemon():
    try:
        with socket.socket(socket.AF_UNIX) as sock:
            sock.settimeout(2)
            sock.connect(str(SOCKET_PATH))
            sock.sendall(b"reload\n")
            sock.recv(4096)
        return True
    except OSError:
        return False


class Capture:
    """One capture at a time; a new request or /cancel ends the previous one."""

    def __init__(self):
        self.lock = threading.Lock()
        self.cancel = threading.Event()

    def begin(self):
        self.cancel.set()
        self.lock.acquire()
        self.cancel = threading.Event()
        return self.cancel

    def end(self):
        self.lock.release()

    def keyboard(self):
        cancel = self.begin()
        devices = []
        try:
            for path in evdev.list_devices():
                try:
                    dev = evdev.InputDevice(path)
                except OSError:
                    continue
                if dev.name == TYPER_NAME or e.EV_KEY not in dev.capabilities():
                    dev.close()
                    continue
                devices.append(dev)
            for dev in devices:  # drop anything already queued
                try:
                    while dev.read_one():
                        pass
                except OSError:
                    pass
            deadline = time.monotonic() + CAPTURE_SECONDS
            while time.monotonic() < deadline and not cancel.is_set():
                ready, _, _ = select.select(devices, [], [], 0.2)
                for dev in ready:
                    try:
                        events = list(dev.read())
                    except OSError:
                        continue
                    for ev in events:
                        if ev.type != e.EV_KEY or ev.value != 1 or ev.code in IGNORED_KEYS:
                            continue
                        if ev.code == e.KEY_ESC:
                            return {"cancelled": True}
                        name = e.KEY.get(ev.code) or e.BTN.get(ev.code)
                        if isinstance(name, list):
                            name = name[0]
                        if name:
                            return {"code": name, "label": key_label(name), "source": dev.name}
            return {"cancelled": True} if cancel.is_set() else {"timeout": True}
        finally:
            for dev in devices:
                dev.close()
            self.end()

    def controller(self):
        cancel = self.begin()
        try:
            devices = vr_devices()
            if devices is None:
                return {"error": "SteamVR is not running."}
            if not devices:
                return {"error": "No controller is connected. Wake a controller and try again."}
            return asyncio.run(self._controller(cancel, devices))
        except (OSError, websockets.WebSocketException) as err:
            return {"error": f"Could not read controllers from SteamVR: {err}"}
        finally:
            self.end()

    async def _controller(self, cancel, devices):
        mailbox = f"frame_voice_config_{os.getpid()}_{secrets.token_hex(3)}"
        types = {d["device"]: d["controller_type"] for d in devices}
        async with websockets.connect(VRSERVER.replace("http", "ws"), origin=VRSERVER, max_size=None,
                                      additional_headers=VR_HEADERS) as ws:
            await ws.send(f"mailbox_open {mailbox}")
            for device in types:
                message = {"type": "request_input_state_updates", "device_path": device, "returnAddress": mailbox}
                await ws.send(f"mailbox_send input_server {json.dumps(message)}")
            released = {}  # device -> inputs seen up; a button held when capture starts doesn't count
            deadline = time.monotonic() + CAPTURE_SECONDS
            while time.monotonic() < deadline and not cancel.is_set():
                try:
                    message = json.loads(await asyncio.wait_for(ws.recv(), 0.2))
                except asyncio.TimeoutError:
                    continue
                device = message.get("device")
                if message.get("type") != "update_component_states" or device not in types:
                    continue
                seen = released.setdefault(device, set())
                for name, value in message.get("components", {}).items():
                    if not name.endswith("/click") or name in IGNORED_INPUTS:
                        continue
                    if not value:
                        seen.add(name)
                    elif name in seen:
                        return {"device": device, "input": name, "controller_type": types[device],
                                "label": controller_label(device, name)}
            return {"cancelled": True} if cancel.is_set() else {"timeout": True}


class Window(Adw.ApplicationWindow):
    def __init__(self, app):
        super().__init__(application=app, title="Frame Voice", default_width=560, default_height=640)
        self.capture = Capture()
        self.data = settings.load()
        self.rows = []
        self.dialog = None

        self.service_busy = False  # a start/stop/enable/disable is running
        self.polling = False
        self.syncing = False  # switches being set from the service state, not by the user

        self.controller_status = "Checking for controllers…"
        self.controller_group = Adw.PreferencesGroup(title="Controller")
        self.keyboard_group = Adw.PreferencesGroup(title="Keyboard and mouse")
        for group, kind in ((self.controller_group, "controller"), (self.keyboard_group, "keyboard")):
            add = Gtk.Button(label="Add", css_classes=["flat"])
            add.connect("clicked", lambda _b, k=kind: self.start_capture(k))
            group.set_header_suffix(add)
        page = Adw.PreferencesPage()
        self.wake_group = self._wake_group()
        for group in (self._service_group(), self.controller_group, self.keyboard_group, self.wake_group,
                      self._behavior_group()):
            page.add(group)
        self.toasts = Adw.ToastOverlay(child=page)
        view = Adw.ToolbarView(content=self.toasts)
        view.add_top_bar(Adw.HeaderBar())
        self.set_content(view)
        self.refresh()
        threading.Thread(target=self._load_controllers, daemon=True).start()
        self._poll_service()
        GLib.timeout_add_seconds(2, self._poll_service)

    def _service_group(self):
        group = Adw.PreferencesGroup(title="Service")
        self.running_row = Adw.SwitchRow(title="Running", subtitle="Checking…")
        self.running_row.connect("notify::active", lambda r, _p: self.syncing or self.run_service(
            "start" if r.get_active() else "stop"))
        self.startup_row = Adw.SwitchRow(title="Start at system startup")
        self.startup_row.connect("notify::active", lambda r, _p: self.syncing or self.run_service(
            "enable" if r.get_active() else "disable"))
        group.add(self.running_row)
        group.add(self.startup_row)
        return group

    def _poll_service(self):
        if not self.polling and not self.service_busy:
            self.polling = True

            def work():
                state = service_state()
                GLib.idle_add(self._polled, state)
            threading.Thread(target=work, daemon=True).start()
        return True  # keep polling: systemctl or a reboot can change it too

    def _polled(self, state):
        self.polling = False
        self.show_service(state)

    def show_service(self, state):
        if self.service_busy:
            return
        active = {u: state[u].get("ActiveState", "unknown") for u in UNITS}
        on = active["frame-voice.service"] in ("active", "activating", "reloading")
        failed = [u for u in UNITS if active[u] == "failed"]
        stopped = [u for u in UNITS if active[u] not in ("active", "activating", "reloading")]
        if failed:
            subtitle = f"{failed[0].removesuffix('.service')} failed. Log: journalctl --user -u {failed[0]}"
        elif on and stopped:
            subtitle = "Partly running: " + ", ".join(u.removesuffix(".service") for u in stopped) + " stopped"
        elif on:
            subtitle = "Dictation, wake words and controller push-to-talk are on"
        else:
            subtitle = "Stopped"
        enabled = state["frame-voice.service"].get("UnitFileState") == "enabled"
        self.syncing = True
        self.running_row.set_active(on)
        self.startup_row.set_active(enabled)
        self.syncing = False
        self.running_row.set_subtitle(subtitle)
        self.startup_row.set_subtitle(
            "Starts before SteamVR, so typing reaches game-mode windows" if enabled else
            "Off: restart SteamVR after starting Frame Voice to type into game-mode windows")

    def run_service(self, action):
        self.service_busy = True
        row = self.running_row if action in ("start", "stop") else self.startup_row
        row.set_subtitle({"start": "Starting…", "stop": "Stopping…", "enable": "Enabling…", "disable": "Disabling…"}[action])
        self.running_row.set_sensitive(False)
        self.startup_row.set_sensitive(False)

        def work():
            error = service_action(action)
            GLib.idle_add(self._service_done, action, error, service_state())
        threading.Thread(target=work, daemon=True).start()

    def _service_done(self, action, error, state):
        self.service_busy = False
        self.running_row.set_sensitive(True)
        self.startup_row.set_sensitive(True)
        self.show_service(state)
        done = {"start": "Frame Voice started.", "stop": "Frame Voice stopped.",
                "enable": "Frame Voice will start at system startup.",
                "disable": "Frame Voice won't start at system startup."}
        self.toast(f"Couldn't {action} Frame Voice: {error}" if error else done[action])

    def _wake_group(self):
        group = Adw.PreferencesGroup(
            title="Wake words",
            description="Say one, then a command; the command goes to hooks/on-command and the event "
                        "stream. Any English words work. Two or more words trigger by accident less.")
        switch = Adw.SwitchRow(title="Listen for wake words", subtitle="While not dictating",
                               active=self.data["wake_word"])
        switch.connect("notify::active", lambda r, _p: self.set_option("wake_word", r.get_active()))
        group.add(switch)
        self.wake_entry = Adw.EntryRow(title="Add a wake word, like \u201cHey Computer\u201d", show_apply_button=True)
        self.wake_entry.connect("apply", self.add_wake_phrase)
        return group

    def add_wake_phrase(self, entry):
        phrase = settings.clean_phrase(entry.get_text())
        entry.set_text("")
        if not phrase:
            self.toast("Use letters and spaces.")
        elif phrase in self.data["wake_phrases"]:
            self.toast(f"\u201c{phrase.title()}\u201d is already a wake word.")
        else:
            self.data["wake_phrases"].append(phrase)
            self.save(f"Added \u201c{phrase.title()}\u201d.")

    def _behavior_group(self):
        group = Adw.PreferencesGroup(title="Behavior",
                                     description="Recordings shorter than 2 seconds are discarded.")
        modes = list(MODE_LABELS)
        for key, title in (("keyboard_mode", "Keyboard and mouse"), ("controller_mode", "Controller")):
            row = Adw.ComboRow(title=title, model=Gtk.StringList.new([MODE_LABELS[m] for m in modes]),
                               selected=modes.index(self.data[key]))
            row.connect("notify::selected", lambda r, _p, k=key: self.set_option(k, modes[r.get_selected()]))
            group.add(row)
        for key, title, subtitle in (
                ("live_typing", "Type while talking", "Text appears every few seconds instead of after you stop"),
                ("sounds", "Sounds", "Tones when recording starts and stops")):
            row = Adw.SwitchRow(title=title, subtitle=subtitle, active=self.data[key])
            row.connect("notify::active", lambda r, _p, k=key: self.set_option(k, r.get_active()))
            group.add(row)
        return group

    def set_option(self, key, value):
        self.data[key] = value
        self.save("Saved.")

    def _load_controllers(self):
        devices = vr_devices()
        if devices is None:
            text = "SteamVR is not running."
        elif not devices:
            text = "No controller connected. Wake one to add its buttons."
        else:
            text = "Connected: " + ", ".join(d["label"] for d in devices) + "."
        self.controller_status = text
        GLib.idle_add(self.refresh)

    def refresh(self):
        for group, row in self.rows:
            group.remove(row)
        self.rows = []
        entries = [(self.controller_group, controller_label(c["device"], c["input"]), f"{c['device']} {c['input']}", ("controller", i))
                   for i, c in enumerate(self.data["controller"])]
        self.controller_group.set_description(MODE_HINTS[self.data["controller_mode"]] + " " + self.controller_status)
        self.keyboard_group.set_description(MODE_HINTS[self.data["keyboard_mode"]])
        entries += [(self.keyboard_group, key_label(k), key_note(k, self.data["keyboard_mode"]), ("keyboard", i))
                    for i, k in enumerate(self.data["keyboard"])]
        entries += [(self.wake_group, p.title(), "", ("wake_phrases", i)) for i, p in enumerate(self.data["wake_phrases"])]
        for group, title, subtitle, (kind, index) in entries:
            row = Adw.ActionRow(title=title)
            if subtitle:
                row.set_subtitle(subtitle)
            remove = Gtk.Button(icon_name="user-trash-symbolic", valign=Gtk.Align.CENTER,
                                css_classes=["flat"], tooltip_text="Remove")
            remove.connect("clicked", lambda _b, k=kind, i=index: self.remove(k, i))
            row.add_suffix(remove)
            group.add(row)
            self.rows.append((group, row))
        for group, kind in ((self.controller_group, "controller"), (self.keyboard_group, "keyboard")):
            if not self.data[kind]:
                row = Adw.ActionRow(title="Nothing bound", subtitle="Use Add to pick a button", sensitive=False)
                group.add(row)
                self.rows.append((group, row))
        self.wake_group.add(self.wake_entry)  # keep the entry below the phrases
        self.rows.append((self.wake_group, self.wake_entry))

    def save(self, message):
        settings.save(self.data)
        live = notify_daemon()
        self.refresh()
        self.toast(message + (" Active now." if live else " frame-voice is not running."))

    def toast(self, text):
        self.toasts.add_toast(Adw.Toast(title=text, timeout=3))

    def remove(self, kind, index):
        del self.data[kind][index]
        self.save("Removed.")

    def start_capture(self, kind):
        what = "controller button" if kind == "controller" else "key or mouse button"
        self.dialog = Adw.AlertDialog(heading=f"Press a {what}",
                                      body=f"Press the {what} you want to use for dictation.")
        self.dialog.set_extra_child(Gtk.Spinner(spinning=True, height_request=32))
        self.dialog.add_response("cancel", "Cancel")
        self.dialog.connect("response", lambda *_: self.capture.cancel.set())
        self.dialog.present(self)

        def work():
            result = self.capture.controller() if kind == "controller" else self.capture.keyboard()
            GLib.idle_add(self.capture_done, kind, result)
        threading.Thread(target=work, daemon=True).start()

    def capture_done(self, kind, result):
        if self.dialog:
            self.dialog.force_close()
            self.dialog = None
        if "error" in result:
            self.toast(result["error"])
        elif "timeout" in result:
            self.toast("Nothing pressed.")
        elif kind == "controller" and "input" in result:
            entry = {"device": result["device"], "input": result["input"]}
            if entry in self.data["controller"]:
                self.toast(f"{result['label']} is already bound.")
            else:
                self.data["controller"].append(entry)
                self.save(f"Added {result['label']}.")
        elif kind == "keyboard" and "code" in result:
            if result["code"] in self.data["keyboard"]:
                self.toast(f"{result['label']} is already bound.")
            else:
                self.data["keyboard"].append(result["code"])
                self.save(f"Added {result['label']}.")
        return False


def main():
    app = Adw.Application(application_id="local.framevoice.Config")
    app.connect("activate", lambda a: (a.get_active_window() or Window(a)).present())
    return app.run(sys.argv)


if __name__ == "__main__":
    sys.exit(main())
