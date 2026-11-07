#!/usr/bin/env python3
"""ft-layout: the Frametop screens' sizes and where they float around you.

Two desktop backends (BACKEND in ~/.config/frametop.conf):
  screens    (default) ft-screens, our own compositor (screens): each screen
             is its own SteamVR panel with its own resolution and size in metres. ft-layout
             places them directly through ft-screens' control socket (@ft_screens).
  gamescope  the old path: every screen one size (at most 1920x1080 worth of pixels),
             panels owned by the SteamVR dashboard, so ft-layout floats each one with
             `vrcmd --dock-overlay` and the pointer helper carries it into place.

The layout is relative to your head when it's applied: its position and the direction
you face (yaw only), like a recenter. It lives in ~/.config/frametop-layout.json:
  {"auto": true,                      arrange when the desktop starts
   "mode": "preset" | "custom",
   "preset": {"kind": "arc" | "flat", "rows": 1, "distance": 2.0, "gap": 0.05, "height": 0},
   "primary": 2,                      the screen with the taskbar (1-based; default: the biggest)
   "layouts": {"Work": [{"pos": ..., "face": ..., "roll": ..., "metres": ..., "curve": ...,
                         "pin": ...}, ...]},   named layouts: each screen's place (SPATIAL)
   "active": "Work",                  the named layout the custom arrangement came from
   "profiles": {"Work": {"hidden": [3], "windows": [...]}},   what a named layout opens too
                                      (docs/profiles.md): screens hidden on their own, and the
                                      apps' windows (ft-floatd's "windows")
   "default_profile": "Work",         the profile the desktop starts with (FT_PROFILE overrides)
   "visibility": {"mode": "always",   ft-screens: always | dashboard (only with the SteamVR
                  "wrist_angle": 60,    dashboard open) | gesture (while you look at a controller)
                  "gesture_hand": "left", "gesture_angle": 20},   | toggle (hidden until shown);
                                      wrist_angle: a pinned screen shows while you see its front
                                      within this many degrees
   "screens": [{"size": [w, h], "metres": 3.6,        ft-screens: pixels, and width in VR
                "curve": 0,                           ft-screens: cylinder radius in metres, 0 = flat
                "pin": {"hand": "left", "rel": [12]},  ft-screens: riding on that controller
                                                      (left | right), or on the headset (head)
                "scale": 1.0,                         KWin output scale (1.0 = 100%)
                "hidden": true,                       ft-screens: hidden on its own, whatever the
                                                      visibility mode (ft-layout hide N)
                "pos": [x, y, z], "face": [yaw, pitch], "roll": 0,   custom layout
                "rotation": "normal" | "left" | "right"}, ...],      gamescope only
   "panel_size": [w, h],              gamescope: last measured panel size
   "hosts": [{"name": "desk-pc", "address": "192.0.2.20", ft-screens: other machines' displays
              "direct": ["10.35.78.20"],          its Steam Link dongle on the Frame's hotspot
              "route": "auto",                    auto (the dongle when it answers) | network |
                                                          dongle (only; ft-stream reads both)
              "mic": false,                       send the Frame's microphone to it (needs
                                                          Frametop's Vibepollo on Windows; ft-stream)
              "input": false,                     use its own keyboard and mouse in Frametop while
                                                          the headset is worn (needs Frametop's
                                                          Vibepollo on Linux; ft-stream)
              "displays": [{"id": "desk-pc-oled",         (docs/remote-displays.md), each a panel
                            "client": "frametop-1",       streamed by ft-stream: its paired client,
                            "app": "display:{GUID}",      what it streams (display:DEVICE, monitor:
                                                          a Vibepollo Remote Monitor, primary, or
                                                          an app's name: Desktop for Sunshine),
                            "port": 48089,                its Sunshine instance's HTTP port, when
                                                          not 47989 (a Mac runs one per display)
                            "label": "OLED", "size": [5120, 1440], "fps": 60, "bitrate": 50000,
                            "metres": 1.8, "curve": 0, "pin": ..., "hidden": true,
                            "off": true,                  disconnected: no stream, no panel
                            "pos": ..., "face": ..., "roll": 0}]}]}   as a screen's
Remote displays are screens 101 and up, in the order they're listed. Without a place of their
own they go in a row above the screens. A profile keeps the ones connected when it was saved,
like its apps: their places and hidden state by id (profiles[NAME]["remote"]). Opening it, or
arranging while it's the one in use, connects each whose host answers and puts it there; the
others stay as they are.
Custom positions: x right, y up, -z forward from the head, in metres; face = the
direction you look to see the screen's front straight on, in degrees, relative to your
heading; roll = the panel turned about its front, counterclockwise as you see it.
Presets: "arc" hinges the screens edge to edge around you, each turned to face you
(like monitors on a desk); "flat" puts them on one flat wall facing forward. Screen 1
is top left, then left to right.

Usage (on the Frame host; Frametop Settings calls it too):
  ft-layout apply [--wait SECONDS]   arrange every screen; --wait is for desktop start:
                                     wait for the screens, skip if "auto" is off
  ft-layout reset                    recenter after the 3-second countdown (Meta+Shift+R,
                                     screen reset button, or mapped button). With ft-screens,
                                     move unpinned panels only; preserve apps and streams.
                                     Use NAME explicitly to restore its full profile.
  ft-layout capture                  save the current arrangement as the custom layout
  ft-layout save NAME                save it as a named layout too, and use that; with the
                                     desktop's apps and hidden screens, as a profile
  ft-layout use NAME                 switch to a named layout, arrange the screens in it, and
                                     open its apps (moving open windows, nothing closed)
  ft-layout start [--wait SECONDS]   desktop start: the profile in FT_PROFILE or default_profile,
                                     or else as apply --wait
  ft-layout open NAME                a profile's launcher entry: use it, or start the desktop in it
  ft-layout default NAME|none        the profile the desktop starts with
  ft-layout layouts                  list the named layouts (* = the one in use)
  ft-layout rename OLD NEW | delete NAME
  ft-layout launchers                write each profile's launcher entry again (and drop stale ones)
  ft-layout plan                     print the arrangement as JSON (no VR needed)
  ft-layout scale                    per-screen scale, positions (as the screens are around
                                     you), and primary to KWin
  ft-layout screen-args              ft-screens' --screen arguments for the session script
  ft-layout remote-view              the primary screen's place in the workspace, for the VNC bridge
  ft-layout toggle                   hide or show all screens (ft-screens)
  ft-layout hide N|all               hide a screen on its own (it stays hidden whatever the
  ft-layout show N|all               visibility mode or the hotkey say), or show it again
  ft-layout hidden                   the screens hidden on their own
  ft-layout pin all|N left|right|head  pin screens to a wrist or your head as they are;
                                     unpin all|N
  ft-layout remote list              the remote displays, their numbers and their streams' state
  ft-layout remote monitors ADDRESS  the host's displays (Vibepollo's /api/display-devices, JSON)
  ft-layout remote add HOST ADDRESS APP [--size WxH] [--fps F] [--bitrate KBPS] [--label TEXT]
                     [--port PORT] [--login]
                                     a display of a host (pairs its client with the host's token
                                     first, or with --login, with the Web UI login read from stdin
                                     as two lines: a Sunshine instance on PORT) -> its id
  ft-layout remote set ID size=WxH|fps=F|bitrate=KBPS|label=TEXT|app=APP|metres=M ...
  ft-layout remote host NAME route=auto|network|dongle direct=ADDRESS[,ADDRESS]|none mic=on|off input=on|off
                                     how a host's streams reach it (they start over), whether
                                     the Frame's microphone goes to it, and whether its keyboard
                                     and mouse come to Frametop (both within 3 s, no restart)
  ft-layout remote connect ID...     start their streams again (and keep them on)
  ft-layout remote disconnect ID...  stop their streams and panels until connected again
  ft-layout remote remove ID         stops it and unpairs its client (the host stays)
  ft-layout streams start|stop       streams only: the connected remote displays without the
                                     desktop (ft-screens with no KWin or Plasma); stop ends
                                     only that, never a running desktop
"""
import concurrent.futures
import fcntl
import json
import math
import os
import re
import socket
import subprocess
import sys
import time

LAYOUT_PATH = os.path.expanduser("~/.config/frametop-layout.json")
CONF_PATH = os.path.expanduser("~/.config/frametop.conf")
# Where the screens were last arranged from (save_anchor): state, not a setting, so a reset,
# which leaves the layout file alone, can keep it current.
ANCHOR_PATH = os.path.expanduser("~/.local/state/frametop/layout-anchor.json")
VRCMD = "/opt/steamvr/bin/linuxarm64/vrcmd"
HELPER = "\0ft_pointer_helper"
SCREENS = "\0ft_screens"
# One ft-layout at a time (and one pending reset: LOCK_PATH + ".reset-delay"). In the runtime
# directory, not /tmp, where another user could put a link there first.
LOCK_PATH = os.path.join(os.environ.get("XDG_RUNTIME_DIR") or "/tmp", "ft-layout.lock")
FLOAT = "\0frametop_float"  # ft-floatd: the apps' windows (profiles)
# Streams only (FT_STREAMS_ONLY=1 in the session): ft-screens runs without KWin and Plasma, so
# it shows the remote displays and no screens of the desktop. The session script keeps this
# file while it runs, so every ft-layout run then knows.
STREAMS_ONLY_PATH = os.path.join(os.environ.get("XDG_RUNTIME_DIR") or "/tmp", "frametop-streams-only")
# The sizes Display Settings offers for a screen and Remote Displays for a stream: (w, h, what).
# ft-screens has no pixel limit; portrait screens are just tall.
SCREEN_RESOLUTIONS = [(1920, 1080, ""), (2560, 1440, ""), (3840, 2160, "4K"), (2560, 1080, "ultrawide"),
                      (3440, 1440, "ultrawide"), (5120, 1440, "super ultrawide"), (1920, 1200, "16:10"),
                      (2560, 1600, "16:10"), (1080, 1920, "portrait"), (1440, 2560, "portrait"),
                      (2160, 3840, "portrait 4K")]
REPO = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
sys.path.insert(0, os.path.join(REPO, "conf"))
import ft_conf  # noqa: E402  (frametop.conf's grammar, docs/config.md)
import ft_store  # noqa: E402  (JSON files read and written safely)
import ftipc  # noqa: E402  (questions to the datagram sockets)
sys.path.insert(0, os.path.dirname(os.path.realpath(__file__)))
import layout_geometry  # noqa: E402  (where the screens go, as pure math)
from layout_geometry import (  # noqa: E402, F401  (re-exported: ft_floatd and the tests use them here)
    chain as _chain,
    cross,
    direction,
    dot,
    normalize,
    panel_axes,
    preview_panel,
    relative_pose,
    surface_point,
    to_anchor,
    turn_yaw,
    yaw_pitch,
)
from layout_geometry import wrap as _wrap  # noqa: E402
LAUNCHERS = os.path.expanduser("~/.local/share/applications")  # a profile's launcher entry each
DEFAULT_PANEL = (1.18, 0.664)  # gamescope: a floating 16:9 dashboard panel, measured on the Frame
PIXELS_PER_METRE = 800         # ft-screens: a new screen's default size in VR (1920 px: 2.4 m)
# controllers: when controllers' lasers work the screens (always | outside_games | dashboard).
# in_games: during a VR game, "always" hides the screens unless the dashboard is open (hide),
# or leaves them up (visible).
VISIBILITY = {"mode": "always", "wrist_angle": 60, "gesture_hand": "left", "gesture_angle": 20,
              "controllers": "outside_games", "in_games": "hide"}
