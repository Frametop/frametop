#!/usr/bin/env python3
"""ft-spotwatch: spotwatch, Frametop's search for apps and remote displays (docs/spotwatch.md).

Meta+Space (the input relay's spotwatch action) opens a search pill straight ahead of where your
head points. Type to find an app or a remote display ("{host} - {display}"); Tab, Shift+Tab,
and the arrow keys move through what's found, and Enter or a click opens it:
  - an app that's running: its most recent window comes to the front. The lazy susan spins the
    panel it's on (a screen or its floating window) to straight ahead, and it gets typing. A
    pinned panel doesn't spin: it gets typing where it is, with a word on where that is.
  - an app that isn't: it starts, its first window floating (ft-floatd's launch).
  - a remote display: connected first if it isn't, then spun to.
Shift+Enter or a right click opens the item's menu: an app's own actions (a browser's private
window), Open new window, Open docked on this screen, and its windows; a display's Go to and
Disconnect. Esc goes back from a menu, and closes the search.

Runs inside the Frametop desktop's Plasma session (its autostart, like ft-floatd), so it's up
exactly while the desktop is, and starts apps with the desktop's environment. The pill is
ft-spotpanel (spotwatch/ft-spotpanel.cpp, in the container), which this runs and keeps running.
While the search is open, ft-screens sends it the typing ("spotwatch on", renewed every
second; see compositor.c), from keyboards and Frametop's keyboard alike, so nothing reaches an
app or Steam meanwhile.

Datagrams on @ft_spotwatch (--socket NAME); a sender with an address gets "ok ..." or "error":
  toggle [keyboard|mouse|left|right]   the input relay's spotwatch action, and what pressed it:
                                       from a mouse or controller, Frametop's keyboard opens too
  open [SOURCE] | close | status       (status -> "ok open|closed list|menu <query>")
  rows                                 -> "ok [titles]" (JSON): what's listed now, for tests
  key <code> <value>                   ft-screens: typing, while the search has it
  dismiss <why>                        ft-screens: something else took the typing (a click
                                       elsewhere, a pause)
  hover N | click N | menu N | back | scroll +1|-1 | outside      ft-spotpanel: the lasers and
                                       the 3D mouse on the panel (N: a row shown)

Usage counts (what you open often comes first) are in ~/.config/frametop-spotwatch.json, and
the icons, rasterized for the panel, in ~/.cache/frametop/spotwatch. Log:
/tmp/frametop-spotwatch.log.

Usage: ft-spotwatch [--socket NAME] [--panel-socket NAME] [--screens NAME] [--float NAME]
                    [--no-panel]
"""
import argparse
import ctypes
import ctypes.util
import hashlib
import json
import math
import os
import socket
import subprocess
import sys
import time

import gi

gi.require_version("GdkPixbuf", "2.0")
from gi.repository import GdkPixbuf, Gio, GLib  # noqa: E402

HERE = os.path.dirname(os.path.realpath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(REPO, "layout"))
sys.path.insert(0, HERE)
import ft_layout  # noqa: E402  (the layout: remote displays)
import spotsearch  # noqa: E402

PANEL = os.path.join(REPO, "screens", "build", "ft-spotpanel")
IN_BOX = os.path.join(REPO, "scripts", "in-box")  # runs a program in the container it was built in
FT_LAYOUT = os.path.join(REPO, "layout", "ft-layout")
STANDALONE_ACTION = "frametop-standalone"  # Launch as Standalone (float/ft_apps.py): what Enter does anyway
USAGE_PATH = os.path.expanduser("~/.config/frametop-spotwatch.json")
ICON_DIR = os.path.expanduser("~/.cache/frametop/spotwatch")
ICON_PX = 64              # ft-spotpanel's icon files: this square, RGBA
ROWS = 7                  # rows the panel shows (ft-spotpanel's kMaxRows)
RENEW_MS = 1000           # "spotwatch on" again this often while open (ft-screens' lease is 3 s)
REPEAT_DELAY_MS, REPEAT_MS = 450, 40  # a held Backspace or arrow repeats (the relay sends no repeats)
REPEATS = {14, 103, 108, 104, 109}
SHIFT, CTRL, ALT, META = {42, 54}, {29, 97}, {56, 100}, {125, 126}
KEY_ESC, KEY_ENTER, KEY_KPENTER, KEY_TAB, KEY_BACKSPACE = 1, 28, 96, 15, 14
KEY_UP, KEY_DOWN, KEY_LEFT, KEY_PAGEUP, KEY_PAGEDOWN, KEY_U = 103, 108, 105, 104, 109, 22
REMOTE_ICON = "video-display"
WINDOW_ICON = "window"


def log(*args):
    print(time.strftime("%H:%M:%S"), *args, flush=True)


def clean(text):
    """A field for the panel: no tabs or newlines (its separators)."""
    return " ".join(str(text or "").split())


# ---------------------------------------------------------------- typing (libxkbcommon)

