#!/usr/bin/env python3
"""Frametop Remote Displays: other computers' monitors as Frametop screens, streamed with
Moonlight's protocol from Vibepollo or Sunshine, or with macOS High Performance Screen Sharing
from a Mac (docs/remote-displays.md).

A Kirigami (QML) app with a Python backend, like Frametop Settings. It runs in
the dev container:
  - Hosts: computers running Vibepollo, Sunshine or macOS Screen Sharing, found on the network
    (they announce _nvstream._tcp or _rfb._tcp over mDNS; avahi-browse on the host) or typed in.
    For Vibepollo, you sign in once with the host's Web UI user name and password: Frametop asks
    it for an API token that can do only what it needs (conf/ft_hostsignin.py), keeps that in
    ~/.local/share/frametop-stream/hosts/ADDRESS.token, readable only by you, and forgets the
    password. The Web UI's certificate is self-signed, so the one seen then is pinned
    (ADDRESS.pin, its public key's SHA-256): ft-stream and later sign-ins refuse another.
    For macOS Screen Sharing (TCP/UDP 5900, kind "apple"), ft-appless verifies your Mac login
    over Apple's SRP-6a / RSA handshake and keeps it in ADDRESS.apple (0600) for reconnecting.
    Plain Sunshine (host/macos runs one instance per display, each on its own port) has no
    tokens: the login is kept in memory only until the displays picked right after signing in
    are paired with it (ft-layout remote add --login), then forgotten. Adding more later signs
    in again. A host added with "Pair with a PIN" (issue 107: any other Moonlight host) has no
    sign-in at all: each display pairs with a PIN shown here and typed in on the host
    (ft-layout remote add --pin; pairingPin), and streams the host's app by name.
  - App windows, per host: the switch signs in to the Vibepollo on it that shares its apps'
    windows (a Windows PC's own, a Linux PC's second one in its Hyprland session), keeps that
    one's token (ADDRESS-PORT.token) and sets the host's "windows" port in the layout, which is
    what frunner and ft-layout window go by (docs/remote-displays.md, "PC app windows").
  - Displays: a host's existing monitor, or a virtual one at any size, each its own stream
    (ft-layout remote add pairs a client for it). Each one connects and disconnects on its
    own, or all of a host's at once; a disconnected one keeps its place and settings.
    Its stream's resolution, frame rate and bitrate, its width in VR, and whether it shows.
  - Its monitors, per Vibepollo host (Frametop's build): with "Turn off and set its monitors"
    on, while one of its display panels streams, the PC turns off each monitor with no
    connected panel here and puts the others in the mode picked for their panel
    (host_monitors.py, layout/ft_hostmonitors.py). Those panels then pick a mode instead of a
    stream size.
  - Windows, per KDE Plasma host running utrack's KWin Window Stream (issue 81): Add windows
    lists its open windows, and each one picked is a panel of its own that the window sizes,
    connecting with one of the host's displays' pairings (kwin_windows.py; ft-layout remote
    kwin-add). It's listed with the displays, without their stream size.
  - Connection, per host with a Steam Link dongle on the Frame's hotspot (found with it on
    the network, or with Find): auto (the dongle when it answers, else the network), network
    only, or dongle only. Find looks for the host among the hotspot's clients (the same
    <uniqueid> in their serverinfo as at its own address). Hosts without one use the network.
Where the displays are in VR is kept like the screens' (move them there; Display Settings'
Save as profile keeps it). Anything that touches the streams runs layout/ft-layout on the
host; the streams' state comes from ft-screens (@ft_screens: "remotes", "remote N info").
Frametop Settings (settings/ft_settings.py) makes it, the first time its page opens.
"""
import concurrent.futures
import html
import json
import os
import re
import socket
import subprocess
import sys
import threading
import time
import urllib.request
import uuid

from PySide6.QtCore import Property, QObject, QProcess, Qt, QTimer, Signal, Slot

HERE = os.path.dirname(os.path.abspath(__file__))
LAYOUT_DIR = os.path.join(HERE, "..", "layout")
sys.path[:0] = [LAYOUT_DIR, os.path.join(HERE, "..", "conf")]
import ft_layout  # noqa: E402
import host_input  # noqa: E402  (a host's "input": its keyboard, its mouse or both)
import ft_hostapi  # noqa: E402  (where a host's token is kept; conf/)
import ft_hostmonitors  # noqa: E402  (a host's monitors following its panels; layout/)
import ft_hostsignin  # noqa: E402  (the sign-ins that get one)
import ftipc  # noqa: E402  (questions to the datagram sockets)
from fthost import host_command  # noqa: E402  (commands on the SteamOS host)
from kwin_windows import KwinWindows  # noqa: E402  (a KDE PC's windows: Add windows)
from host_monitors import HostMonitors  # noqa: E402  (a PC's monitors: the switch, the modes)