SPATIAL = ("pos", "face", "roll", "metres", "curve", "pin")  # what a named layout keeps of a screen
DEFAULTS = {"auto": True, "mode": "preset",
            "preset": {"kind": "arc", "rows": 1, "distance": 2.0, "gap": 0.05, "height": 0.0},
            "screens": [], "panel_size": list(DEFAULT_PANEL), "hosts": []}
REMOTE_FIRST = 101  # ft-screens' remote screens (screens/remote.h)
REMOTE_MAX = 16
REMOTE_PIXELS_PER_METRE = 2400  # a new remote display's width in VR (5120 px: 2.1 m), at least 1 m
GAMESTREAM_PORT = 47989  # a host's HTTP port, where ft-stream reaches it
WEB_UI_PORT = 47990  # Vibepollo's Web UI: a host answers there when it's up
STREAM = os.path.join(REPO, "stream", "build", "ft-stream")
RESET_TIMER = os.path.join(REPO, "layout", "countdown", "build", "ft-reset-timer")


def log(*args):
    print(*args, flush=True)


# ---------------------------------------------------------------- config

def read_conf():
    return ft_conf.read(CONF_PATH)


def backend():
    return "gamescope" if read_conf().get("BACKEND", "screens") == "gamescope" else "screens"


def streams_only():
    """Whether Frametop runs only its remote displays (no desktop): STREAMS_ONLY_PATH."""
    return os.path.exists(STREAMS_ONLY_PATH)


def native_count(layout=None):
    """The desktop's screens ft-screens shows: none when it runs streams only."""
    return 0 if streams_only() else screen_count(layout)


def screen_count(layout=None):
    """ft-screens: the configured screens; gamescope: SCREENS."""
    if backend() == "screens":
        return max(1, len((layout or load_layout()).get("screens", [])))
    try:
        return max(1, int(read_conf().get("SCREENS", "2")))
    except ValueError:
        return 2


# frametop-layout.json is there but doesn't read: it's kept as it is, with a copy next to it, and
# nothing is saved over it.
LayoutFileError = ft_store.StoreError


class Layout(dict):
    """A layout as load_layout() read it: .base is what it was then, so save_layout() writes only
    the keys changed since, over whatever another program saved meanwhile."""
    base = None


def _why_not(saved):
    """Why the saved layout can't be used (or saved over), or None."""
    if not isinstance(saved, dict):
        return "it isn't a JSON object"
    return None if isinstance(saved.get("preset", {}), dict) else '"preset" isn\'t an object'


def read_layout_file():
    """The saved layout as it is in the file, None when there's none. LayoutFileError (after a
    copy is kept) when it isn't a JSON object with an object "preset"."""
    return ft_store.load_json_strict(LAYOUT_PATH, None, _why_not)


def _with_defaults(saved):
    layout = json.loads(json.dumps(DEFAULTS))
    if saved:
        layout.update({k: v for k, v in saved.items() if k != "preset"})
        layout["preset"].update(saved.get("preset", {}))
    if backend() == "screens" and not layout.get("screens"):
        layout["screens"] = [{"size": [1920, 1080], "metres": 1920 / PIXELS_PER_METRE}]
    return layout


def load_layout():
    """The saved layout over DEFAULTS. A file that doesn't read gives the defaults, said once on
    stderr; saving is refused until it's fixed (save_layout)."""
    layout = Layout(_with_defaults(ft_store.load_json(LAYOUT_PATH, None, _why_not, who="ft-layout")))
    layout.base = json.loads(json.dumps(layout))
    return layout


_MISSING = object()


def save_layout(layout):
    """Write layout to frametop-layout.json, under a lock so writers take turns: a layout from
    load_layout() writes only the keys it changed, over the file as it is now (another program's
    changes to other keys stay); any other dict is written whole. A file that doesn't read is
    never written over: LayoutFileError. Written to a temp file of its own, flushed to disk and
    renamed (ft_store), so a reader never sees half a file."""
    with ft_store.locked(LAYOUT_PATH):   # .frametop-layout.json.lock; 10 s, then LayoutFileError
        current = read_layout_file()   # LayoutFileError: refused
        base = getattr(layout, "base", None)
        out = dict(layout)
        if current is not None and base is not None:
            out = _with_defaults(current)
            for k in set(layout) | set(base):
                if layout.get(k, _MISSING) != base.get(k, _MISSING):
                    if k in layout:
                        out[k] = layout[k]
                    else:
                        out.pop(k, None)
        ft_store.write_json(LAYOUT_PATH, out, indent=2, newline=False)
    if isinstance(layout, Layout):
        layout.base = json.loads(json.dumps(layout))


def save_on_the_way(layout):
    """save_layout for what arranging notes down on the way (remote display numbers, the panel
    size, the profile a start uses): when the file doesn't read or stays locked, that's logged
    and the arranging goes on; the file stays as it is."""
    try:
        save_layout(layout)
        return True
    except LayoutFileError as e:
        log(f"layout not saved: {e}")
        return False


def screen_entry(layout, i):
    screens = layout.get("screens", [])
    return screens[i] if i < len(screens) else {}


def screen_scale(layout, i):
    return float(screen_entry(layout, i).get("scale", 1.0))


def screen_pixels(layout, i):
    w, h = screen_entry(layout, i).get("size", [1920, 1080])
    return int(w), int(h)


def screen_metres(layout, i):
    w, _ = screen_pixels(layout, i)
    return float(screen_entry(layout, i).get("metres", w / PIXELS_PER_METRE))


ROLL = {"normal": 0.0, "left": 90.0, "right": -90.0}


def screen_rotation(layout, i):
    if backend() == "screens":
        return "normal"  # portrait screens are simply tall
    r = screen_entry(layout, i).get("rotation", "normal")
    return r if r in ROLL else "normal"


def screen_size(layout, i, panel_size=None):
    """A screen's size in VR (width, height) in metres."""
    if backend() == "screens":
        w, h = screen_pixels(layout, i)
        m = screen_metres(layout, i)
        return m, m * h / w
    w, h = panel_size or layout.get("panel_size") or DEFAULT_PANEL
    return (h, w) if screen_rotation(layout, i) != "normal" else (w, h)


def primary_screen(layout):
    """0-based index of the screen with the taskbar: the chosen one, or the biggest."""
    n = screen_count(layout)
    p = layout.get("primary")
    if isinstance(p, int) and 1 <= p <= n:
        return p - 1
    return max(range(n), key=lambda i: screen_pixels(layout, i)[0] * screen_pixels(layout, i)[1])


# ---------------------------------------------------------------- geometry
# The pure math (head frame, vectors, the screens' rows) is layout_geometry's, re-exported here.
# Replacing one of those names here (a test's patch) doesn't reach plan() or preview_panel(),
# which call layout_geometry's own: patch layout_geometry instead.

def plan(layout, count, panel_size=None):
    """Screen poses in the head frame: [{"pos": (x, y, z), "face": (yaw, pitch), "roll": deg}]."""
    if count <= 0:
        return []  # streams only: no screens of the desktop
    sizes = [screen_size(layout, i, panel_size) for i in range(count)]
    rolls = [ROLL[screen_rotation(layout, i)] for i in range(count)]
    return layout_geometry.place(layout, sizes, rolls)


# ---------------------------------------------------------------- preview (Display Settings)
# Everything in the layout's head frame: where you stood, and the way you faced, when the
# screens were last arranged (the "anchor", saved then in ANCHOR_PATH). A panel is its centre, its axes (x
# right, y up, z out of its front) and size; a curved one is a cylinder of radius "curve"
# round its front, its centre line where the flat one would be (screens/vr.cpp OnSurface).

def inferred_anchor(layout, g):
    """(eye, heading) the screens were arranged from, worked out from screen 1's pose now
    (parse_get) and where the layout puts it; None if it's pinned to a hand."""
    if g["hand"] != "none":
        return None
    t = plan(layout, screen_count(layout))[0]
    heading = yaw_pitch(tuple(-c for c in g["z"]))[0] - t["face"][0]
    return tuple(c - v for c, v in zip(g["center"], turn_yaw(t["pos"], heading))), heading


def save_anchor(eye, heading):
    """Where the screens were arranged from, for Display Settings' preview (which works it
    out from screen 1 without it, so a failed write only logs)."""
    try:
        ft_store.write_json(ANCHOR_PATH, {"eye": [round(v, 4) for v in eye], "heading": round(heading, 2)},
                            indent=None, newline=False)
    except OSError as e:
        log(f"couldn't save where the screens were arranged from: {e}")


def stored_anchor():
    try:
        with open(ANCHOR_PATH) as f:
            a = json.load(f)
        eye = tuple(float(v) for v in a["eye"])
        return (eye, float(a["heading"])) if len(eye) == 3 else None
    except (OSError, ValueError, TypeError, KeyError):
        return None


def clear_anchor():
    """At desktop start: the screens start over, so where they were arranged from is unknown
    until they're arranged again (the preview works it out from screen 1 meanwhile)."""
    try:
        os.remove(ANCHOR_PATH)
    except FileNotFoundError:
        pass
    except OSError as e:
        log(f"couldn't forget where the screens were arranged from: {e}")


def planned_panels(layout, count, displays=None):
    """The screens where the layout puts them (and, given displays, the remote displays that
    are on), as preview panels in the head frame."""
    out = []
    for i, t in enumerate(plan(layout, count)):
        w, h = screen_size(layout, i)
        entry = screen_entry(layout, i)
        out.append({"kind": "screen", "label": str(i + 1), "center": tuple(t["pos"]),
                    "axes": panel_axes(t["face"], t["roll"]), "width": w, "height": h,
                    "curve": float(entry.get("curve", 0) or 0), "hidden": bool(entry.get("hidden"))})
    for (n, host, d), t in zip(displays or [], plan_remotes(layout, count, displays or [])):
        if t is None or d.get("off"):
            continue
        w, h = remote_size(d)
        out.append({"kind": "remote", "label": remote_label(host, d), "center": tuple(t["pos"]),
                    "axes": panel_axes(t["face"], t["roll"]), "width": w, "height": h,
                    "curve": float(d.get("curve", 0) or 0), "hidden": bool(d.get("hidden"))})
    return out


def remote_label(host, d):
    return " ".join(filter(None, (host.get("name"), d.get("label") or d.get("id"))))