class Xkb:
    """Key codes to text on the desktop's keyboard layout (kxkbrc), through libxkbcommon. Falls
    back to a US layout (spotsearch.US_TEXT) without it."""

    class Names(ctypes.Structure):
        _fields_ = [("rules", ctypes.c_char_p), ("model", ctypes.c_char_p), ("layout", ctypes.c_char_p),
                    ("variant", ctypes.c_char_p), ("options", ctypes.c_char_p)]

    def __init__(self):
        self.lib = self.keymap = self.state = None
        layout, variant, options = self.layout()
        try:
            lib = ctypes.CDLL(ctypes.util.find_library("xkbcommon") or "libxkbcommon.so.0")
            lib.xkb_context_new.restype = ctypes.c_void_p
            lib.xkb_context_new.argtypes = [ctypes.c_int]
            lib.xkb_keymap_new_from_names.restype = ctypes.c_void_p
            lib.xkb_keymap_new_from_names.argtypes = [ctypes.c_void_p, ctypes.POINTER(self.Names), ctypes.c_int]
            lib.xkb_state_new.restype = ctypes.c_void_p
            lib.xkb_state_new.argtypes = [ctypes.c_void_p]
            lib.xkb_state_unref.argtypes = [ctypes.c_void_p]
            lib.xkb_state_update_key.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int]
            lib.xkb_state_key_get_utf8.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_char_p, ctypes.c_size_t]
            ctx = lib.xkb_context_new(0)
            names = self.Names(b"evdev", b"pc105", layout.encode(), variant.encode(), options.encode() or None)
            keymap = lib.xkb_keymap_new_from_names(ctx, ctypes.byref(names), 0) if ctx else None
            if not keymap:
                raise OSError(f"no keymap for layout {layout!r}")
            self.lib, self.keymap = lib, keymap
            self.reset()
            log(f"typing: layout {layout}{'(' + variant + ')' if variant else ''}")
        except (OSError, AttributeError) as e:
            log(f"typing: libxkbcommon: {e}; a US layout instead")

    @staticmethod
    def layout():
        """The desktop's first keyboard layout (Plasma's kxkbrc in this session's config)."""
        kf = GLib.KeyFile()
        path = os.path.join(os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config"), "kxkbrc")
        try:
            kf.load_from_file(path, GLib.KeyFileFlags.NONE)
        except GLib.Error:
            return "us", "", ""

        def first(key):
            try:
                return (kf.get_string("Layout", key) or "").split(",")[0].strip()
            except GLib.Error:
                return ""
        try:
            options = kf.get_string("Layout", "Options") if kf.get_boolean("Layout", "ResetOldOptions") else ""
        except GLib.Error:
            options = ""
        return first("LayoutList") or "us", first("VariantList"), options or ""

    def reset(self):
        if self.lib:
            if self.state:
                self.lib.xkb_state_unref(self.state)
            self.state = self.lib.xkb_state_new(self.keymap)

    def update(self, code, down):
        if self.state:
            self.lib.xkb_state_update_key(self.state, code + 8, 1 if down else 0)

    def text(self, code, shift):
        if self.state:
            buf = ctypes.create_string_buffer(16)
            n = self.lib.xkb_state_key_get_utf8(self.state, code + 8, buf, len(buf))
            return buf.raw[:max(0, min(n, 15))].decode(errors="ignore")
        pair = spotsearch.US_TEXT.get(code)
        return pair[1 if shift else 0] if pair else ""


# ---------------------------------------------------------------- icons

class Icons:
    """Icon names to files (the desktop's icon theme, then breeze and hicolor), and files to the
    panel's 64x64 RGBA icons, cached in ICON_DIR."""

    def __init__(self):
        self.themes = None   # [(theme, {name: path})], best first
        self.raw = {}        # source path -> raw icon path (or "-")

    @staticmethod
    def bases():
        home = os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")
        dirs = os.environ.get("XDG_DATA_DIRS", "/usr/local/share:/usr/share").split(":")
        return [os.path.join(home, "icons"), os.path.expanduser("~/.icons")] + \
            [os.path.join(d, "icons") for d in dirs if d]

    def chain(self):
        kf = GLib.KeyFile()
        config = os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config")
        theme = "breeze"
        try:
            kf.load_from_file(os.path.join(config, "kdeglobals"), GLib.KeyFileFlags.NONE)
            theme = kf.get_string("Icons", "Theme") or theme
        except GLib.Error:
            pass
        out = []
        todo = [theme, "breeze", "hicolor"]
        while todo:
            t = todo.pop(0)
            if t in out:
                continue
            out.append(t)
            for base in self.bases():
                index = GLib.KeyFile()
                try:
                    index.load_from_file(os.path.join(base, t, "index.theme"), GLib.KeyFileFlags.NONE)
                    todo[0:0] = [i.strip() for i in index.get_string("Icon Theme", "Inherits").split(",") if i.strip()]
                    break
                except GLib.Error:
                    continue
        return out

    def scan(self):
        """Each theme's icons by name: an app icon over others, scalable over the biggest bitmap."""
        self.themes = []
        for theme in self.chain():
            found = {}
            for base in self.bases():
                root = os.path.join(base, theme)
                for dirpath, _dirs, files in os.walk(root):
                    rel = os.path.relpath(dirpath, root)
                    size = max([int(p.split("x")[0].split("@")[0]) for p in rel.split(os.sep)
                                if p.split("x")[0].split("@")[0].isdigit()] or [0])
                    for f in files:
                        name, ext = os.path.splitext(f)
                        if ext not in (".svg", ".svgz", ".png"):
                            continue
                        rank = (("/apps" in "/" + rel) * 10000 + (5000 if ext != ".png" or "scalable" in rel else size))
                        if name not in found or rank > found[name][0]:
                            found[name] = (rank, os.path.join(dirpath, f))
            self.themes.append((theme, {k: v[1] for k, v in found.items()}))

    def find(self, gicon):
        """The file for a Gio.Icon (or an icon name), or None."""
        if isinstance(gicon, Gio.FileIcon):
            return gicon.get_file().get_path()
        names = gicon.get_names() if isinstance(gicon, Gio.ThemedIcon) else [gicon] if isinstance(gicon, str) else []
        for name in names:
            if os.path.isabs(name):
                return name if os.path.isfile(name) else None
        if self.themes is None:
            self.scan()
        for name in names:
            for _theme, icons in self.themes:
                if name in icons:
                    return icons[name]
            for ext in (".png", ".svg", ".xpm"):
                p = os.path.join("/usr/share/pixmaps", name + ext)
                if os.path.isfile(p):
                    return p
        return None

    def panel_file(self, gicon):
        """A 64x64 RGBA file of the icon for ft-spotpanel, or "-"."""
        src = self.find(gicon) if gicon is not None else None
        if not src:
            return "-"
        if src in self.raw:
            return self.raw[src]
        try:
            st = os.stat(src)
            key = hashlib.sha1(f"{src}:{st.st_mtime_ns}:{st.st_size}".encode()).hexdigest()[:20]
            out = os.path.join(ICON_DIR, key + ".rgba")
            if not os.path.exists(out):
                pb = GdkPixbuf.Pixbuf.new_from_file_at_scale(src, ICON_PX, ICON_PX, True)
                if not pb.get_has_alpha():
                    pb = pb.add_alpha(False, 0, 0, 0)
                canvas = GdkPixbuf.Pixbuf.new(GdkPixbuf.Colorspace.RGB, True, 8, ICON_PX, ICON_PX)
                canvas.fill(0)
                w, h = min(pb.get_width(), ICON_PX), min(pb.get_height(), ICON_PX)
                pb.copy_area(0, 0, w, h, canvas, (ICON_PX - w) // 2, (ICON_PX - h) // 2)
                pixels, stride = canvas.get_pixels(), canvas.get_rowstride()
                data = b"".join(pixels[y * stride:y * stride + ICON_PX * 4] for y in range(ICON_PX))
                os.makedirs(ICON_DIR, exist_ok=True)
                with open(out + ".tmp", "wb") as f:
                    f.write(data)
                os.replace(out + ".tmp", out)
            self.raw[src] = out
        except (OSError, GLib.Error) as e:
            log(f"icon {src}: {e}")
            self.raw[src] = "-"
        return self.raw[src]


# ---------------------------------------------------------------- the search

class Entry:
    """A row: a result (list) or a menu line."""

    def __init__(self, kind, title, sub="", icon=None, badge="", run=None, item=None):
        self.kind = kind    # item, header, section
        self.title, self.sub, self.badge = title, sub, badge
        self.icon = icon    # a Gio.Icon or an icon name, made into the panel's file when shown
        self.run = run      # what Enter does
        self.item = item    # the result's spotsearch.Item (list rows)


class Spotwatch:
    def __init__(self, args):
        self.args = args
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.sock.bind("\0" + args.socket)
        self.sock.setblocking(False)
        self.fire = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)  # unbound: nobody answers it
        self.asker = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.asker.bind("")
        self.float_sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)  # ft-floatd's slow answers
        self.float_sock.bind("")
        self.float_sock.setblocking(False)
        self.xkb = Xkb()
        self.icons = Icons()
        self.apps = []          # spotsearch.Item, kind "app"; data: Gio.DesktopAppInfo
        self.remotes = []       # spotsearch.Item, kind "remote"; data: {"n", "id", "host", "label", "off"}
        self.running_remotes = {}  # screen number -> state, from ft-screens
        self.windows = []       # ft-floatd's spotlist
        self.usage = self.load_usage()
        self.is_open = False
        self.query = ""
        self.mode = "list"      # list, menu
        self.entries = []       # the rows of the current mode
        self.results = []       # the list's rows, kept while a menu is open
        self.list_sel = 0
        self.sel = 0
        self.top = 0
        self.held = set()       # key codes held (from the search's own typing)
        self.repeat = None      # (code, GLib source)
        self.renew = None
        self.opened_keyboard = False
        self.panel = None
        self.panel_backoff = 5
        self.panel_started = 0.0
        self.toast_timer = None
        self.app_reload_pending = False
        self.load_apps()
        self.monitor = Gio.AppInfoMonitor.get()
        self.monitor.connect("changed", self.apps_changed)

    # ------------------------------------------------------------ talking to the others

    def to_panel(self, text):
        try:
            self.fire.sendto(text.encode(), "\0" + self.args.panel_socket)
        except OSError:
            pass

    def ask(self, name, text, timeout=1.0):
        """A question to ft-screens or ft-spotpanel; None without an answer."""
        self.asker.settimeout(timeout)
        try:
            self.asker.sendto(text.encode(), "\0" + name)
            return self.asker.recv(65536).decode(errors="replace")
        except OSError:
            return None

    def screens(self, text, timeout=1.0):
        reply = self.ask(self.args.screens, text, timeout)
        if reply is None or not reply.startswith("ok"):
            log(f"ft-screens: {text}: {reply or 'no answer'}")
        return reply or ""

    def floatd(self, text):
        """A command for ft-floatd, answered at once ("ok", "error ..."), or None when it isn't running."""
        reply = self.ask(self.args.float, text, 2.0)
        if reply is not None and not reply.startswith("ok"):
            log(f"ft-floatd: {text}: {reply}")
        return reply

    # ------------------------------------------------------------ ft-spotpanel

    def start_panel(self):
        if self.args.no_panel or self.panel:
            return False
        if not os.path.exists(PANEL):
            log(f"ft-spotpanel isn't built: run {REPO}/screens/build.sh")
            return False
        env = dict(os.environ, XDG_RUNTIME_DIR=f"/run/user/{os.getuid()}")  # podman needs the real one
        args = [IN_BOX, PANEL, "--watch-stdin", "--socket", self.args.panel_socket, "--notify", self.args.socket]
        try:
            self.panel = subprocess.Popen(args, env=env, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                                          start_new_session=True)
        except OSError as e:
            log(f"ft-spotpanel: {e}")
            return False
        self.panel_started = time.monotonic()
        GLib.child_watch_add(GLib.PRIORITY_DEFAULT, self.panel.pid, self.panel_exited)
        log(f"ft-spotpanel started (pid {self.panel.pid})")
        return False

    def panel_exited(self, pid, status):
        log(f"ft-spotpanel stopped (status {status})")
        self.panel = None
        self.close("the panel stopped")
        # Again after a while: SteamVR may not be up yet, or it restarted.
        self.panel_backoff = 5 if time.monotonic() - self.panel_started > 60 else min(60, self.panel_backoff * 2)
        GLib.timeout_add_seconds(self.panel_backoff, self.start_panel)

    # ------------------------------------------------------------ what there is to find

    def load_apps(self):
        apps = []
        for info in Gio.AppInfo.get_all():
            if not isinstance(info, Gio.DesktopAppInfo) or not info.should_show():
                continue
            app_id = (info.get_id() or "").removesuffix(".desktop")
            if not app_id:
                continue
            it = spotsearch.Item("app", app_id, info.get_display_name() or info.get_name() or app_id,
                                 info.get_generic_name() or "", info.get_keywords() or [],
                                 os.path.basename(info.get_executable() or ""), info)
            apps.append(it)
        self.apps = apps
        self.app_reload_pending = False
        log(f"{len(apps)} apps")

    def apps_changed(self, *_):
        if not self.app_reload_pending:  # wait for an install or update to settle
            self.app_reload_pending = True
            GLib.timeout_add_seconds(2, self.reload_apps)

    def reload_apps(self):
        self.load_apps()
        return False

    def warm_icons(self):
        """Every app's icon made for the panel, one per idle moment, so typing never waits on SVGs."""
        todo = [a.data.get_icon() for a in self.apps if a.data.get_icon() is not None] + [REMOTE_ICON]

        def one():
            if todo and not self.is_open:
                self.icons.panel_file(todo.pop())
            return bool(todo)
        GLib.timeout_add(50, one)
        return False

    def load_remotes(self):
        """The layout's remote displays, without hidden ones, and which are running (ft-screens)."""
        try:
            layout = ft_layout.load_layout()
        except (OSError, ValueError) as e:
            log(f"layout: {e}")
            layout = {}
        out = []
        for n, host, d in ft_layout.remote_displays(layout):
            if d.get("hidden") or not d.get("id"):
                continue
            hname = host.get("name") or host.get("address") or "?"
            label = d.get("label") or d["id"]
            out.append(spotsearch.Item("remote", "remote:" + d["id"], f"{hname} - {label}", "Remote display",
                                       ["remote", "display", "stream", hname, label], "",
                                       {"n": n, "id": d["id"], "host": hname, "label": label}))
        self.remotes = out
        reply = self.screens("remotes")
        self.running_remotes = {}
        for word in reply.split()[2:] if reply.startswith("ok") else []:
            parts = word.split(":")
            if len(parts) >= 3 and parts[0].isdigit():
                self.running_remotes[int(parts[0])] = parts[2]
        for r in self.remotes:
            r.running = r.data["n"] in self.running_remotes

    def refresh_windows(self):
        """Ask ft-floatd for every window (answered when KWin's script has reported them)."""
        try:
            self.float_sock.sendto(b"spotlist", "\0" + self.args.float)
        except OSError:
            self.windows = []  # no ft-floatd (no floating slots): nothing counts as running

    def float_readable(self, *_):
        while True:
            try:
                data = self.float_sock.recv(1 << 20)
            except BlockingIOError:
                return True
            text = data.decode(errors="replace")
            if text.startswith("ok ["):
                try:
                    self.windows = json.loads(text[3:])
                except ValueError:
                    continue
                if self.is_open:
                    self.refilter(keep=True)

    def windows_of(self, item):
        """An app's windows, most recently active first."""
        info = item.data
        names = {n.lower() for n in (item.key, info.get_startup_wm_class() or "", item.exe) if n}
        wins = [w for w in self.windows
                if w.get("app") == item.key or (not w.get("app") and w.get("class", "").lower() in names)]
        return sorted(wins, key=lambda w: -(w.get("activeAt") or 0))

    def screen_count(self):
        reply = self.screens("screens")
        return int(reply.split()[1]) if reply.startswith("ok") and len(reply.split()) > 1 else 0

    def front_screen(self):
        """The screen nearest where your head points (1-based), or None."""
        head = self.screens("head").split()
        if len(head) < 5 or head[0] != "ok":
            return None
        x, z, yaw = float(head[1]), float(head[3]), float(head[4])
        best, best_n = 181.0, None
        for n in range(1, self.screen_count() + 1):
            g = self.screens(f"get {n}").split()
            if len(g) < 4 or g[0] != "ok":
                continue
            # Bearing of its centre from the head, against the heading (both atan2(x, z) of a back vector).
            dx, dz = float(g[1]) - x, float(g[3]) - z
            a = abs((math.degrees(math.atan2(-dx, -dz)) - yaw + 180) % 360 - 180)
            if a < best:
                best, best_n = a, n
        return best_n

    @staticmethod
    def where(w, screens_n=None):
        if w.get("minimized"):
            return "Minimized"
        if w.get("floating"):
            return "Floating"
        return f"Screen {w['panel']}" if w.get("panel") else "Desktop"

    # ------------------------------------------------------------ the rows

    def result_entry(self, item):
        if item.kind == "remote":
            state = self.running_remotes.get(item.data["n"])
            return Entry("item", item.title, f"Remote display · {state}" if state else "Remote display · not connected",
                         REMOTE_ICON, "Go to" if state else "Connect",
                         run=lambda: self.go_remote(item), item=item)
        wins = self.windows_of(item)
        item.running = bool(wins)
        if wins:
            sub = f"Open · {self.where(wins[0])}" + (f" · {len(wins)} windows" if len(wins) > 1 else "")
        else:
            sub = item.generic or "Application"
        return Entry("item", item.title, sub, item.data.get_icon(), "Switch to" if wins else "Open",
                     run=lambda: self.go_app(item), item=item)

    def refilter(self, keep=False):
        """The results for the query. keep: the same item stays selected (new window info came)."""
        if self.mode != "list":
            return
        was = self.entries[self.sel].item if keep and 0 <= self.sel < len(self.entries) else None
        for a in self.apps:
            a.running = bool(self.windows_of(a)) if self.windows else False
        found = spotsearch.search(self.apps + self.remotes, self.query, self.usage)
        self.entries = [self.result_entry(it) for it in found]
        self.sel = next((i for i, e in enumerate(self.entries) if e.item is was), 0) if was else 0
        self.top = 0
        self.render()

    def menu_for(self, entry):
        item = entry.item
        rows = [Entry("header", item.title, "", entry.icon)]  # (a click on it goes back)
        if item.kind == "remote":
            state = self.running_remotes.get(item.data["n"])
            rows.append(Entry("item", "Go to" if state else "Connect and go to", item.title,
                              run=lambda: self.go_remote(item)))
            if state:
                rows.append(Entry("item", "Disconnect", item.title, run=lambda: self.disconnect(item)))
            return rows
        info = item.data
        actions = [(a, info.get_action_name(a)) for a in info.list_actions() if a != STANDALONE_ACTION]
        for a, name in actions:
            rows.append(Entry("item", name or a, "", run=lambda a=a: self.launch(item, action=a)))
        rows.append(Entry("item", "Open new window", "Floating, even with one open",
                          run=lambda: self.launch(item, action=self.new_window_action(actions))))
        screen = self.front_screen()
        if screen:
            rows.append(Entry("item", "Open docked on this screen", f"Screen {screen}",
                              run=lambda: self.launch(item, screen=screen)))
        wins = self.windows_of(item)
        if wins:
            rows.append(Entry("section", "Windows"))
            for w in wins:
                rows.append(Entry("item", w.get("title") or item.title, self.where(w), entry.icon,
                                  run=lambda w=w: self.go_window(item, w)))
        return rows

    @staticmethod
    def new_window_action(actions):
        """An app's own "new window" action, when it has one (not a private one)."""
        for a, name in actions:
            text = (a + " " + (name or "")).lower().replace("-", "").replace("_", "").replace(" ", "")
            if "newwindow" in text and not any(p in text for p in ("private", "incognito", "inprivate")):
                return a
        return None

    def render(self):
        if not self.is_open:
            return
        n = len(self.entries)
        self.sel = max(0, min(self.sel, n - 1)) if n else 0
        if self.sel < self.top:
            self.top = self.sel
        elif self.sel >= self.top + ROWS:
            self.top = self.sel - ROWS + 1
        self.top = max(0, min(self.top, max(0, n - ROWS)))
        lines = [f"query\t{clean(self.query)}"]
        if not self.entries and self.query.strip():
            lines.append("hint\tNo apps or displays match")
        lines.append(f"sel\t{self.sel - self.top if n else -1}")
        for e in self.entries[self.top:self.top + ROWS]:
            icon = self.icons.panel_file(e.icon) if e.icon is not None else "-"
            lines.append("\t".join(["row", e.kind, icon, clean(e.title), clean(e.sub), clean(e.badge)]))
        self.to_panel("state\n" + "\n".join(lines))

    def move(self, step, wrap=False):
        """The selection step rows on (wrap: round from the end), past headers and sections."""
        items = [i for i, e in enumerate(self.entries) if e.kind == "item"]
        if not items:
            return
        if wrap:
            at = items.index(self.sel) if self.sel in items else -1
            self.sel = items[(at + (1 if step > 0 else -1)) % len(items)]
        elif step > 0:
            self.sel = ([i for i in items if i >= self.sel + step] or [items[-1]])[0]
        else:
            self.sel = ([i for i in items if i <= self.sel + step] or [items[0]])[-1]
        self.render()

    # ------------------------------------------------------------ open and close

    def open(self, source="keyboard"):
        if self.is_open:
            return "ok open"
        if not self.panel and not self.args.no_panel:
            self.start_panel()
            return "error the panel isn't running yet"
        if not self.screens("spotwatch on").startswith("ok"):
            return "error ft-screens didn't answer"
        self.is_open, self.query, self.mode, self.entries, self.sel, self.top = True, "", "list", [], 0, 0
        self.held.clear()
        self.xkb.reset()
        self.stop_repeat()
        if self.toast_timer:
            GLib.source_remove(self.toast_timer)
            self.toast_timer = None
        self.refresh_windows()
        self.load_remotes()
        self.render()
        reply = self.ask(self.args.panel_socket, "show") if not self.args.no_panel else "ok"
        if not reply or not reply.startswith("ok"):
            log(f"ft-spotpanel: show: {reply or 'no answer'}")
            self.close("the panel can't show")
            return "error " + (reply or "the panel didn't answer")
        self.renew = GLib.timeout_add(RENEW_MS, self.renew_lease)
        # Opened from a mouse or controller button: maybe no keyboard at hand.
        self.opened_keyboard = source != "keyboard" and self.screens("vrkeyboard show") == "ok opened"
        log(f"open (from the {source})")
        return "ok"

    def renew_lease(self):
        if not self.is_open:
            return False
        try:
            self.fire.sendto(b"spotwatch on", "\0" + self.args.screens)
        except OSError:
            pass
        return True

    def close(self, why, tell=True):
        if not self.is_open:
            return
        self.is_open = False
        self.to_panel("hide")
        if self.renew:
            GLib.source_remove(self.renew)
            self.renew = None
        self.stop_repeat()
        if tell:
            self.screens("spotwatch off")
        if self.opened_keyboard:
            self.screens("vrkeyboard close")
            self.opened_keyboard = False
        log(f"closed ({why})")

    def toast(self, text, seconds=2.5):
        """A word on the panel, without the search (where a pinned panel is)."""
        if self.args.no_panel:
            return
        self.to_panel(f"state\ntoast\t{clean(text)}")
        reply = self.ask(self.args.panel_socket, "show toast")
        if reply and reply.startswith("ok"):
            if self.toast_timer:
                GLib.source_remove(self.toast_timer)
            self.toast_timer = GLib.timeout_add(int(seconds * 1000), self.end_toast)

    def end_toast(self):
        self.toast_timer = None
        if not self.is_open:
            self.to_panel("hide")
        return False

    # ------------------------------------------------------------ doing it

    def used(self, key):
        spotsearch.use(self.usage, key)
        try:
            os.makedirs(os.path.dirname(USAGE_PATH), exist_ok=True)
            with open(USAGE_PATH + ".tmp", "w") as f:
                json.dump({"usage": self.usage}, f)
            os.replace(USAGE_PATH + ".tmp", USAGE_PATH)
        except OSError as e:
            log(f"usage: {e}")

    @staticmethod
    def load_usage():
        try:
            with open(USAGE_PATH) as f:
                u = json.load(f).get("usage", {})
            return u if isinstance(u, dict) else {}
        except (OSError, ValueError, AttributeError):
            return {}

    def go_app(self, item):
        wins = self.windows_of(item)
        if wins:
            self.go_window(item, wins[0])
        else:
            self.launch(item)

    def go_window(self, item, w):
        self.close("switch")
        self.used(item.key)
        log(f"to {item.key}'s window {w.get('id', '')[:9]} ({self.where(w)})")
        self.floatd(f"activate {w['id']}")
        if w.get("panel") and not w.get("minimized"):
            self.bring(w["panel"], item.title)
        elif w.get("panel"):
            GLib.timeout_add(300, lambda: self.bring(w["panel"], item.title) and False)  # unminimizing

    def bring(self, n, name):
        """Panel n to straight ahead (the lazy susan), or typing to it where it is when it can't spin."""
        reply = self.screens(f"spin to {n}")
        if reply.startswith("ok"):
            return
        self.screens(f"front {n}")
        get = self.screens(f"get {n}").split()
        pin = get[16] if len(get) > 16 and get[0] == "ok" else "none"
        if pin in ("left", "right"):
            self.toast(f"{name} is on your {pin} wrist")
        elif pin == "head":
            self.toast(f"{name} is pinned to your view")

    def launch(self, item, action=None, screen=None):
        self.close("launch")
        self.used(item.key)
        cmd = f"launch {item.key}.desktop" + (f" action={action}" if action else "") + \
            (f" screen={screen}" if screen else "")
        reply = self.floatd(cmd)
        if reply is not None:
            log(f"{cmd}: {reply}")
            return
        # No ft-floatd (no floating slots): the desktop opens it as it would from its menu.
        try:
            if action:
                item.data.launch_action(action, Gio.AppLaunchContext())
            else:
                item.data.launch([], Gio.AppLaunchContext())
            log(f"launched {item.key} without ft-floatd")
        except GLib.Error as e:
            log(f"launch {item.key}: {e.message}")

    def go_remote(self, item):
        self.close("remote display")
        self.used(item.key)
        n = item.data["n"]
        if n in self.running_remotes:
            self.bring(n, item.title)
            return
        log(f"connecting {item.data['id']}")
        self.layout_command(["remote", "connect", item.data["id"]], lambda ok: ok and self.await_remote(n, item.title))

    def await_remote(self, n, name, tries=40):
        """After a connect: spin to the display once its panel is there."""
        if self.screens(f"get {n}").startswith("ok"):
            self.bring(n, name)
        elif tries > 0:
            GLib.timeout_add(250, lambda: self.await_remote(n, name, tries - 1) and False)
        else:
            log(f"remote display {n}: no panel after connecting")

    def disconnect(self, item):
        self.close("disconnect")
        self.layout_command(["remote", "disconnect", item.data["id"]], lambda ok: None)

    def layout_command(self, args, done):
        try:
            p = subprocess.Popen([FT_LAYOUT, *args], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                 start_new_session=True)
        except OSError as e:
            log(f"ft-layout: {e}")
            return

        def exited(_pid, status):
            if status:
                log(f"ft-layout {' '.join(args)}: exit {status}")
            done(status == 0)
        GLib.child_watch_add(GLib.PRIORITY_DEFAULT, p.pid, exited)

    def activate(self):
        if 0 <= self.sel < len(self.entries) and self.entries[self.sel].run:
            self.entries[self.sel].run()

    def open_menu(self):
        if self.mode != "list" or not (0 <= self.sel < len(self.entries)):
            return
        self.results, self.list_sel = self.entries, self.sel
        self.entries = self.menu_for(self.entries[self.sel])
        self.mode, self.sel, self.top = "menu", 1, 0
        self.render()

    def back(self):
        if self.mode != "menu":
            return
        self.mode, self.entries, self.sel, self.top = "list", self.results, self.list_sel, 0
        self.render()

    # ------------------------------------------------------------ typing

    def key(self, code, value):
        if not self.is_open:
            return
        down = value != 0
        self.xkb.update(code, down)
        (self.held.add if down else self.held.discard)(code)
        if not down:
            if self.repeat and self.repeat[0] == code:
                self.stop_repeat()
            return
        self.press(code)
        if code in REPEATS and self.is_open:
            self.stop_repeat()
            self.repeat = (code, GLib.timeout_add(REPEAT_DELAY_MS, self.start_repeat, code))

    def start_repeat(self, code):
        self.repeat = (code, GLib.timeout_add(REPEAT_MS, self.repeat_key, code))
        return False

    def repeat_key(self, code):
        if not self.is_open or code not in self.held:
            self.repeat = None
            return False
        self.press(code)
        return True

    def stop_repeat(self):
        if self.repeat:
            GLib.source_remove(self.repeat[1])
            self.repeat = None

    def press(self, code):
        shift, ctrl = bool(self.held & SHIFT), bool(self.held & CTRL)
        if code == KEY_ESC:
            self.back() if self.mode == "menu" else self.close("Esc")
        elif code in (KEY_ENTER, KEY_KPENTER):
            self.open_menu() if shift and self.mode == "list" else self.activate()
        elif code == KEY_TAB:
            self.move(-1 if shift else 1, wrap=True)
        elif code in (KEY_UP, KEY_DOWN):
            self.move(-1 if code == KEY_UP else 1)
        elif code in (KEY_PAGEUP, KEY_PAGEDOWN):
            self.move(-ROWS if code == KEY_PAGEUP else ROWS)
        elif code == KEY_LEFT and self.mode == "menu":
            self.back()
        elif code == KEY_BACKSPACE:
            if self.mode == "menu":
                self.back()
            else:
                self.query = spotsearch.drop_word(self.query) if ctrl else self.query[:-1]
                self.refilter()
        elif ctrl and code == KEY_U:
            self.back()
            self.query = ""
            self.refilter()
        elif not (ctrl or self.held & ALT or self.held & META):
            text = self.xkb.text(code, shift)
            if text and text.isprintable():
                self.back()
                self.query += text
                self.refilter()

    # ------------------------------------------------------------ the socket

    def request(self, text):
        words = text.split()
        if not words:
            return "error empty"
        cmd, rest = words[0], words[1:]
        if cmd == "key" and len(rest) == 2:
            try:
                self.key(int(rest[0]), int(rest[1]))
            except ValueError:
                return "error key CODE VALUE"
            return None
        if cmd == "toggle":
            if self.is_open:
                self.close("toggle")
                return "ok closed"
            return self.open(rest[0] if rest else "keyboard")
        if cmd == "open":
            return self.open(rest[0] if rest else "keyboard")
        if cmd == "close":
            self.close("asked")
            return "ok"
        if cmd == "dismiss":
            self.close(" ".join(rest) or "dismissed", tell=False)
            return None
        if cmd == "status":
            return f"ok {'open' if self.is_open else 'closed'} {self.mode} {self.query}"
        if cmd == "rows":
            return "ok " + json.dumps([e.title for e in self.entries])
        if not self.is_open:
            return None
        # From the panel.
        if cmd in ("hover", "click", "menu") and rest and rest[0].isdigit():
            i = self.top + int(rest[0])
            if not (0 <= i < len(self.entries)) or self.entries[i].kind != "item":
                return None
            if self.sel != i:
                self.sel = i
                self.render()
            if cmd == "click":
                self.activate()
            elif cmd == "menu":
                self.open_menu() if self.mode == "list" else self.activate()
        elif cmd == "back":
            self.back()
        elif cmd == "scroll" and rest:
            self.move(1 if rest[0].startswith("+") else -1)
        elif cmd == "outside":
            self.close("a click beside it")
        return None

    def readable(self, *_):
        while True:
            try:
                data, sender = self.sock.recvfrom(65536)
            except BlockingIOError:
                return True
            text = data.decode(errors="replace").strip()
            if text.startswith(("ok", "error")):
                continue  # (an answer to nobody)
            try:
                reply = self.request(text)
            except Exception as e:  # keep the service up; the log says what broke
                log(f"{text[:80]}: {e!r}")
                reply = "error " + str(e)
            if sender and reply is not None:
                try:
                    self.sock.sendto(reply.encode(), sender)
                except OSError:
                    pass