FT_LAYOUT = os.path.join(LAYOUT_DIR, "ft-layout")
FT_SCREENS = "\0ft_screens"
# A Mac's Sunshine instances, one per display (host/macos): HTTP ports 47989 + 100*N.
SUNSHINE_PORTS = [47989 + 100 * k for k in range(8)]
STREAM_RATES = [30, 60, 72, 90, 120]
# A host paired by PIN: its "Desktop" app (Sunshine's and Moonlight hosts' usual name for the
# whole desktop), at 1920x1080 to start with; the app's name can be changed before adding it.
PIN_DESKTOP = {"app": "Desktop", "port": 0, "key": "Desktop", "name": "Desktop", "width": 1920, "height": 1080,
               "primary": True, "active": True, "virtual": False}
# A host's name: a Mac's is often "Name's MacBook" (straight or curly apostrophe). It goes to
# ft-layout as an argument and into the layout and panel labels, never through a shell.
NAME_RE = r"[A-Za-z0-9][A-Za-z0-9 ._'’-]{0,39}"
RESOLUTIONS = ft_layout.SCREEN_RESOLUTIONS


def parse_avahi(text):
    """avahi-browse -rpt _nvstream._tcp / _rfb._tcp -> [{name, address, dongle}]: a host's
    address on the network, and on the Frame's hotspot (wlanap: its Steam Link dongle)."""
    unescape = lambda v: re.sub(r"\\(\d{3})", lambda m: chr(int(m.group(1))), v).replace("\\.", ".")
    found = {}
    for line in text.splitlines():
        f = line.split(";")
        if len(f) < 9 or f[0] != "=" or f[2] != "IPv4" or f[1] == "lo" or f[1].startswith("tailscale"):
            continue
        # A Mac's Sunshine instances all announce its name; mDNS numbers the ones after the
        # first ("Mac (2)"). They're one computer.
        name = re.sub(r" \(\d+\)$", "", unescape(f[3]))
        h = found.setdefault(name, {"name": name, "address": "", "dongle": ""})
        key = "dongle" if f[1] == "wlanap" else "address"
        h[key] = h[key] or f[7]
    out = []
    for h in found.values():
        if not h["address"]:  # only on the hotspot
            h["address"], h["dongle"] = h["dongle"], ""
        out.append(h)
    return sorted(out, key=lambda h: h["name"].lower())