def read_room(ask, layout, windows=()):
    """What ft-screens shows now, through ask(text) -> reply or None: the head and every
    panel (screens, the floating windows given as [(panel, title)], connected remote
    displays) in world coordinates. None if ft-screens doesn't answer."""
    def get(n):
        reply = ask(f"get {n}")
        return parse_get(reply) if reply and reply.startswith("ok") else None
    reply = ask("screens")
    if not reply or not reply.startswith("ok"):
        return None
    head = ask("head")
    f = head.split() if head and head.startswith("ok") else []
    room = {"head": (tuple(map(float, f[1:4])), float(f[4])) if len(f) >= 5 else None, "panels": []}
    for i in range(int(reply.split()[1])):
        g = get(i + 1)
        if g:
            room["panels"].append({"kind": "screen", "label": str(i + 1), "number": i + 1, "pose": g,
                                   "hidden": bool(screen_entry(layout, i).get("hidden"))})
    by_number = {n: (host, d) for n, host, d in remote_displays(layout)}
    # Each one ft-screens shows, live or not: one connecting or lost still has its panel there.
    reply = ask("remotes") or ""
    for item in reply.split()[2:] if reply.startswith("ok") else []:
        try:
            n = int(item.split(":")[0])
        except ValueError:
            continue
        g = get(n)
        host, d = by_number.get(n, ({}, {"label": f"remote {n}"}))
        if g:
            room["panels"].append({"kind": "remote", "label": remote_label(host, d), "number": n, "pose": g,
                                   "hidden": bool(d.get("hidden"))})
    for n, title in windows:
        g = get(n)
        if g:
            room["panels"].append({"kind": "window", "label": title, "number": n, "pose": g, "hidden": False})
    return room


def preview(layout, room=None):
    """Display Settings' preview: {"live", "head": {"x", "y", "z", "yaw", "tracked"},
    "panels": [...], "planned": [...]} in the head frame (see preview_panel). Live (room from
    read_room): the panels where they are now, and the screens where the layout puts them as
    "planned". Otherwise the layout's screens and remote displays. The head is where you are
    ("tracked"), or with no head pose, where the screens were arranged from."""
    count = screen_count(layout)
    if not room or not room["panels"]:
        eye = (0.0, 0.0, 0.0)
        return {"live": False, "head": {"x": 0.0, "y": 0.0, "z": 0.0, "yaw": 0.0, "tracked": False}, "panels": [],
                "planned": [preview_panel(p, eye) for p in planned_panels(layout, count, remote_displays(layout))]}
    anchor = stored_anchor()
    if anchor is None:
        first = next((p["pose"] for p in room["panels"] if p["kind"] == "screen" and p["number"] == 1), None)
        anchor = (inferred_anchor(layout, first) if first else None) or room["head"]
    if anchor is None:
        return preview(layout)  # nothing to place the room by (screen 1 on a hand, the headset off)
    if room["head"]:
        eye = to_anchor(room["head"][0], anchor)
        yaw = _wrap(room["head"][1] - anchor[1])
    else:
        eye, yaw = (0.0, 0.0, 0.0), 0.0
    panels = []
    for p in room["panels"]:
        g = p["pose"]
        panels.append({"kind": p["kind"], "label": p["label"], "hidden": p["hidden"],
                       "center": to_anchor(g["center"], anchor),
                       "axes": tuple(to_anchor(g[k], anchor, point=False) for k in ("x", "y", "z")),
                       "width": g["metres"], "height": g["height"], "curve": g["curve"]})
    return {"live": True, "head": {"x": round(eye[0], 4), "y": round(eye[1], 4), "z": round(eye[2], 4),
                                   "yaw": round(yaw, 2), "tracked": bool(room["head"])},
            "panels": [preview_panel(p, eye, yaw) for p in panels],
            "planned": [preview_panel(p, eye, yaw) for p in planned_panels(layout, count)]}


class NoAnswer(RuntimeError):
    """The other side isn't there or didn't reply (as opposed to replying with an error)."""


class Socket:
    """Request/reply over an abstract datagram socket (ft-screens or the pointer helper)."""

    def __init__(self, address, what):
        self.address, self.what = address, what
        self.client = ftipc.Client()

    def ask(self, text, timeout=10.0):
        reply = self.client.ask(self.address, text, timeout)
        if reply is None:
            raise NoAnswer(f"{self.what} didn't answer ({self.client.why})")
        if not reply.startswith("ok"):
            raise RuntimeError(reply)
        return reply

    def close(self):
        self.client.close()


# ---------------------------------------------------------------- ft-screens

def screens_socket():
    return Socket(SCREENS, "ft-screens (the desktop's compositor) isn't running or")


def screen_args(layout=None):
    layout = layout or load_layout()
    return " ".join(f"--screen {w}x{h}@{screen_metres(layout, i):.3f}"
                    for i, (w, h) in ((i, screen_pixels(layout, i)) for i in range(screen_count(layout))))


def visibility(layout):
    v = dict(VISIBILITY)
    v.update(layout.get("visibility", {}))
    return v


def send_visibility(sock, layout):
    v = visibility(layout)
    sock.ask(f"visibility {v['mode']}")
    sock.ask(f"wrist {float(v['wrist_angle']):.1f}")
    sock.ask(f"gesture {v['gesture_hand']} {float(v['gesture_angle']):.1f}")
    sock.ask(f"controllers {v['controllers']}")
    sock.ask(f"ingames {v['in_games']}")


def send_hidden(sock, layout):
    """Screens hidden on their own (ft-screens' conceal/reveal)."""
    for i in range(screen_count(layout)):
        word = "conceal" if screen_entry(layout, i).get("hidden") else "reveal"
        try:
            sock.ask(f"{word} {i + 1}")
        except RuntimeError as e:
            log(f"screens hidden on their own: {e}")  # an ft-screens from before conceal
            return


def set_hidden(which, hidden):
    """Hide (or show) screen N (1-based) or "all" on its own: saved, and applied if the
    desktop runs."""
    layout = load_layout()
    n = screen_count(layout)
    remotes = {num: d for num, _, d in remote_displays(layout)}
    if which == "all":
        picked, picked_remote = range(n), list(remotes)
    elif which.isdigit() and int(which) in remotes:
        picked, picked_remote = [], [int(which)]
    else:
        picked, picked_remote = ([int(which) - 1] if which.isdigit() else []), []
    if not (picked or picked_remote) or not all(0 <= i < n for i in picked):
        raise RuntimeError(f"no screen {which} (1 to {n}, or a remote display's number)")
    for num in picked_remote:
        if hidden:
            remotes[num]["hidden"] = True
        else:
            remotes[num].pop("hidden", None)
    screens = layout.setdefault("screens", [])
    while len(screens) < n:
        screens.append({})
    for i in picked:
        if hidden:
            screens[i]["hidden"] = True
        else:
            screens[i].pop("hidden", None)
    save_layout(layout)
    try:
        sock = screens_socket()
        for num in [i + 1 for i in picked] + picked_remote:
            sock.ask(f"{'conceal' if hidden else 'reveal'} {num}")
    except RuntimeError as e:
        log(f"saved; not applied now: {e}")


def parse_get(reply):
    """ft-screens' "get": pose, size, curve, and the pin (hand and controller->screen)."""
    f = reply.split()[1:]
    g = list(map(float, f[:15]))
    out = {"center": tuple(g[0:3]), "x": tuple(g[3:6]), "y": tuple(g[6:9]), "z": tuple(g[9:12]),
           "metres": g[12], "height": g[13], "curve": g[14], "hand": f[15] if len(f) > 15 else "none"}
    if out["hand"] != "none" and len(f) >= 28:
        out["rel"] = [round(float(v), 5) for v in f[16:28]]
    return out


def screens_up(sock):
    """How many screens ft-screens has shown so far."""
    f = sock.ask("screens").split()
    return sum(1 for s in f[2:] if not s.split(":")[1].startswith("0x"))


def apply_screens(wait=0):
    layout = load_layout()
    count = native_count(layout)
    deadline = time.time() + wait
    while True:
        try:
            sock = screens_socket()
            if screens_up(sock) >= count or time.time() >= deadline:
                break
        except RuntimeError:
            if time.time() >= deadline:
                raise
        time.sleep(1)
    start_remotes(sock, layout)
    save_on_the_way(layout)  # screen numbers given to new remote displays
    try:
        f = sock.ask("head").split()
    except RuntimeError:
        # No head pose (the headset off or asleep): the screens aren't placed now, but the
        # visibility mode and the screens hidden on their own still reach ft-screens, which
        # otherwise keeps its defaults (always, nothing concealed) until a reset.
        send_visibility(sock, layout)
        if count:
            send_hidden(sock, layout)
        raise
    eye, heading = tuple(map(float, f[1:4])), float(f[4])
    send_visibility(sock, layout)
    results = []
    for i, t in enumerate(plan(layout, count)):
        world = turn_yaw(t["pos"], heading)
        center = tuple(e + v for e, v in zip(eye, world))
        entry = screen_entry(layout, i)
        sock.ask(f"width {i + 1} {screen_metres(layout, i):.4f}")
        sock.ask(f"curve {i + 1} {float(entry.get('curve', 0)):.3f}")
        results.append(sock.ask("place %d %.4f %.4f %.4f %.3f %.3f %.3f" % (i + 1, *center, t["face"][0] + heading,
                                                                             t["face"][1], t["roll"])))
        pin = entry.get("pin") if layout.get("mode") == "custom" else None
        if pin and len(pin.get("rel", [])) == 12:
            try:
                sock.ask(f"pin {i + 1} {pin['hand']} " + " ".join(f"{v:.5f}" for v in pin["rel"]))
            except RuntimeError as e:
                log(f"screen {i + 1}: {e}")  # that controller isn't on
    if count:
        send_hidden(sock, layout)
    place_remotes(sock, layout, count, eye, heading)
    try:
        sock.ask("vrkeyboard close")  # the keyboard, if open, goes too: a reset starts over
    except RuntimeError:
        pass  # an older ft-screens
    save_anchor(eye, heading)
    if count:
        log(f"arranged {count} screen(s)")
    return results


def recenter_screens():
    """Recenter existing panels without restoring apps or display state.

    Named layout poses are read into this invocation only. Do not save that copy:
    Reset must not change window state, hidden screens, or remote connections.
    A panel pinned to the head (its pin button) goes back in the room, as with a full
    reset, and is pinned again only if the layout saved it so; one on a wrist stays there.
    """
    layout = load_layout()
    name = layout.get("active") if layout.get("mode") == "custom" else None
    if name in layout.get("layouts", {}):
        use_named(layout, name)
    count = native_count(layout)
    custom = layout.get("mode") == "custom"
    targets = [(i + 1, t, screen_entry(layout, i).get("pin") if custom else None)
               for i, t in enumerate(plan(layout, count))]
    displays = remote_displays(layout)
    targets.extend((n, t, d.get("pin")) for (n, _, d), t in zip(displays, plan_remotes(layout, count, displays))
                   if not d.get("off") and t is not None)
    sock = screens_socket()
    movable = []
    try:
        for n, t, saved in targets:
            try:
                current = parse_get(sock.ask(f"get {n}"))
            except NoAnswer:
                raise
            except RuntimeError as e:
                log(f"recenter: screen {n} unavailable; left alone ({e})")
                continue
            if current["hand"] not in ("none", "head"):
                log(f"recenter: screen {n} is on a wrist; left alone")
                continue
            head_pin = saved if saved and saved.get("hand") == "head" and len(saved.get("rel", [])) == 12 else None
            movable.append((n, t, head_pin))
        if not movable:
            log("recenter: no unpinned panels available")
            return []
        # Sample once, after the countdown and inspection, immediately before moving.
        f = sock.ask("head").split()
        eye, heading = tuple(map(float, f[1:4])), float(f[4])
        results = []
        for n, t, head_pin in movable:
            center = tuple(e + v for e, v in zip(eye, turn_yaw(t["pos"], heading)))
            # "place" leaves a head-pinned panel in the room
            results.append(sock.ask("place %d %.4f %.4f %.4f %.3f %.3f %.3f" %
                                    (n, *center, t["face"][0] + heading, t["face"][1], t["roll"])))
            if head_pin:
                sock.ask(f"pin {n} head " + " ".join(f"{v:.5f}" for v in head_pin["rel"]))
        try:
            sock.ask("vrkeyboard close")  # the keyboard, if open, goes too, as with a full reset
        except RuntimeError:
            pass  # an older ft-screens
        save_anchor(eye, heading)
        log(f"recentered {len(results)} panel(s); app windows and streams unchanged")
        return results
    finally:
        sock.close()