def main():
    p = argparse.ArgumentParser(description="Spotwatch: Frametop's search for apps and remote displays")
    p.add_argument("--socket", default="ft_spotwatch")
    p.add_argument("--panel-socket", default="ft_spotpanel")
    p.add_argument("--screens", default="ft_screens")
    p.add_argument("--float", default="frametop_float")
    p.add_argument("--no-panel", action="store_true", help="no ft-spotpanel (tests)")
    args = p.parse_args()
    try:
        sw = Spotwatch(args)
    except OSError as e:
        sys.exit(f"ft-spotwatch: @{args.socket}: {e} (another copy running?)")
    GLib.io_add_watch(sw.sock.fileno(), GLib.IO_IN, sw.readable)
    GLib.io_add_watch(sw.float_sock.fileno(), GLib.IO_IN, sw.float_readable)
    sw.start_panel()
    loop = GLib.MainLoop()
    GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, 15, loop.quit)
    GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, 2, loop.quit)
    # Icons in the background, so the first search doesn't wait for the theme's scan or SVGs.
    GLib.timeout_add_seconds(3, sw.warm_icons)
    log(f"@{args.socket}: ready")
    loop.run()
    sw.close("stopping")
    if sw.panel:
        sw.panel.terminate()


if __name__ == "__main__":
    main()