class Backend(QObject):
    changed = Signal()
    busyChanged = Signal()
    message = Signal(str, bool)  # text, is error
    monitorsReady = Signal(str, "QVariantList", str)  # a host's address, its monitors, an error
    dongleFound = Signal(str, str, str)  # a host's name, its dongle's address ("" none), what happened
    hostsFound = Signal("QVariantList")  # computers on the network: [{name, address, dongle, added}]
    signedIn = Signal(str, bool, str)  # a host's name, whether it worked, what went wrong
    pairingPin = Signal(str, str)  # what's being paired (ft-layout's run label), the PIN to type in on the host
    _signInDone = Signal(str, str, str, str, str, str, str)  # name, address, dongle, token, pin, kind, error
    appWindowsSet = Signal(str, bool, str)  # a host's name, whether it worked, what went wrong
    _appWindowsDone = Signal(str, str, int, str, str, str)  # name, address, port, token, pin, error
    _reached = Signal(str, bool)  # a host's address, whether its Web UI answered
    _ipcDone = Signal(object, object)  # a callback and its answer, from the ftipc worker

    def __init__(self):
        super().__init__()
        # ft-screens is asked on a thread of its own, never the UI's
        self._ipcDone.connect(lambda then, answer: then(answer), Qt.QueuedConnection)
        self._ipc = ftipc.Worker(self._ipcDone.emit)
        self._polling = False
        self._running = False
        self._streams_only = False  # ft-screens runs without the desktop (ft-layout streams)
        self._states = {}   # remote screen number -> its stream's state ("lost can't connect")
        self._online = {}   # host address -> True/False (None: not checked yet)
        self._queue = []    # ft-layout runs waiting their turn: (label, args, stdin, then)
        self._proc = None
        self._out = []      # what the running ft-layout said so far (a PIN to show comes early)
        self._pin_said = False
        self._busy = ""
        self._logins = {}   # Sunshine host address -> "USER\nPASSWORD\n", until its displays are paired
        self._reached.connect(self._host_reached, Qt.QueuedConnection)
        self._signInDone.connect(self._sign_in_done, Qt.QueuedConnection)
        self._appWindowsDone.connect(self._app_windows_done, Qt.QueuedConnection)
        self._kwin = KwinWindows(lambda *args: self._run(*args), self)  # in this queue, as it is then
        self._monitors = HostMonitors(lambda *args, then=None: self._run(*args, then=then), self)
        self.poll = QTimer(interval=2000, timeout=self._check)
        self.poll.start()
        self.ping = QTimer(interval=15000, timeout=self._ping_hosts)
        self.ping.start()
        self._check()
        self._ping_hosts()

    # --- state ---
    def _ask(self, text, then=None):
        """A question to ft-screens, on the worker; then(answer, or None if it isn't running)."""
        self._ipc.ask(FT_SCREENS, text, then)

    def _check(self):
        """Reads the streams' states on the worker; _checked gets them here (one read at a time)."""
        if not self._polling:
            self._polling = True
            self._ipc.run(self._read_states, then=self._checked)

    @staticmethod
    def _read_states(client):
        """On the worker's thread: (whether ft-screens runs, whether it runs streams only,
        {remote screen: state}). ft-screens shows the remote displays in the desktop and
        without it, so it's what's asked; one that's slow to answer still runs."""
        reply = client.ask(FT_SCREENS, "remotes")
        running = reply is not None or client.why != ftipc.NOBODY
        states = {}
        if reply and reply.startswith("ok"):
            for e in reply.split()[2:]:
                n, state = int(e.split(":")[0]), e.split(":")[2]
                if state in ("lost", "live"):  # why ("lost can't connect"), or which way ("live via dongle")
                    info = client.ask(FT_SCREENS, f"remote {n} info") or ""
                    state = info[3:].strip() if info.startswith("ok ") else state
                states[n] = state
        return running, running and ft_layout.streams_only(), states

    def _checked(self, result):
        self._polling = False
        if result is None:
            return
        running, streams_only, states = result
        if (running, streams_only, states) != (self._running, self._streams_only, self._states):
            self._running, self._streams_only, self._states = running, streams_only, states
            self.changed.emit()

    def _ping_hosts(self):
        for h in ft_layout.load_layout().get("hosts", []):
            if h.get("address"):
                threading.Thread(target=self._ping, args=(h,), daemon=True).start()

    def _ping(self, host):
        self._reached.emit(host["address"], ft_layout.host_answers(host, timeout=2))

    def _host_reached(self, address, ok):
        if self._online.get(address) != ok:
            self._online[address] = ok
            self.changed.emit()

    @Property(bool, notify=changed)
    def desktopRunning(self):
        """Whether the remote displays can stream now: the desktop runs, or streams only."""
        return self._running

    @Property(bool, notify=changed)
    def streamsOnly(self):
        return self._streams_only

    @Slot()
    def startStreams(self):
        """The connected displays without the desktop (ft-layout streams start)."""
        self._run("Streaming without the desktop", "streams", "start")

    @Slot()
    def stopStreams(self):
        self._run("Stopping the streams", "streams", "stop")

    @Property(QObject, constant=True)
    def kwinWindows(self):
        """A KDE PC's open windows, and adding one as a panel (kwin_windows.py)."""
        return self._kwin

    @Property(QObject, constant=True)
    def hostMonitors(self):
        """A PC's own monitors: the switch, and its monitors' modes for the pickers (host_monitors.py)."""
        return self._monitors

    @Property(str, notify=busyChanged)
    def busy(self):
        return self._busy

    @Property("QVariantList", constant=True)
    def streamRates(self):
        return STREAM_RATES

    @Property("QVariantList", constant=True)
    def resolutions(self):
        return [{"text": f"{w} × {h}" + (f"  ({t})" if t else ""), "width": w, "height": h} for w, h, t in RESOLUTIONS]

    @Property("QVariantList", notify=changed)
    def hosts(self):
        """Each host, whether it answers, and its displays with their streams' state."""
        layout = ft_layout.load_layout()
        numbers = {d["id"]: n for n, _, d in ft_layout.remote_displays(layout)}
        out = []
        for h in layout.get("hosts", []):
            displays = []
            for d in h.get("displays", []):
                if d.get("window"):
                    # A window panel isn't one of the PC's monitors: the launcher and the panel
                    # itself run it, and this page's controls would fight it (turned off, it's
                    # forgotten; its size is the window's).
                    continue
                n = numbers.get(d.get("id"), 0)
                w, hh = d.get("size", [2560, 1440])
                if d.get("off"):
                    state = "disconnected"
                elif not self._running:
                    state = "desktop off"
                else:
                    state = self._states.get(n, "not running")
                raw_routes = d.get("routes") if isinstance(d.get("routes"), dict) else {}

                def route_val(route_name, key, default, _routes=raw_routes):
                    r = _routes.get(route_name)
                    v = r.get(key) if isinstance(r, dict) else None
                    return int(v) if isinstance(v, int) and not isinstance(v, bool) else default

                displays.append({"id": d["id"], "number": n, "label": d.get("label") or d["id"], "app": d["app"],
                                 # Its monitor follows it (the host's switch) in the mode picked here.
                                 "host": h.get("name", ""),
                                 "followsMonitor": ft_hostmonitors.controls(h) and ft_hostmonitors.monitor_of(d) is not None,
                                 "mode": ft_hostmonitors.mode_of(d) or {},
                                 "scale": ft_hostmonitors.scale_of(d) or 0,
                                 # A KDE PC's window (issue 81): the window sets its size.
                                 "kwin": str(d["app"]).startswith("kwin:"),
                                 "key": f"{d['app']}@{d['port']}" if d.get("port") else d["app"],
                                 "virtual": d["app"] == "monitor" or str(d["app"]).startswith("apple:virtual"),
                                 "width": int(w), "height": int(hh),
                                 "fps": int(d.get("fps", 60)), "bitrate": int(d.get("bitrate", 0)),
                                 "directFps": route_val("direct", "fps", 0),
                                 "directBitrate": route_val("direct", "bitrate", -1),
                                 "networkFps": route_val("network", "fps", 0),
                                 "networkBitrate": route_val("network", "bitrate", -1),
                                 "hasRoutes": any(isinstance(raw_routes.get(r), dict) and raw_routes[r]
                                                  for r in ft_layout.ROUTE_NAMES),
                                 "metres": ft_layout.remote_size(d)[0], "shown": not d.get("hidden"),
                                 "connected": not d.get("off"), "state": state})
            address = h.get("address", "")
            kind = h.get("kind", "vibepollo")
            # A host paired by PIN needs no sign-in: it's always ready for another display.
            has_token = (True if kind == "pin" else address in self._logins if kind == "sunshine"
                         else os.path.exists(ft_hostapi.apple_path(address)) if kind == "apple"
                         else os.path.exists(ft_hostapi.token_path(address)))
            out.append({"name": h.get("name", ""), "address": address, "kind": kind,
                        "hasToken": has_token,
                        "online": self._online.get(address), "route": h.get("route", "auto"),
                        "direct": ", ".join(h.get("direct", [])), "mic": bool(h.get("mic")),
                        "inputKeyboard": "keyboard" in host_input.lent(h),
                        "inputMouse": "mouse" in host_input.lent(h), "clipboard": bool(h.get("clipboard")),
                        "appWindows": isinstance(h.get("windows"), dict),
                        "monitors": ft_hostmonitors.controls(h),
                        "kwinWindows": KwinWindows.available(h),
                        "displays": displays})
        return out

    # --- hosts ---
    @Slot()
    def discoverHosts(self):
        """Vibepollo, Sunshine and macOS Screen Sharing computers on the network (answer: hostsFound)."""
        def work():
            lines = []
            for svc in ("_nvstream._tcp", "_rfb._tcp"):
                try:
                    r = subprocess.run(host_command("avahi-browse", "-rpt", svc), capture_output=True,
                                       text=True, timeout=10)
                    lines.append(r.stdout)
                except (OSError, subprocess.SubprocessError):
                    pass
            found = parse_avahi("\n".join(lines))
            known = {h.get("address") for h in ft_layout.load_layout().get("hosts", [])}
            for h in found:
                h["added"] = h["address"] in known
            self.hostsFound.emit(found)
        threading.Thread(target=work, daemon=True).start()

    @Slot(str, str, str, str, str)
    def signIn(self, name, address, dongle, user, password):
        """Asks the host for Frametop's API token with its Web UI login, or verifies a Mac's
        Screen Sharing login on port 5900 (answer: signedIn). Over its dongle when it has one
        that answers, so the password skips the router. A host without tokens on 47990 is plain
        Sunshine: the login is checked, and kept in memory for pairing the displays picked next."""
        name, address, dongle = " ".join(name.split()), address.strip(), dongle.strip()
        problem = self._host_problem(name, address, dongle)
        if problem:
            return self.signedIn.emit(name, False, problem)
        existing = next((h for h in ft_layout.load_layout().get("hosts", []) if h.get("address") == address), None)
        existing_kind = existing.get("kind") if existing else None
        targets = ([dongle] if dongle else []) + [address]

        def work():
            web_err = ""
            if existing_kind != "apple":
                try:
                    token, pin, kind = ft_hostsignin.sign_in(targets, user, password,
                                                             known=ft_hostapi.first_word(ft_hostapi.pin_path(address)))
                    if kind == "sunshine":
                        self._logins[address] = f"{user}\n{password}\n"
                    return self._signInDone.emit(name, address, dongle, token, pin, kind, "")
                except ft_hostsignin.SignInError as e:
                    web_err = str(e)

            # macOS High Performance Screen Sharing (screensharingd on TCP 5900, RFB 003.889):
            # verifies credentials over SRP-6a / RSA and writes ADDRESS.apple (0600). Only for a
            # Mac: a host already added as another kind (a Vibepollo host has no kind) never sends
            # its Web UI password to port 5900, and a new address must answer with Apple's banner.
            is_mac = existing_kind == "apple" or (existing is None and any(map(self._is_apple_rfb, targets)))
            if not is_mac:
                return self._signInDone.emit(name, address, dongle, "", "", "", web_err or "Couldn't reach it")
            try:
                args = ["auth", address, "--login", *(["--via", dongle] if dongle else [])]
                code, out, err = ft_layout.run_appless(*args, timeout=20, stdin=f"{user}\n{password}\n")
                if code == 0:
                    return self._signInDone.emit(name, address, dongle, "", "", "apple", "")
                apple_err = (err or out).strip().splitlines()[-1] if (err or out).strip() else ""
                self._signInDone.emit(name, address, dongle, "", "", "", apple_err or "Authentication failed")
            except (OSError, subprocess.SubprocessError) as e:
                self._signInDone.emit(name, address, dongle, "", "", "", f"Couldn't reach it: {str(e) or type(e).__name__}")
        threading.Thread(target=work, daemon=True).start()

    @staticmethod
    def _host_problem(name, address, dongle):
        """Why a new host can't be added under that name and address, or ""."""
        if not re.fullmatch(NAME_RE, name):
            return "A host's name needs 1 to 40 letters, digits, spaces, dots, dashes or apostrophes"
        if not re.fullmatch(r"[A-Za-z0-9.:-]{1,64}", address) or (dongle and not re.fullmatch(r"[0-9.]{7,15}", dongle)):
            return "The address is a host name or an IP address"
        if any(h.get("name") == name and h.get("address") != address for h in ft_layout.load_layout().get("hosts", [])):
            return f"There's already a host called {name}"
        return ""

    @Slot(str, str, str)
    def addPinHost(self, name, address, dongle):
        """A host paired by PIN, with no Web UI sign-in (issue 107: an unmodified Sunshine, or
        another Moonlight host such as a USB KVM): each display's client is paired with a PIN
        Frametop shows (pairingPin) and you type in on the host. Answer: signedIn."""
        name, address, dongle = " ".join(name.split()), address.strip(), dongle.strip()
        problem = self._host_problem(name, address, dongle)
        if problem:
            return self.signedIn.emit(name, False, problem)
        self._sign_in_done(name, address, dongle, "", "", "pin", "")

    def _sign_in_done(self, name, address, dongle, token, pin, kind, error):
        if error:
            return self.signedIn.emit(name, False, error)
        if kind not in ("pin", "apple"):  # no token: a PIN pairs each display, a Mac's login is ft-appless's
            ft_hostsignin.keep(address, token, pin)
        layout = ft_layout.load_layout()
        host = next((h for h in layout.setdefault("hosts", []) if h.get("address") == address), None)
        if host is None:
            host = {"name": name, "address": address, "displays": []}
            layout["hosts"].append(host)
        if kind in ("sunshine", "pin", "apple"):
            host["kind"] = kind
        else:
            host.pop("kind", None)
        if dongle and dongle not in host.get("direct", []):
            host["direct"] = [dongle] + host.get("direct", [])
        try:
            ft_layout.save_layout(layout)
        except (OSError, ft_layout.LayoutFileError) as e:   # a layout that doesn't read isn't saved over
            return self.signedIn.emit(host["name"], False, str(e))
        self.changed.emit()
        threading.Thread(target=self._ping, args=(host,), daemon=True).start()
        self.signedIn.emit(host["name"], True, "")

    @Slot(str)
    def forgetLogin(self, address):
        """A Sunshine host's login, once its displays are queued for pairing (or none were)."""
        self._logins.pop(address, None)
        self.changed.emit()

    @Slot(str)
    def removeHost(self, name):
        layout = ft_layout.load_layout()
        host = next((h for h in layout.get("hosts", []) if h.get("name") == name), None)
        if host is None:
            return
        if host.get("displays"):
            return self.message.emit("Remove its displays first", True)
        layout["hosts"].remove(host)
        if not self._save(layout):
            return
        if not any(h.get("address") == host.get("address") for h in layout["hosts"]):
            ft_hostsignin.forget(host.get("address", ""))
        self.changed.emit()

    @Slot(str, bool)
    def setHostConnected(self, name, on):
        """All of a host's displays at once."""
        layout = ft_layout.load_layout()
        host = next((h for h in layout.get("hosts", []) if h.get("name") == name), None)
        ids = [d["id"] for d in (host or {}).get("displays", []) if bool(d.get("off")) == on]
        if ids:
            self._run(f"{'Connecting' if on else 'Disconnecting'} {name}", "remote", "connect" if on else "disconnect", *ids)

    @Slot(str, str)
    def setRoute(self, name, route):
        self._run(f"Connecting {name} {dict(auto='either way', network='over the network', dongle='over the dongle')[route]}",
                  "remote", "host", name, f"route={route}")

    @Slot(str, bool)
    def setMic(self, name, on):
        """Send the Frame's microphone to the host (its streams pick it up within 3 s)."""
        self._run(f"{'Sending' if on else 'Not sending'} the microphone to {name}",
                  "remote", "host", name, f"mic={'on' if on else 'off'}")

    @Slot(str, str, bool)
    def setInput(self, name, kind, on):
        """Use the host's own keyboard or mouse ("keyboard", "mouse") in Frametop, or stop, leaving
        the other as it is (its streams pick it up within 3 s)."""
        host = next((h for h in ft_layout.load_layout().get("hosts", []) if h.get("name") == name), None)
        if host is None or kind not in host_input.KINDS:
            return
        kinds = host_input.lent(host) | {kind} if on else host_input.lent(host) - {kind}
        self._run(f"{'Using' if on else 'Not using'} {name}'s {kind}",
                  "remote", "host", name, f"input={host_input.word(kinds)}")

    @Slot(str, bool)
    def setClipboard(self, name, on):
        """Share copied text with the host both ways (its streams pick it up within 3 s)."""
        self._run(f"{'Sharing' if on else 'Not sharing'} the clipboard with {name}",
                  "remote", "host", name, f"clipboard={'on' if on else 'off'}")

    @Slot(str, str, str)
    def appWindowsOn(self, name, user, password):
        """Signs in to the Vibepollo on the host that shares its app windows, with that one's
        Web UI login, and turns them on (answer: appWindowsSet). The token it gives is that
        instance's own; the password isn't kept."""
        host = next((h for h in ft_layout.load_layout().get("hosts", []) if h.get("name") == name), None)
        if host is None or not host.get("address"):
            return self.appWindowsSet.emit(name, False, f"There's no host called {name}")
        address = host["address"]

        def known(port):
            """The pin its Web UI must have: its own from an earlier sign-in, or on the port the
            displays use, the one seen when the host was added."""
            own = ft_hostapi.first_word(ft_hostapi.own_token_path(address, port)[:-len(".token")] + ".pin")
            return own or (ft_hostapi.first_word(ft_hostapi.pin_path(address))
                           if port == ft_hostapi.GAMESTREAM_PORT else "")

        def work():
            try:
                port, token, pin = ft_hostsignin.app_windows(host.get("direct", []) + [address], user, password, known)
            except ft_hostsignin.SignInError as e:
                return self._appWindowsDone.emit(name, address, 0, "", "", str(e))
            self._appWindowsDone.emit(name, address, port, token, pin, "")
        threading.Thread(target=work, daemon=True).start()

    def _app_windows_done(self, name, address, port, token, pin, error):
        if error:
            return self.appWindowsSet.emit(name, False, error)
        ft_hostsignin.keep(address, token, pin, port)
        self._run(f"Turning on {name}'s app windows", "remote", "host", name, f"windows={port}")
        self.appWindowsSet.emit(name, True, "")

    @Slot(str)
    def appWindowsOff(self, name):
        """Its apps and windows leave the launcher. Window panels that are open stay until
        closed, so its token stays too."""
        self._run(f"Turning off {name}'s app windows", "remote", "host", name, "windows=off")

    @Slot(str, str)
    def setDirect(self, name, text):
        addresses = [a for a in re.split(r"[\s,]+", text.strip()) if a]
        if any(not re.fullmatch(r"[A-Za-z0-9.:-]{1,64}", a) for a in addresses):
            return self.message.emit("The dongle's address is an IP address", True)
        self._run(f"Setting {name}'s dongle", "remote", "host", name, "direct=" + (",".join(addresses) or "none"))

    @staticmethod
    def _host_id(address):
        """A host's <uniqueid> from its serverinfo (Vibepollo can take seconds to answer)."""
        url = f"http://{address}:{47989}/serverinfo?uniqueid=0123456789ABCDEF&uuid={uuid.uuid4()}"
        with urllib.request.urlopen(url, timeout=12) as r:
            m = re.search(r"<uniqueid>([^<]+)</uniqueid>", r.read().decode(errors="replace"))
        return m.group(1) if m else None

    @staticmethod
    def _is_apple_rfb(address):
        """Whether `address` speaks Apple Screen Sharing's RFB 003.889 banner on TCP 5900."""
        try:
            with socket.create_connection((address, ft_layout.APPLE_PORT), timeout=2) as s:
                deadline, banner = time.monotonic() + 2, b""
                while len(banner) < 12 and (left := deadline - time.monotonic()) > 0:
                    s.settimeout(left)
                    part = s.recv(12 - len(banner))
                    if not part:
                        break
                    banner += part
                return banner == b"RFB 003.889\n"
        except OSError:
            return False

    @Slot(str)
    def findDongle(self, name):
        """The host among the Frame hotspot's clients (answer: dongleFound)."""
        host = next((h for h in ft_layout.load_layout().get("hosts", []) if h.get("name") == name), None)
        if host is None:
            return

        def work():
            try:
                with open("/proc/net/arp") as f:  # IP, HW type, flags, MAC, mask, device
                    rows = [line.split() for line in f.read().splitlines()[1:]]
                candidates = [r[0] for r in rows if len(r) >= 6 and r[5] == "wlanap" and r[2] != "0x0"]
                if host.get("kind") == "apple":
                    matches = [ip for ip in candidates if self._is_apple_rfb(ip)]
                    if len(matches) == 1:
                        return self.dongleFound.emit(name, matches[0], "")
                    if len(matches) > 1:
                        return self.dongleFound.emit(name, "", "more than one Mac is on the Frame's hotspot: type its address")
                    return self.dongleFound.emit(name, "", "it isn't on the Frame's hotspot" if candidates else
                                                 "nothing is on the Frame's hotspot (is Steam Link's dongle paired?)")
                want = self._host_id(host["address"])
                if not want:
                    return self.dongleFound.emit(name, "", "it didn't say who it is at its own address")
                for ip in candidates:
                    try:
                        if self._host_id(ip) == want:
                            return self.dongleFound.emit(name, ip, "")
                    except OSError:
                        continue
                self.dongleFound.emit(name, "", "it isn't on the Frame's hotspot" if candidates else
                                      "nothing is on the Frame's hotspot (is Steam Link's dongle paired?)")
            except OSError as e:
                self.dongleFound.emit(name, "", str(e))
        threading.Thread(target=work, daemon=True).start()

    # --- displays ---
    @staticmethod
    def _sunshine_monitors(address):
        """A Mac's displays: its Sunshine instances, each on its own port, named by the host
        setup "COMPUTER: DISPLAY (WxH)" (its serverinfo hostname)."""
        def probe(port):
            url = f"http://{address}:{port}/serverinfo?uniqueid=0123456789ABCDEF&uuid={uuid.uuid4()}"
            try:
                with urllib.request.urlopen(url, timeout=4) as r:
                    m = re.search(r"<hostname>([^<]*)</hostname>", r.read().decode(errors="replace"))
            except OSError:
                return None
            name = html.unescape(m.group(1)) if m else f"port {port}"
            size = re.search(r"\s*\((\d+)x(\d+)\)\s*$", name)
            label = name[:size.start()] if size else name
            return {"app": "Desktop", "port": port, "key": f"Desktop@{port}",
                    "name": label.split(": ", 1)[-1] or name,
                    "width": int(size.group(1)) if size else 2560, "height": int(size.group(2)) if size else 1440,
                    "primary": port == SUNSHINE_PORTS[0], "active": True, "virtual": False}
        with concurrent.futures.ThreadPoolExecutor(len(SUNSHINE_PORTS)) as pool:
            return [m for m in pool.map(probe, SUNSHINE_PORTS) if m]

    @Slot(str)
    def listMonitors(self, address):
        """The host's monitors, from its Web UI API, or a Mac's Sunshine instances (answer:
        monitorsReady)."""
        host = next((h for h in ft_layout.load_layout().get("hosts", []) if h.get("address") == address), {})

        def work():
            monitors, error = [], ""
            if host.get("kind") == "pin":  # no API to list them: the host's desktop, by its app's name
                return self.monitorsReady.emit(address, [dict(PIN_DESKTOP)], "")
            if host.get("kind") == "apple":
                monitors = [{"app": "apple:physical", "port": 0, "key": "apple:physical",
                             "name": "Main display", "width": 1920, "height": 1080,
                             "primary": True, "active": True, "virtual": False}]
                return self.monitorsReady.emit(address, monitors, "")
            if host.get("kind") == "sunshine":
                monitors = self._sunshine_monitors(address)
                return self.monitorsReady.emit(address, monitors, "" if monitors else "no Sunshine instance answers")
            try:
                code, out, err = ft_layout.run_stream("monitors", address, timeout=30)
                if code:
                    error = (err or out).strip().splitlines()[-1] if (err or out).strip() else f"exit {code}"
                else:
                    for m in json.loads(out[out.index("["):]):
                        info = m.get("info") or {}
                        res = info.get("resolution") or {}
                        virtual = (m.get("edid") or {}).get("manufacturer_id") == "SDD" or \
                            str(m.get("friendly_name", "")).startswith(("frametop", "Vibepollo Private Display"))
                        monitors.append({"app": "display:" + m.get("device_id", ""), "port": 0,
                                         "key": "display:" + m.get("device_id", ""),
                                         "name": m.get("friendly_name") or m.get("display_name") or "?",
                                         "width": int(res.get("width", 0)), "height": int(res.get("height", 0)),
                                         "primary": bool(info.get("primary")), "active": bool(info),
                                         "virtual": virtual})
            except Exception as e:  # noqa: BLE001  (shown to the user, not raised in a thread)
                error = str(e)
            self.monitorsReady.emit(address, monitors, error)
        threading.Thread(target=work, daemon=True).start()

    @Slot(str, str, str, int, int, int, int, int)
    def addDisplay(self, host, app, label, width, height, fps, bitrate, port):
        """Pairs a client for it with the host's token, or a Sunshine instance's (port) with the
        login kept since signing in, and starts its stream (ft-layout)."""
        layout = ft_layout.load_layout()
        h = next((x for x in layout.get("hosts", []) if x.get("name") == host), None)
        if h is None:
            return self.message.emit(f"No host called {host}", True)
        label = " ".join(label.split()) or (
            "Virtual" if app == "monitor" or app.startswith("apple:virtual")
            else "Main" if app == "apple:physical" else "Display")
        args = ["remote", "add", host, h["address"], app, "--label", label, "--size", f"{width}x{height}",
                "--fps", str(fps), "--bitrate", str(bitrate)]
        login = None
        if h.get("kind") == "pin":
            args.append("--pin")
            return self._run(f"Pairing {label} with {host}", *args)
        if port:
            login = self._logins.get(h["address"])
            if login is None:
                return self.message.emit(f"Sign in to {host} again to add a display", True)
            args += ["--port", str(port), "--login"]
        self._run(f"Adding {label} from {host}", *args, stdin=login)

    @Slot(str, bool)
    def setConnected(self, display_id, on):
        self._run("Connecting" if on else "Disconnecting", "remote", "connect" if on else "disconnect", display_id)

    @Slot(str, int, int, int, int)
    def setStream(self, display_id, width, height, fps, bitrate):
        """Its stream's resolution, frame rate and bitrate (it starts over)."""
        self._run("Changing the stream", "remote", "set", display_id, f"size={width}x{height}", f"fps={fps}",
                  f"bitrate={bitrate}", then=lambda: self._monitors.refresh_of(display_id))

    @Slot(str, int, int, int, int)
    def setRouteStream(self, display_id, direct_fps, direct_bitrate, network_fps, network_bitrate):
        """Per-route frame rate and bitrate overrides (direct/dongle vs network).
        fps: <0 unchanged, 0 reset to display default, >0 explicit fps.
        bitrate: <=-2 unchanged, -1 reset to display default, >=0 explicit kbit/s."""
        pairs = []
        for route, fps, bitrate in (("direct", direct_fps, direct_bitrate), ("network", network_fps, network_bitrate)):
            if fps == 0:
                pairs.append(f"{route}.fps=")
            elif fps > 0:
                pairs.append(f"{route}.fps={fps}")
            if bitrate == -1:
                pairs.append(f"{route}.bitrate=")
            elif bitrate >= 0:
                pairs.append(f"{route}.bitrate={bitrate}")
        if pairs:
            self._run("Changing route settings", "remote", "set", display_id, *pairs)

    def _save(self, layout):
        """Save the layout; False, said in the window, when it can't be (a frametop-layout.json
        that doesn't read is never saved over)."""
        try:
            ft_layout.save_layout(layout)
            return True
        except (OSError, ft_layout.LayoutFileError) as e:
            self.message.emit(str(e), True)
            return False

    def _edit(self, display_id, fn):
        layout = ft_layout.load_layout()
        n, _, d = ft_layout.find_display(layout, display_id)
        fn(d)
        if not self._save(layout):
            return None, d
        self.changed.emit()
        return n, d

    @Slot(str, float)
    def setMetres(self, display_id, m):
        n, d = self._edit(display_id, lambda d: d.__setitem__("metres", round(m, 3)))
        if n is not None and self._running and not d.get("off"):
            self._ask(f"width {n} {m:.3f}")

    @Slot(str, bool)
    def setShown(self, display_id, shown):
        n, d = self._edit(display_id, lambda d: d.pop("hidden", None) if shown else d.__setitem__("hidden", True))
        if n is not None and self._running and not d.get("off"):
            self._ask(f"{'reveal' if shown else 'conceal'} {n}")

    @Slot(str)
    def removeDisplay(self, display_id):
        self._run("Removing the display", "remote", "remove", display_id)

    @Slot()
    def cancelPairing(self):
        """Ends a PIN pairing that's waiting for its PIN (ft-layout remote add --pin). Killing our
        ft-layout wouldn't reach the ft-stream waiting in its container, so ft-layout remote
        cancel-pin ends that one, and the pairing then ends as cancelled."""
        if self._proc is not None and self._busy.startswith("Pairing "):
            self._start_detached(host_command(os.path.abspath(FT_LAYOUT), "remote", "cancel-pin"))

    def _start_detached(self, argv):  # tests replace it
        QProcess.startDetached(argv[0], argv[1:])

    # --- ft-layout on the host, one at a time ---
    def _run(self, label, *args, stdin=None, then=None):
        """Queues ft-layout ARGS; then() once it has run (either way)."""
        self._queue.append((label, args, stdin, then))
        if self._proc is None:
            self._next()

    def _next(self):
        if not self._queue:
            return
        label, args, stdin, then = self._queue.pop(0)
        self._busy = label
        self.busyChanged.emit()
        proc = QProcess(self)
        proc.setProcessChannelMode(QProcess.MergedChannels)
        argv = host_command(os.path.abspath(FT_LAYOUT), *args)
        proc.finished.connect(lambda code, _status: self._done(proc, label, code, then))
        proc.readyReadStandardOutput.connect(lambda: self._said(proc, label))
        self._proc = proc
        self._out, self._pin_said = [], False
        proc.start(argv[0], argv[1:])
        if stdin is not None:  # a Sunshine login for ft-layout remote add --login
            proc.write(stdin.encode())
        proc.closeWriteChannel()

    def _said(self, proc, label):
        """ft-layout's output as it comes: a PIN to type in on the host shows at once."""
        self._out.append(bytes(proc.readAllStandardOutput()).decode(errors="replace"))
        m = None if self._pin_said else re.search(r"^PIN (\d{4}):", "".join(self._out), re.M)
        if m:
            self._pin_said = True
            self.pairingPin.emit(label, m.group(1))

    def _done(self, proc, label, code, then=None):
        self._said(proc, label)
        out = "".join(self._out).strip()
        self._proc = None
        self._busy = ""
        self.busyChanged.emit()
        self.changed.emit()
        self._check()
        last = out.splitlines()[-1] if out else ""
        if code == 0:
            self.message.emit(f"{label}: done" + (f" ({last})" if last and not last.startswith("ok") else ""), False)
        else:
            self.message.emit(f"{label} failed: {last or 'exit code ' + str(code)}", True)
        if then is not None:
            then()
        self._next()