def check_room(sock):
    """Refuse while SteamVR has lost the room: in a temporary universe, the screens
    placed in the room aren't where they are from the head, so a layout saved from the head
    would put them somewhere else."""
    try:
        f = sock.ask("universe").split()
    except NoAnswer:
        raise
    except RuntimeError:
        return  # an older ft-screens
    if len(f) > 2 and f[2] == "temporary":
        raise RuntimeError("SteamVR has lost the room for now (a temporary tracking universe), so the "
                           "arrangement wasn't saved: try again once tracking has found the room")


def capture_screens():
    layout = load_layout()
    sock = screens_socket()
    check_room(sock)
    f = sock.ask("head").split()
    eye, heading = tuple(map(float, f[1:4])), float(f[4])
    screens = []
    for i in range(screen_count(layout)):
        g = parse_get(sock.ask(f"get {i + 1}"))
        entry = dict(screen_entry(layout, i))
        entry.update(relative_pose(g["center"], g["x"], g["z"], eye, heading))
        entry["metres"] = round(g["metres"], 4)  # resized by hand
        entry["curve"] = round(g["curve"], 3)
        entry.pop("pin", None)
        if "rel" in g:
            entry["pin"] = {"hand": g["hand"], "rel": g["rel"]}
        screens.append(entry)
    layout["screens"] = screens + layout.get("screens", [])[len(screens):]
    layout["mode"] = "custom"
    capture_remotes(sock, layout, eye, heading)
    save_layout(layout)
    save_anchor(eye, heading)  # what they're relative to now
    return screens


# ---------------------------------------------------------------- gamescope (dashboard panels)

def vrcmd(*args, timeout=10):
    # SteamVR's config folder: in the desktop, XDG_CONFIG_HOME is its own (docs/design.md).
    env = dict(os.environ, LD_LIBRARY_PATH=os.path.dirname(VRCMD), XDG_CONFIG_HOME=os.path.expanduser("~/.config"))
    try:
        return subprocess.run([VRCMD, *args], capture_output=True, text=True, timeout=timeout, env=env).stdout
    except (OSError, subprocess.TimeoutExpired):
        return ""


def screen_keys():
    """The screens' overlay keys, in window order. gamescope (PerWindow) names each
    window's overlay frametop.app.<window seq>; .app.0 is its default connector, which
    never gets a window. It must be skipped: docking an unknown key moves whatever
    panel the dashboard shows instead."""
    keys = []
    for m in re.finditer(r"^'(frametop\.app\.(\d+))' .*VROverlayType_Dashboard_Main", vrcmd("--overlays"), re.M):
        if int(m.group(2)) > 0:
            keys.append((int(m.group(2)), m.group(1)))
    return [k for _, k in sorted(keys)]


def visible_keys():
    return set(re.findall(r"^'([^']+)' .* visible VROverlayType", vrcmd("--overlays"), re.M))


def helper_measure(helper, key):
    f = list(map(float, helper.ask(f"measure {key}").split()[1:]))
    return {"center": tuple(f[0:3]), "size": (f[3], f[4]), "x": tuple(f[5:8]), "y": tuple(f[8:11]),
            "z": tuple(f[11:14])}


def float_screen(key):
    """Float a docked screen: dock it into the dashboard (which opens the dashboard; a
    floated panel takes its first position from it, and none exists while it's closed),
    float it, then close the dashboard (so the carry can't snap it back in). A new
    window starts docked in the dashboard, where a dashboard request is ignored as
    redundant (and doesn't open the dashboard), so it goes to theater first."""
    vrcmd("--dock-overlay", "theater", key)
    time.sleep(0.5)
    vrcmd("--dock-overlay", "dashboard", key)
    time.sleep(0.8)
    vrcmd("--dock-overlay", "world", key)
    time.sleep(0.8)
    vrcmd("--hidedashboard")
    time.sleep(0.8)


def apply_gamescope(wait=0):
    layout = load_layout()
    count = screen_count(layout)
    deadline = time.time() + wait
    keys = screen_keys()
    while len(keys) < count and time.time() < deadline:
        time.sleep(1)
        keys = screen_keys()
    if wait:
        time.sleep(3)  # let Plasma draw before the screens start moving
    if not keys:
        raise RuntimeError("no Frametop screens in SteamVR; is the desktop running?")
    keys = keys[:count]
    helper = Socket(HELPER, "the pointer helper (frametop-pointer.service)")
    f = helper.ask("head").split()
    eye, heading = tuple(map(float, f[1:4])), float(f[4])
    vrcmd("--hidedashboard")
    time.sleep(0.5)
    shown = visible_keys()
    size = None
    for key in keys:
        if key not in shown:
            float_screen(key)
        try:
            m = helper_measure(helper, key)
        except RuntimeError:
            float_screen(key)  # floating but without a position: float it again
            m = helper_measure(helper, key)
        # The landscape size: a screen's panel is always landscape before it's rolled.
        size = size or (tuple(sorted(m["size"], reverse=True)))
    results = []
    for key, t in zip(keys, plan(layout, len(keys), size)):
        center = tuple(e + v for e, v in zip(eye, turn_yaw(t["pos"], heading)))
        reply = helper.ask("place %s %.4f %.4f %.4f %.3f %.3f %.3f" % (key, *center, t["face"][0] + heading,
                                                                        t["face"][1], t["roll"]), timeout=30)
        log(reply)
        results.append(reply)
    if size and list(size) != layout.get("panel_size"):
        layout["panel_size"] = [round(size[0], 4), round(size[1], 4)]
        save_on_the_way(layout)
    return results


def capture_gamescope():
    layout = load_layout()
    helper = Socket(HELPER, "the pointer helper (frametop-pointer.service)")
    f = helper.ask("head").split()
    eye, heading = tuple(map(float, f[1:4])), float(f[4])
    shown = visible_keys()
    keys = [k for k in screen_keys() if k in shown]
    if not keys:
        raise RuntimeError("no floating screens to capture (screens docked in the dashboard don't count)")
    screens = []
    for i, key in enumerate(keys):
        m = helper_measure(helper, key)
        entry = dict(screen_entry(layout, i))
        entry.update(relative_pose(m["center"], m["x"], m["z"], eye, heading))
        screens.append(entry)
        layout["panel_size"] = [round(v, 4) for v in sorted(m["size"], reverse=True)]
    layout["screens"] = screens + layout.get("screens", [])[len(screens):]
    layout["mode"] = "custom"
    save_layout(layout)
    return screens



# ---------------------------------------------------------------- remote displays (docs/remote-displays.md)

def remote_displays(layout):
    """[(number, host, display)]: each display's screen number (its "screen", kept so it
    doesn't move when another is removed; or the first free one)."""
    entries = [(h, d) for h in layout.get("hosts", []) for d in h.get("displays", [])]
    used, out = set(), []
    for h, d in entries:
        n = d.get("screen")
        if isinstance(n, int) and REMOTE_FIRST <= n < REMOTE_FIRST + REMOTE_MAX and n not in used:
            used.add(n)
            d["_kept"] = True
    for h, d in entries:
        if not d.pop("_kept", False):
            free = [k for k in range(REMOTE_FIRST, REMOTE_FIRST + REMOTE_MAX) if k not in used]
            if not free:
                continue
            d["screen"] = free[0]
            used.add(free[0])
        out.append((d["screen"], h, d))
    return out


def find_display(layout, display_id):
    for n, host, d in remote_displays(layout):
        if d.get("id") == display_id:
            return n, host, d
    raise RuntimeError(f"no remote display {display_id!r}")


def stream_size(w, h):
    """A stream's size in pixels, rounded down to even numbers: hosts' encoders (NVENC's 4:2:0)
    send no video at all for an odd width or height, and the stream reconnects forever."""
    return max(2, int(w) // 2 * 2), max(2, int(h) // 2 * 2)


def remote_size(d):
    """A remote display's size in VR (width, height) in metres."""
    w, h = d.get("size", [2560, 1440])
    m = float(d.get("metres", max(1.0, w / REMOTE_PIXELS_PER_METRE)))
    return m, m * h / w


def plan_remotes(layout, count, displays):
    """The remote displays' poses in the head frame (as plan()): their own place, or else a
    row above the screens, hinged like the arc preset."""
    out = [None] * len(displays)
    for k, (_, _, d) in enumerate(displays):
        if "pos" in d:
            out[k] = {"pos": tuple(d["pos"]), "face": tuple(d.get("face", yaw_pitch(d["pos"]))),
                      "roll": float(d.get("roll", 0))}
    free = [k for k in range(len(displays)) if out[k] is None and not displays[k][2].get("off")]
    if not free:
        return out
    p = layout["preset"]
    dist = max(0.3, float(p.get("distance", 2.0)))
    gap = max(0.0, float(p.get("gap", 0.05)))
    span = lambda m, at: 2 * math.degrees(math.atan(m / 2 / at))
    top = 0.0
    for i, t in enumerate(plan(layout, count)):
        x, y, z = t["pos"]
        at = max(0.3, math.sqrt(x * x + y * y + z * z))
        top = max(top, math.degrees(math.atan2(y, math.hypot(x, z))) + span(screen_size(layout, i)[1], at) / 2)
    sizes = [remote_size(displays[k][2]) for k in free]
    pitch = top + span(gap, dist) + max(span(h, dist) for _, h in sizes) / 2
    cp, sp = math.cos(math.radians(pitch)), math.sin(math.radians(pitch))
    for k, (x, z, yaw) in zip(free, _chain([w for w, _ in sizes], dist, gap)):
        out[k] = {"pos": (x * cp, math.hypot(x, z) * sp, z * cp), "face": (yaw, pitch), "roll": 0.0}
    return out


def remotes_running(sock):
    """ft-screens' remote screens, {number: state}; None from an ft-screens without them.
    Raises NoAnswer when ft-screens isn't running: that's no sign of an old build."""
    try:
        fields = sock.ask("remotes").split()[2:]
    except NoAnswer:
        raise
    except RuntimeError:
        return None
    return {int(e.split(":")[0]): e.split(":")[2] for e in fields}


def display_target(host, d):
    """Where ft-stream reaches a display: the host's address, with its instance's port when it
    has one of its own (HOST:PORT)."""
    port = d.get("port")
    return f"{host['address']}:{int(port)}" if port and int(port) != GAMESTREAM_PORT else host["address"]


def start_remote(sock, n, host, d):
    """Its stream (ft-screens starts it over only if the settings changed). ft-screens'
    reply: "ok restored" when its panel is back where it was before a disconnect."""
    w, h = stream_size(*d.get("size", [2560, 1440]))
    label = f"{host.get('name') or host['address']}: {d.get('label') or d['id']}"
    return sock.ask(f"remote {n} start {d['client']} {display_target(host, d)} {d['app']} {int(w)}x{int(h)} "
                    f"{int(d.get('fps', 60))} {int(d.get('bitrate', 0))} {remote_size(d)[0]:.3f} {label}")


def place_remote(sock, n, d, t, eye, heading):
    center = tuple(e + v for e, v in zip(eye, turn_yaw(t["pos"], heading)))
    sock.ask(f"width {n} {remote_size(d)[0]:.4f}")
    sock.ask(f"curve {n} {float(d.get('curve', 0)):.3f}")
    sock.ask("place %d %.4f %.4f %.4f %.3f %.3f %.3f" % (n, *center, t["face"][0] + heading, t["face"][1], t["roll"]))
    pin = d.get("pin")
    if pin and len(pin.get("rel", [])) == 12:
        try:
            sock.ask(f"pin {n} {pin['hand']} " + " ".join(f"{v:.5f}" for v in pin["rel"]))
        except RuntimeError as e:
            log(f"remote display {d['id']}: {e}")  # that controller isn't on
    sock.ask(f"{'conceal' if d.get('hidden') else 'reveal'} {n}")


def host_answers(host, timeout=1.5):
    """Whether a host answers, at its address or its Steam Link dongle's (as its route allows):
    Vibepollo on its Web UI port, Sunshine instances on any of their displays' ports."""
    route = host.get("route", "auto")
    addresses = ([] if route == "dongle" else [host.get("address")]) + \
        ([] if route == "network" else list(host.get("direct", [])))
    ports = sorted({int(d["port"]) for d in host.get("displays", []) if d.get("port")}) or [WEB_UI_PORT]
    for address in filter(None, addresses):
        for port in ports:
            try:
                with socket.create_connection((address, port), timeout=timeout):
                    return True
            except OSError:
                pass
    return False


def profile_remotes(layout, name):
    """Remote displays the profile has a place for go there; the others stay where they are."""
    remote = layout.get("profiles", {}).get(name, {}).get("remote", {})
    for _, _, d in remote_displays(layout):
        if d.get("id") in remote:
            for k in SPATIAL:
                d.pop(k, None)
            d.update(json.loads(json.dumps({k: v for k, v in remote[d["id"]].items() if k in SPATIAL})))


def open_remotes(layout, name):
    """Connect the remote displays profile `name` was saved with (in the layout: apply starts
    their streams), each whose host answers now. A host that doesn't is skipped, and its
    displays stay disconnected."""
    remote = layout.get("profiles", {}).get(name, {}).get("remote", {})
    wanted = [(h, d) for _, h, d in remote_displays(layout) if d.get("id") in remote and d.get("off")]
    hosts = list({id(h): h for h, _ in wanted}.values())
    if not hosts:
        return
    with concurrent.futures.ThreadPoolExecutor(len(hosts)) as pool:
        up = dict(zip(map(id, hosts), pool.map(host_answers, hosts)))
    for h, d in wanted:
        if up[id(h)]:
            d.pop("off", None)
            log(f"remote display {d['id']}: connecting (profile {name!r})")
        else:
            log(f"remote display {d['id']}: {h.get('name') or h['address']} isn't answering; skipped")


def start_remotes(sock, layout, only=None):
    """Start the remote displays' streams (or just the ids in `only`; ft-screens leaves one
    running with the same settings alone), and stop the streams of ones no longer listed or
    disconnected ("off"). They start without a head pose too: placing them waits for one
    (place_remotes)."""
    displays = remote_displays(layout)
    running = remotes_running(sock)
    if running is None:
        if displays:
            log("remote displays: this ft-screens can't show them (build it again)")
        return False
    if only is None:
        for n in set(running) - {n for n, _, d in displays if not d.get("off")}:
            sock.ask(f"remote {n} stop")
    for n, host, d in displays:
        if d.get("off"):
            continue
        if only is None or d.get("id") in only:
            try:
                start_remote(sock, n, host, d)
            except RuntimeError as e:
                log(f"remote display {d.get('id')}: {e}")
    return True


def place_remotes(sock, layout, count, eye, heading, only=None):
    displays = remote_displays(layout)
    for (n, host, d), t in zip(displays, plan_remotes(layout, count, displays)):
        if d.get("off"):
            continue  # disconnected: no panel
        if only is None or d.get("id") in only:
            try:
                place_remote(sock, n, d, t, eye, heading)
            except RuntimeError as e:
                log(f"remote display {d.get('id')}: {e}")
    on = [d for _, _, d in displays if not d.get("off") and (only is None or d.get("id") in only)]
    if on:
        log(f"arranged {len(on)} remote display(s)")


def capture_remotes(sock, layout, eye, heading):
    """Where the remote displays are now (each one running) into their entries."""
    for n, _, d in remote_displays(layout):
        try:
            g = parse_get(sock.ask(f"get {n}"))
        except RuntimeError:
            continue  # not running
        d.update(relative_pose(g["center"], g["x"], g["z"], eye, heading))
        d["metres"] = round(g["metres"], 4)
        d["curve"] = round(g["curve"], 3)
        d.pop("pin", None)
        if "rel" in g:
            d["pin"] = {"hand": g["hand"], "rel": g["rel"]}


def in_container():
    return os.path.exists("/run/.containerenv")


def run_stream(*args, timeout=60, stdin=None):
    """ft-stream, in the container ft-screens runs in: "dev" for a clone, the release's own for a
    release (scripts/in-box). A missing "dev" made distrobox ask whether to create it, and the
    question hung until the timeout. `stdin`: text for it to read (a --login)."""
    cmd = [STREAM, *args]
    if not in_container():
        cmd = [os.path.join(REPO, "scripts", "in-box"), *cmd]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, input=stdin)
    return r.returncode, r.stdout, r.stderr


def slug(text):
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-") or "display"


def remote_anchor(sock, layout):
    """(eye, heading) to place remote displays from: the head, or with no head pose (the
    headset off), where the last arrangement was made from, worked out from where screen 1
    is and where the layout puts it."""
    try:
        f = sock.ask("head").split()
        return tuple(map(float, f[1:4])), float(f[4])
    except RuntimeError:
        if not native_count(layout):  # streams only: no screen 1 to work it out from
            anchor = stored_anchor()
            if anchor is None:
                raise RuntimeError("no head pose, and nothing arranged yet")
            return anchor
        anchor = inferred_anchor(layout, parse_get(sock.ask("get 1")))
        if anchor is None:
            raise RuntimeError("no head pose, and screen 1 is pinned")
        return anchor


def start_remotes_now(layout, only):
    """Start and place remote displays now, if the desktop runs."""
    try:
        sock = screens_socket()
        if not start_remotes(sock, layout, only):
            return
        eye, heading = remote_anchor(sock, layout)
        place_remotes(sock, layout, native_count(layout), eye, heading, only=only)
    except NoAnswer as e:
        log(f"saved; not started now: {e}")
    except RuntimeError as e:
        log(f"not placed now: {e}")


def remote_add_save(layout, host_name, address, port, d):
    """remote add's new display d, saved under its host (added when it's new)."""
    host = next((h for h in layout.setdefault("hosts", []) if h.get("name") == host_name), None)
    if host is None:
        host = {"name": host_name, "address": address, "displays": []}
        layout["hosts"].append(host)
    host["address"] = address
    if port:
        d["port"] = port
    host["displays"].append(d)
    remote_displays(layout)  # gives it a screen number
    save_layout(layout)


def remote_command(args):
    """ft-layout remote list|monitors|add|set|connect|disconnect|remove (see the usage)."""
    what = args[0] if args else ""
    if what == "list" and len(args) == 1:
        layout = load_layout()
        try:
            running = remotes_running(screens_socket()) or {}
        except RuntimeError:
            running = {}
        for n, host, d in remote_displays(layout):
            w, h = d.get("size", [2560, 1440])
            state = "disconnected" if d.get("off") else running.get(n, "off")
            state += f" route {host.get('route', 'auto')}{' mic' if host.get('mic') else ''}"
            state += " input" if host.get("input") else ""
            print(f"{n} {d['id']} {host.get('name')} {display_target(host, d)} {d['app']} {w}x{h}@{d.get('fps', 60)} "
                  f"{state}{' hidden' if d.get('hidden') else ''}  {d.get('label', '')}")
        return
    if what == "host" and len(args) >= 3:
        layout = load_layout()
        host = next((h for h in layout.get("hosts", []) if h.get("name") == args[1]), None)
        if host is None:
            raise RuntimeError(f"no host called {args[1]!r}")
        restart = False
        for kv in args[2:]:
            k, _, v = kv.partition("=")
            if k == "route" and v in ("auto", "network", "dongle"):
                host["route"] = v
                restart = True
            elif k == "direct" and (v == "none" or re.fullmatch(r"[A-Za-z0-9.:-]+(,[A-Za-z0-9.:-]+)*", v)):
                host["direct"] = [] if v == "none" else v.split(",")
                restart = True
            elif k == "mic" and v in ("on", "off"):
                host["mic"] = v == "on"  # the stream playing its sound looks every 3 s
            elif k == "input" and v in ("on", "off"):
                host["input"] = v == "on"  # its streams look every 3 s
            else:
                raise RuntimeError(f"remote host: {kv!r}? (route=auto|network|dongle direct=ADDRESS[,ADDRESS]|none "
                                   "mic=on|off input=on|off)")
        save_layout(layout)
        if not restart:
            return
        # Its streams start over the new way, their panels where they are (ft-screens keeps
        # their places across a stop in the same run).
        try:
            sock = screens_socket()
            running = remotes_running(sock) or {}
            for n, h, d in remote_displays(layout):
                if h is host and n in running and not d.get("off"):
                    sock.ask(f"remote {n} stop")
                    start_remote(sock, n, h, d)
                    log(f"{d['id']}: starting over")
        except RuntimeError as e:
            log(f"saved; not applied now: {e}")
        return
    if what in ("connect", "disconnect") and len(args) >= 2:
        layout = load_layout()
        found = [find_display(layout, i) for i in args[1:]]
        for _, _, d in found:
            if what == "connect":
                d.pop("off", None)
            else:
                d["off"] = True
        save_layout(layout)
        if what == "connect":
            # Back where it was if ft-screens still knows (this run); else placed like the
            # others, from where you look now.
            try:
                sock = screens_socket()
                unplaced = {d["id"] for n, host, d in found if start_remote(sock, n, host, d) != "ok restored"}
            except RuntimeError as e:
                log(f"saved; not started now: {e}")
                return
            for _, _, d in found:
                log(f"connected {d['id']}" + ("" if d["id"] in unplaced else " (where it was)"))
            if unplaced:
                try:
                    eye, heading = remote_anchor(sock, layout)
                    place_remotes(sock, layout, native_count(layout), eye, heading, only=unplaced)
                except RuntimeError as e:
                    log(f"not placed now: {e}")
            return
        try:
            sock = screens_socket()
            for n, _, d in found:
                sock.ask(f"remote {n} stop")
                log(f"disconnected {d['id']}")
        except RuntimeError as e:
            log(f"saved; not stopped now: {e}")
        return
    if what == "monitors" and len(args) == 2:
        code, out, err = run_stream("monitors", args[1])
        if code:
            raise RuntimeError(err.strip() or out.strip() or "ft-stream monitors failed")
        print(out.strip())
        return
    if what == "add" and len(args) >= 4:
        host_name, address, app = args[1], args[2], args[3]
        rest = args[4:]
        login = "--login" in rest  # the Web UI login on stdin: a Sunshine instance's pairing
        rest = [a for a in rest if a != "--login"]
        opts = dict(zip(rest[0::2], rest[1::2]))
        unknown = set(opts) - {"--size", "--fps", "--bitrate", "--label", "--client", "--port"}
        if unknown or len(rest) % 2:
            raise RuntimeError("remote add: options are --size WxH --fps F --bitrate KBPS --label TEXT --client NAME "
                               "--port PORT --login")
        if not re.fullmatch(r"[A-Za-z0-9.:-]+", address) or not re.fullmatch(r"[A-Za-z0-9{}._:-]+", app):
            raise RuntimeError("remote add: the address or app has other characters")
        port = opts.get("--port", "")
        if port and not (port.isdigit() and 1024 < int(port) < 65535):
            raise RuntimeError("remote add: --port is a port number")
        port = int(port) if port else 0
        # Every option is checked here, before pairing: a bad one leaves no client on the host.
        for k in ("--fps", "--bitrate"):
            if k in opts and not opts[k].isdigit():
                raise RuntimeError(f"remote add: {k} is a whole number, not {opts[k]!r}")
        fps, bitrate = int(opts.get("--fps", 60)), int(opts.get("--bitrate", 0))
        if "--size" in opts and not re.fullmatch(r"\d+x\d+", opts["--size"].lower()):
            raise RuntimeError(f"remote add: --size is WxH, not {opts['--size']!r}")
        layout = load_layout()
        displays = remote_displays(layout)
        if len(displays) >= REMOTE_MAX:
            raise RuntimeError(f"at most {REMOTE_MAX} remote displays")
        label = opts.get("--label") or ("Virtual" if app == "monitor" else app.split(":", 1)[-1])
        ids = {d["id"] for _, _, d in displays}
        display_id = base = slug(f"{host_name}-{label}")
        for k in range(2, 100):
            if display_id not in ids:
                break
            display_id = f"{base}-{k}"
        client = opts.get("--client") or f"frametop-{display_id}"
        if not re.fullmatch(r"[A-Za-z0-9._-]+", client):
            raise RuntimeError("remote add: the client name has other characters")
        size = [2560, 1440]
        if "--size" in opts:
            size = list(stream_size(*(int(v) for v in opts["--size"].lower().split("x"))))
        elif app.startswith("display:"):  # the monitor's own resolution
            code, out, _ = run_stream("monitors", address)
            try:
                for m in (json.loads(out[out.index("["):]) if code == 0 else []):
                    if m.get("device_id") == app[8:]:
                        res = m["info"]["resolution"]
                        size = [int(res["width"]), int(res["height"])]
            except (ValueError, KeyError, TypeError):
                pass
        target = f"{address}:{port}" if port else address
        if login:
            code, out, err = run_stream("pair", target, "--id", client, "--login", timeout=30, stdin=sys.stdin.read())
        else:
            code, out, err = run_stream("pair", target, "--id", client, timeout=30)
        if code:
            raise RuntimeError(f"pairing {client} with {address} failed: {(err or out).strip()}")
        log(out.strip().splitlines()[-1] if out.strip() else f"paired {client}")
        try:
            remote_add_save(layout, host_name, address, port, {
                "id": display_id, "client": client, "app": app, "label": label, "size": size, "fps": fps,
                "bitrate": bitrate, "metres": round(max(1.0, size[0] / REMOTE_PIXELS_PER_METRE), 3)})
        except BaseException:
            # Nothing saved: the client just paired would be left on the host with no display.
            code, out, err = run_stream("unpair", target, "--id", client, timeout=30)
            if code:
                log(f"unpairing {client} from {address} failed too: {(err or out).strip()}")
            else:
                log(f"unpaired {client}: the display wasn't saved")
            raise
        print(display_id)
        start_remotes_now(layout, {display_id})
        return
    if what == "set" and len(args) >= 3:
        layout = load_layout()
        n, host, d = find_display(layout, args[1])
        for kv in args[2:]:
            k, _, v = kv.partition("=")
            if k == "size" and re.fullmatch(r"\d+x\d+", v.lower()):
                d["size"] = list(stream_size(*(int(x) for x in v.lower().split("x"))))
            elif k in ("fps", "bitrate") and v.isdigit():
                d[k] = int(v)
            elif k == "label" and v:
                d["label"] = v
            elif k == "metres" and re.fullmatch(r"\d+(\.\d+)?", v) and 0.15 <= float(v) <= 12:
                d["metres"] = float(v)
            elif k == "app" and re.fullmatch(r"[A-Za-z0-9{}._:-]+", v):
                d["app"] = v
            else:
                raise RuntimeError(f"remote set: {kv!r}? (size=WxH fps=F bitrate=KBPS label=TEXT app=APP metres=M)")
        save_layout(layout)
        if d.get("off"):
            return  # disconnected: the new settings are for its next connection
        try:
            start_remote(screens_socket(), n, host, d)  # starts over with the new settings
        except RuntimeError as e:
            log(f"saved; not applied now: {e}")
        return
    if what == "remove" and len(args) == 2:
        layout = load_layout()
        n, host, d = find_display(layout, args[1])
        try:
            screens_socket().ask(f"remote {n} stop")
            time.sleep(2)  # its stream releases a Remote Monitor on the way out
        except RuntimeError:
            pass
        host["displays"].remove(d)  # the host stays (Remote Displays removes hosts)
        for profile in layout.get("profiles", {}).values():
            profile.get("remote", {}).pop(d["id"], None)
        save_layout(layout)
        code, out, err = run_stream("unpair", display_target(host, d), "--id", d["client"], timeout=30)
        said = (out or err).strip().splitlines()
        log(f"removed {d['id']}; {said[-1] if said else f'unpair exited {code}'}")
        return
    raise RuntimeError("remote list | monitors ADDRESS | add HOST ADDRESS APP [options] | set ID KEY=VALUE... | "
                       "host NAME route=...|direct=...|mic=...|input=... | connect ID... | disconnect ID... | remove ID")


def apply(wait=0):
    return apply_screens(wait) if backend() == "screens" else apply_gamescope(wait)


def capture():
    """Where the screens are now as the custom layout. It's no longer a named one's until
    `save NAME` (placed by hand since)."""
    screens = capture_screens() if backend() == "screens" else capture_gamescope()
    layout = load_layout()
    if layout.pop("active", None) is not None:
        save_layout(layout)
    return screens


# ---------------------------------------------------------------- named layouts

def layout_names(layout):
    return sorted(layout.get("layouts", {}), key=str.casefold)


def check_name(name):
    name = " ".join(name.split())
    if not name or len(name) > 40:
        raise RuntimeError("a layout's name needs 1 to 40 characters")
    return name


def save_named(layout, name):
    """The custom arrangement (as captured) under `name`, replacing one of that name, and
    in use."""
    name = check_name(name)
    screens = [s for s in layout.get("screens", [])[:screen_count(layout)] if "pos" in s]
    if not screens:
        raise RuntimeError("nothing to save: no arrangement captured")
    layout.setdefault("layouts", {})[name] = [{k: s[k] for k in SPATIAL if k in s} for s in screens]
    # The remote displays connected now, like the apps open now.
    remote = {d["id"]: {k: d[k] for k in SPATIAL if k in d} for _, _, d in remote_displays(layout)
              if not d.get("off") and "pos" in d}
    profile = layout.setdefault("profiles", {}).setdefault(name, {})
    if remote:
        profile["remote"] = remote
    else:
        profile.pop("remote", None)
    layout["mode"], layout["active"] = "custom", name
    return name


def use_named(layout, name):
    """Make a named layout the custom arrangement (not arranged yet). A layout saved with
    fewer screens leaves the others where the preset would put them, or where they were
    saved last; one saved with more keeps its extra screens for later."""
    saved = layout.get("layouts", {}).get(name)
    if saved is None:
        raise RuntimeError(f"no layout called {name!r}")
    count = screen_count(layout)
    preset = plan(dict(layout, mode="preset"), count)
    screens = layout.setdefault("screens", [])
    while len(screens) < count:
        screens.append({})
    for i in range(count):
        if i < len(saved):
            for k in SPATIAL:
                screens[i].pop(k, None)
            screens[i].update(json.loads(json.dumps(saved[i])))
        elif "pos" not in screens[i]:
            screens[i].update({"pos": list(preset[i]["pos"]), "face": list(preset[i]["face"]),
                               "roll": preset[i]["roll"]})
    profile_remotes(layout, name)
    layout["mode"], layout["active"] = "custom", name


def rename_named(layout, old, new):
    new = check_name(new)
    named = layout.get("layouts", {})
    if old not in named:
        raise RuntimeError(f"no layout called {old!r}")
    if new != old and new in named:
        raise RuntimeError(f"there's already a layout called {new!r}")
    named[new] = named.pop(old)
    profiles = layout.get("profiles", {})
    if old in profiles:
        profiles[new] = profiles.pop(old)
    if layout.get("default_profile") == old:
        layout["default_profile"] = new
    if layout.get("active") == old:
        layout["active"] = new
    return new


def delete_named(layout, name):
    """The screens stay where the layout put them, as an unnamed custom arrangement."""
    if layout.get("layouts", {}).pop(name, None) is None:
        raise RuntimeError(f"no layout called {name!r}")
    layout.get("profiles", {}).pop(name, None)
    if layout.get("default_profile") == name:
        layout.pop("default_profile")
    if layout.get("active") == name:
        layout.pop("active")


# ---------------------------------------------------------------- profiles (docs/profiles.md)

def ask_float(text, timeout=6.0):
    """ft-floatd (floating windows, in the desktop's session); None if it isn't running."""
    return ftipc.ask(FLOAT, text, timeout)


def capture_profile(layout, name):
    """The desktop's apps and hidden screens into profile `name` (kept as they were if
    ft-floatd doesn't answer)."""
    hidden = [i + 1 for i in range(screen_count(layout)) if screen_entry(layout, i).get("hidden")]
    profile = layout.setdefault("profiles", {}).setdefault(name, {})
    profile["hidden"] = hidden
    remote = profile.get("remote", {})  # the ones connected (save_named)
    for _, _, d in remote_displays(layout):
        if d["id"] in remote:
            remote[d["id"]]["hidden"] = bool(d.get("hidden"))
    reply = ask_float("windows")
    if reply and reply.startswith("ok "):
        profile["windows"] = json.loads(reply[3:])
        log(f"profile {name!r}: {len(profile['windows'])} windows, hidden screens {hidden or 'none'}")
    else:
        log(f"profile {name!r}: the apps weren't saved ({reply or 'ft-floatd is not running'})")


def use_hidden(layout, name):
    """A profile's hidden screens as the screens' own setting (applied with the arrangement)."""
    profile = layout.get("profiles", {}).get(name)
    if profile is None:
        return
    hidden = set(profile.get("hidden", []))
    screens = layout.setdefault("screens", [])
    for i in range(screen_count(layout)):
        while len(screens) <= i:
            screens.append({})
        if i + 1 in hidden:
            screens[i]["hidden"] = True
        else:
            screens[i].pop("hidden", None)
    remote = profile.get("remote", {})
    for _, _, d in remote_displays(layout):
        if "hidden" in remote.get(d.get("id"), {}):
            if remote[d["id"]]["hidden"]:
                d["hidden"] = True
            else:
                d.pop("hidden", None)


def open_apps(name, wait=0):
    """Have ft-floatd open a profile's apps (waiting up to `wait` seconds for it to start)."""
    if not load_layout().get("profiles", {}).get(name, {}).get("windows"):
        return
    deadline = time.time() + wait
    while True:
        reply = ask_float(f"profile {name}")
        if reply is not None or time.time() >= deadline:
            break
        time.sleep(1)
    log(f"apps: {reply or 'ft-floatd is not running'}")


def launcher_name(name):
    slug = re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-") or "profile"
    return f"frametop-profile-{slug}.desktop"


def write_launchers(layout):
    """A launcher entry for each profile (SteamVR's Launch a program list, the Application
    Launcher, KRunner), and none for ones that are gone."""
    os.makedirs(LAUNCHERS, exist_ok=True)
    want = {}
    for name in layout_names(layout):
        # Quoted for Exec (\" \` \$ \\), then each backslash doubled for the key file.
        quoted = ('"' + re.sub(r'(["`$\\])', r"\\\1", name) + '"').replace("\\", "\\\\")
        want[launcher_name(name)] = "\n".join([
            "[Desktop Entry]", "Type=Application", f"Name=Frametop: {name}",
            "Comment=Open the Frametop desktop in this profile: its screens and apps",
            f"Exec={os.path.join(REPO, 'layout', 'ft-layout')} open {quoted}",
            "Icon=preferences-desktop-display", "Categories=Utility;", "X-Frametop-Profile=true", ""])
    for f in os.listdir(LAUNCHERS):
        if f.startswith("frametop-profile-") and f.endswith(".desktop") and f not in want:
            os.remove(os.path.join(LAUNCHERS, f))
    for f, text in want.items():
        path = os.path.join(LAUNCHERS, f)
        try:
            with open(path) as old:
                if old.read() == text:
                    continue
        except OSError:
            pass
        with open(path, "w") as out:
            out.write(text)


def start_profile(layout):
    """The profile the desktop starts with: FT_PROFILE, else default_profile."""
    name = os.environ.get("FT_PROFILE") or layout.get("default_profile")
    return name if name and name in layout.get("layouts", {}) else None


def desktop_running():
    try:
        screens_socket().ask("screens", timeout=2)
        return True
    except RuntimeError:
        return False


def run_session(setenv, what):
    """The session script in its own unit, frametop-desktop (as desktops.sh start does), with
    setenv's NAME=VALUE settings."""
    if subprocess.run(["pgrep", "-x", "vrcompositor"], capture_output=True).returncode != 0:
        raise RuntimeError("SteamVR isn't running")
    subprocess.run(["systemctl", "--user", "reset-failed", "frametop-desktop"], capture_output=True)
    session = os.path.join(REPO, "session", "frametop-session.sh")
    r = subprocess.run(["systemd-run", "--user", "--collect", "--quiet", "--unit", "frametop-desktop"] +
                       [f"--setenv={e}" for e in setenv] +
                       ["bash", "-c", f'exec "{session}" > /tmp/frametop-session.log 2>&1'],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"couldn't start {what}: {r.stderr.strip()}")


def start_desktop(profile):
    """Start the Frametop desktop in a profile (as desktops.sh start does)."""
    run_session([f"FT_PROFILE={profile}"], "the desktop")
    log(f"starting the desktop in {profile!r}")


def session_running():
    """Whether a Frametop session runs or is starting: the desktop (either backend) or streams
    only. Its unit is there before ft-screens is (the container takes seconds to start)."""
    return any(subprocess.run(argv, capture_output=True).returncode == 0 for argv in (
        ["pgrep", "-x", "ft-screens"], ["pgrep", "-f", "[v]r-overlay-key frametop "],
        ["systemctl", "--user", "is-active", "--quiet", "frametop-desktop"]))


def streams_command(args):
    """ft-layout streams start|stop: the remote displays without the desktop."""
    if args == ["start"]:
        if backend() != "screens":
            raise RuntimeError("streaming without the desktop needs the screens backend (BACKEND=screens)")
        if session_running():
            log("streams only" if streams_only() else "the Frametop desktop is running: its remote displays stream there")
            return 0
        run_session(["FT_STREAMS_ONLY=1"], "the streams")
        log("streaming the connected remote displays without the desktop")
        return 0
    if args == ["stop"]:
        if not streams_only():
            # Never the desktop: stopping it closes its windows.
            log("not streaming without the desktop" + (" (the Frametop desktop is running)" if session_running() else ""))
            return 0
        r = subprocess.run(["systemctl", "--user", "stop", "frametop-desktop"], capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(f"couldn't stop the streams: {r.stderr.strip()}")
        log("stopped streaming")
        return 0
    log("usage: ft-layout streams start|stop")
    return 2


# ---------------------------------------------------------------- KWin (scale, positions, primary)

def nested_env():
    """Environment of the running Frametop Plasma session (its private bus and runtime dir)."""
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            with open(f"/proc/{pid}/comm") as f:
                if f.read().strip() != "plasmashell":
                    continue
            with open(f"/proc/{pid}/environ", "rb") as f:
                env = dict(e.split("=", 1) for e in f.read().decode(errors="replace").split("\0") if "=" in e)
        except OSError:
            continue
        if env.get("XDG_RUNTIME_DIR", "").endswith("/frametop"):
            keep = ("DBUS_SESSION_BUS_ADDRESS", "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR", "XDG_CONFIG_HOME")
            return dict(os.environ, **{k: env[k] for k in keep if k in env})
    return None


def outputs(env):
    """KWin's outputs for the screens, in screen order (WL-0, WL-1, ...)."""
    try:
        data = json.loads(subprocess.run(["kscreen-doctor", "-j"], capture_output=True, text=True, env=env,
                                         timeout=10).stdout)
    except (OSError, ValueError, subprocess.TimeoutExpired):
        return []
    outs = [o for o in data.get("outputs", []) if o.get("connected")]
    if backend() == "screens":
        # Outputs after the screens are spares for floating windows (ft-floatd places them).
        count = screen_count()
        outs = [o for o in outs if not re.fullmatch(r"WL-(\d+)", o.get("name", "")) or
                int(o["name"][3:]) < count]
    return sorted(outs, key=lambda o: [int(t) if t.isdigit() else t for t in re.split(r"(\d+)", o.get("name", ""))])


KSCREEN_ROTATION = {1: "normal", 2: "left", 4: "inverted", 8: "right"}  # kscreen-doctor -j "rotation"


def arrangement(count):
    """The screens as you see them from where you are: columns left to right, each top to
    bottom (0-based screen indices), for KWin's output positions. Screens pinned to a
    wrist come last. None when there's nothing to go by (gamescope, no ft-screens)."""
    if backend() != "screens":
        return None
    try:
        sock = screens_socket()
        f = sock.ask("head").split()
        eye, heading = tuple(map(float, f[1:4])), float(f[4])
        gets = [parse_get(sock.ask(f"get {i + 1}")) for i in range(count)]
    except (RuntimeError, ValueError, IndexError):
        return None
    seen, pinned = [], []
    for i, g in enumerate(gets):
        if g["hand"] != "none":
            pinned.append(i)
            continue
        rel = turn_yaw(tuple(c - e for c, e in zip(g["center"], eye)), -heading)
        yaw, pitch = yaw_pitch(rel)
        half = math.degrees(math.atan2(g["metres"] / 2, max(0.1, math.sqrt(dot(rel, rel)))))
        seen.append({"i": i, "x": -yaw, "pitch": pitch, "half": half})  # x grows to the right
    seen.sort(key=lambda b: b["x"])
    columns = []
    for b in seen:
        # One above the other: centres closer sideways than half the narrower screen.
        if columns and abs(b["x"] - columns[-1][-1]["x"]) < min(b["half"], columns[-1][-1]["half"]):
            columns[-1].append(b)
        else:
            columns.append([b])
    return [[b["i"] for b in sorted(c, key=lambda b: -b["pitch"])] for c in columns] + [[i] for i in pinned]


def send_scales(outs):
    """KWin's scale for each screen, as it is now, to ft-screens: KWin's nested backend
    doesn't undo its scale on pointer input, so ft-screens does (panel pixels / scale)."""
    if backend() != "screens":
        return
    try:
        sock = screens_socket()
        for i, o in enumerate(outs):
            reply = sock.ask(f"scale {i + 1} {float(o.get('scale', 1)):g}")
            if not reply.startswith("ok"):
                log(f"screen {i + 1}: scale: {reply}")
    except RuntimeError as e:
        log(f"scale: {e}")


def logical_size(o):
    """An output's size in logical units. kscreen's "size" is in pixels (already turned for a
    rotation); positions are logical, the pixels divided by the scale (KWin rounds up)."""
    s = float(o.get("scale", 1))
    return (math.ceil(o["size"]["width"] / s - 1e-6), math.ceil(o["size"]["height"] / s - 1e-6))


def apply_scales():
    """Per-screen scale and rotation, positions side by side, and the primary screen (the
    taskbar goes there) to KWin, which keeps them in the session's config."""
    env = nested_env()
    if not env:
        raise RuntimeError("the Frametop desktop isn't running")
    layout = load_layout()
    args = []
    outs = outputs(env)
    for i, o in enumerate(outs):
        s = screen_scale(layout, i)
        if abs(float(o.get("scale", 1)) - s) > 1e-3:
            args.append(f"output.{o['id']}.scale.{s:g}")
        rot = screen_rotation(layout, i)
        if KSCREEN_ROTATION.get(o.get("rotation"), "normal") != rot:
            args.append(f"output.{o['id']}.rotation.{rot}")
    if outs:
        p = outs[min(primary_screen(layout), len(outs) - 1)]
        if p.get("priority") != 1:
            args.append(f"output.{p['id']}.priority.1")
    if args:
        subprocess.run(["kscreen-doctor", *args], capture_output=True, env=env, timeout=20)
    # Laid out as you see the screens around you (arrangement), centred on one line, so
    # the pointer and dragged windows cross to the screen you see next to this one.
    outs = outputs(env)
    send_scales(outs)
    sizes = [logical_size(o) for o in outs if o.get("size")]
    if len(sizes) == len(outs) and outs:
        columns = arrangement(len(outs))
        if not columns or sorted(i for c in columns for i in c) != list(range(len(outs))):
            # Nothing to go by (no head pose with the headset off, say): keep KWin's order.
            columns = [[i] for i in sorted(range(len(outs)), key=lambda i: (outs[i].get("pos", {}).get("x", 0),
                                                                            outs[i].get("pos", {}).get("y", 0)))]
        widths = [max(sizes[i][0] for i in c) for c in columns]
        heights = [sum(sizes[i][1] for i in c) for c in columns]
        tallest, x, moves = max(heights), 0, []
        for c, cw, ch in zip(columns, widths, heights):
            y = (tallest - ch) // 2
            for i in c:
                want = (x + (cw - sizes[i][0]) // 2, y)
                y += sizes[i][1]
                o = outs[i]
                if (o.get("pos", {}).get("x"), o.get("pos", {}).get("y")) != want:
                    moves.append(f"output.{o['id']}.position.{want[0]},{want[1]}")
            x += cw
        if moves:
            subprocess.run(["kscreen-doctor", *moves], capture_output=True, env=env, timeout=20)
            args += moves
    return args


def kwin_follow():
    """KWin's outputs after the screens moved, if the desktop is up."""
    try:
        changes = apply_scales()
        log("kwin: " + (" ".join(changes) if changes else "unchanged"))
    except RuntimeError as e:
        log(f"kwin: {e}")


def remote_view():
    """Where the primary screen sits in the workspace (all screens' bounding box), in
    logical units: "x y width height workspace_width workspace_height". The VNC bridge
    (session/vnc-bridge.sh) shows that part of krdp's workspace stream."""
    env = nested_env()
    if not env:
        raise RuntimeError("the Frametop desktop isn't running")
    outs = [o for o in outputs(env) if o.get("enabled", True) and o.get("size") and o.get("pos")]
    if not outs:
        raise RuntimeError("the Frametop desktop has no screens yet")
    rects = [(o["pos"]["x"], o["pos"]["y"], *logical_size(o)) for o in outs]
    left, top = min(r[0] for r in rects), min(r[1] for r in rects)
    right, bottom = max(r[0] + r[2] for r in rects), max(r[1] + r[3] for r in rects)
    p = next((i for i, o in enumerate(outs) if o.get("priority") == 1), 0)
    x, y, w, h = rects[p]
    return f"{x - left} {y - top} {w} {h} {right - left} {bottom - top}"


def reset_countdown():
    """Give a hand-triggered reset time to face forward, even without the optional overlay."""
    started = time.monotonic()
    try:
        if not os.access(RESET_TIMER, os.X_OK):
            raise FileNotFoundError(f"countdown helper unavailable: {RESET_TIMER}")
        # in-box cannot forward the host timeout to its container child. Give the
        # helper an absolute expiry so a late container start cannot show stale UI.
        command = [RESET_TIMER, "--deadline", f"{started + 6:.9f}"]
        if not in_container():
            command = [os.path.join(REPO, "scripts", "in-box"), *command]
        result = subprocess.run(command, timeout=6, check=False)
        if result.returncode:
            log("visible countdown unavailable; continuing the delayed reset")
    except (OSError, subprocess.TimeoutExpired) as e:
        log(f"visible countdown unavailable: {e}; continuing the delayed reset")
    remaining = 3 - (time.monotonic() - started)
    if remaining > 0:
        time.sleep(remaining)


def main(argv):
    if len(argv) < 2 or argv[1] in ("-h", "--help"):
        print(__doc__.split("Usage")[1].split("\n", 1)[1])
        return 0 if len(argv) >= 2 else 2
    cmd = argv[1]
    try:
        if cmd == "plan":
            layout = load_layout()
            print(json.dumps(plan(layout, screen_count(layout))))
        elif cmd == "screen-args":
            print(screen_args())
        elif cmd == "remote-view":
            print(remote_view())
        elif cmd == "toggle":
            log(screens_socket().ask("toggle"))
        elif cmd in ("hide", "show") and len(argv) == 3:
            set_hidden(argv[2], cmd == "hide")
        elif cmd == "hidden":
            layout = load_layout()
            print(" ".join(str(i + 1) for i in range(screen_count(layout)) if screen_entry(layout, i).get("hidden")))
        elif cmd == "layouts":
            layout = load_layout()
            for name in layout_names(layout):
                print(("* " if name == layout.get("active") and layout.get("mode") == "custom" else "  ") + name)
        elif cmd in ("rename", "delete") and len(argv) == (4 if cmd == "rename" else 3):
            layout = load_layout()
            if cmd == "rename":
                rename_named(layout, argv[2], argv[3])
            else:
                delete_named(layout, argv[2])
            save_layout(layout)
            write_launchers(layout)
        elif cmd == "launchers":
            write_launchers(load_layout())
        elif cmd == "default" and len(argv) == 3:
            layout = load_layout()
            if argv[2] == "none":
                layout.pop("default_profile", None)
            elif argv[2] in layout.get("layouts", {}):
                layout["default_profile"] = argv[2]
            else:
                raise RuntimeError(f"no profile called {argv[2]!r}")
            save_layout(layout)
        elif cmd == "open" and len(argv) == 3:
            if argv[2] not in load_layout().get("layouts", {}):
                raise RuntimeError(f"no profile called {argv[2]!r}")
            if streams_only():
                raise RuntimeError("streaming without the desktop: stop streaming (Frametop Settings, Remote displays) to "
                                   "start the desktop in a profile")
            if desktop_running():
                return main([argv[0], "use", argv[2]])
            start_desktop(argv[2])
        elif cmd == "start":
            # Desktop start (the session script): the profile it starts with, or the arrangement.
            clear_anchor()
            layout = load_layout()
            name = start_profile(layout)
            if not name:
                return main([argv[0], "apply"] + argv[2:])
            use_named(layout, name)
            use_hidden(layout, name)
            open_remotes(layout, name)
            save_on_the_way(layout)
            log(f"starting in profile {name!r}")
            wait = float(argv[argv.index("--wait") + 1]) if "--wait" in argv else 60
            with open(LOCK_PATH, "w") as lock:
                fcntl.flock(lock, fcntl.LOCK_EX)
                try:
                    apply(wait)
                except RuntimeError as e:   # (no head pose with the headset off): the scales still go
                    log(f"not arranged now: {e}")
                if streams_only():
                    return 0  # no KWin to scale, no Plasma to open apps in
                for _ in range(30):  # Plasma may still be starting
                    try:
                        log("kwin: " + (" ".join(apply_scales()) or "unchanged"))
                        break
                    except RuntimeError as e:
                        last = e
                        time.sleep(1)
                else:
                    log(f"kwin: {last}")
            open_apps(name, wait=90)  # ft-floatd starts with Plasma
        elif cmd == "reset":
            # Keep a separate lock through the reset so repeated clicks cannot queue jumps.
            with open(LOCK_PATH + ".reset-delay", "w") as reset_lock:
                try:
                    fcntl.flock(reset_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError:
                    log("a delayed reset is already running")
                    return 1
                log("resetting the layout in 3 seconds; look straight ahead")
                reset_countdown()
                if backend() == "screens":
                    with open(LOCK_PATH, "w") as layout_lock:
                        try:
                            fcntl.flock(layout_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                        except BlockingIOError:
                            log("another ft-layout is already running")
                            return 1
                        if recenter_screens():
                            kwin_follow()  # Plasma's screen arrangement follows where they are now
                    return 0
                layout = load_layout()
                name = layout.get("active") if layout.get("mode") == "custom" else None
                return main([argv[0], "use", name] if name in layout.get("layouts", {}) else [argv[0], "apply"])
        elif cmd == "remote":
            remote_command(argv[2:])
        elif cmd == "streams":
            return streams_command(argv[2:])
        elif cmd in ("pin", "unpin") and len(argv) >= 3:
            log(screens_socket().ask(" ".join(argv[1:])))
            kwin_follow()  # pinned screens go last
        elif cmd in ("apply", "capture", "scale") or (cmd in ("save", "use") and len(argv) == 3):
            with open(LOCK_PATH, "w") as lock:
                try:
                    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError:
                    log("another ft-layout is already running")
                    return 1
                if cmd == "apply":
                    wait = float(argv[argv.index("--wait") + 1]) if "--wait" in argv else 0
                    if wait and not load_layout().get("auto", True):
                        log("auto-arrange is off")
                        if backend() == "screens":  # the visibility settings apply anyway
                            try:
                                sock = screens_socket()
                                deadline = time.time() + wait
                                while screens_up(sock) < native_count() and time.time() < deadline:
                                    time.sleep(1)
                                send_visibility(sock, load_layout())
                                if native_count():
                                    send_hidden(sock, load_layout())
                                layout = load_layout()
                                if start_remotes(sock, layout):  # streams start, arranged or not
                                    save_layout(layout)
                            except RuntimeError as e:
                                log(f"visibility: {e}")
                    else:
                        layout = load_layout()
                        name = layout.get("active") if layout.get("mode") == "custom" else None
                        if name in layout.get("profiles", {}):
                            # Arranged as the profile in use: its remote displays too.
                            profile_remotes(layout, name)
                            open_remotes(layout, name)
                            save_on_the_way(layout)
                        try:
                            apply(wait)
                        except RuntimeError as e:
                            if not wait:
                                raise
                            # Desktop start with the headset off (no head pose): KWin's scales must
                            # still reach ft-screens, or every click on a scaled screen lands at
                            # scale x its distance from the top-left (2026-10-11: 1.25, hundreds of
                            # pixels off on the native screen).
                            log(f"not arranged now: {e}")
                    if streams_only():
                        pass  # no KWin
                    elif not wait:
                        kwin_follow()
                    else:
                        # KWin keeps these, but new screens or a changed layout need them once.
                        for _ in range(30):  # Plasma may still be starting
                            try:
                                log("kwin: " + (" ".join(apply_scales()) or "unchanged"))
                                break
                            except RuntimeError as e:
                                last = e
                                time.sleep(1)
                        else:
                            log(f"kwin: {last}")
                elif cmd == "capture":
                    for i, s in enumerate(capture()):
                        log(f"screen {i + 1}: {s}")
                    kwin_follow()
                elif cmd == "save":
                    check_name(argv[2])
                    capture()
                    layout = load_layout()
                    name = save_named(layout, argv[2])
                    capture_profile(layout, name)
                    save_layout(layout)
                    write_launchers(layout)
                    log(f"saved layout {name!r}")
                    kwin_follow()
                elif cmd == "use":
                    layout = load_layout()
                    use_named(layout, argv[2])
                    use_hidden(layout, argv[2])
                    open_remotes(layout, argv[2])
                    save_layout(layout)
                    log(f"using layout {argv[2]!r}")
                    try:
                        apply()
                    except RuntimeError as e:
                        log(f"not arranged now: {e}")
                        try:  # the screens stay put, but the profile's hidden ones still hide
                            send_hidden(screens_socket(), layout)
                        except RuntimeError:
                            pass
                    else:
                        kwin_follow()
                    open_apps(argv[2])  # floating windows go relative to the screens, wherever they are
                else:
                    changes = apply_scales()
                    log("kwin: " + (" ".join(changes) if changes else "unchanged"))
        else:
            print(f"unknown command: {cmd}", file=sys.stderr)
            return 2
    except RuntimeError as e:
        log(f"error: {e}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
