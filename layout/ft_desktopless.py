"""Desktopless mode (docs/desktopless.md): the desktops go, their apps stay. The Frame's screens
and the PCs' displays hide, and every app window on them becomes a panel of its own: the Frame's
windows float (ft-floatd's "float all"), and each PC window streams as a window panel (ft-layout
window). Off again, the windows go back where they were: the Frame's onto their screens, the PCs'
onto their desktops (the host puts each one back at its old place).

Only PCs with a Frametop windows instance (Linux hosts, "windows=PORT") give their windows up; a
PC's display panels hide only then. A hidden display panel stays connected: its stream holds the
PC's keyboard and mouse for the Frame (window streams don't take the input lock).

  ft-layout desktopless on|off|toggle|status
  ft-layout desktopless watch    (the frametop-desktopless user unit while it's on: a PC's new
                                  windows become panels, closed ones go, a PC that connects later
                                  gives its windows up)

The layout keeps "desktopless": {"on": true, "before": {"screens": [...], "remotes": [...]}} (what
was hidden before, to put back) and "places": {"HOST|APP": [pose, ...]} (where each PC app's
window panels were, for next time). The Frame's windows remember their places in ft-floatd.
"""
import json
import math
import os
import subprocess
import sys
import time

import ft_layout as L

UNIT = "frametop-desktopless"
POLL_SECONDS = 2.0
ARC_DISTANCE = 1.3   # metres: the fan of panels with no place of their own
ARC_SPAN = 150.0     # degrees: a row of the fan at most, then a row above it
ARC_GAP = 0.04       # metres between panels in the fan
ARC_PITCH = -8.0     # degrees: the first row, a little below eye level
SEEN_PATH = os.path.join(os.environ.get("XDG_RUNTIME_DIR") or "/tmp", "frametop-desktopless-seen.json")


def state(layout):
    s = layout.get("desktopless")
    return s if isinstance(s, dict) else {}


def window_hosts(layout):
    """[(host, port)]: the PCs that give their windows up (a Frametop windows instance)."""
    out = []
    for h in layout.get("hosts", []):
        port = h.get("windows", {}).get("port") if isinstance(h.get("windows"), dict) else None
        if isinstance(port, int):
            out.append((h, port))
    return out


def host_name(host):
    return host.get("name") or host["address"]


def app_key(host, w):
    """A PC window's app, for its place next time: its desktop app, else its class."""
    return f"{host_name(host)}|{w.get('app') or w.get('class') or '?'}"


def rank(w):
    """Which windows come out first when there are more than panels: the ones on a workspace that
    shows, then the most recently used (Hyprland's focus history: 0 is the focused one). User
    choice, 2026-11-07."""
    f = w.get("focus")
    return not w.get("shown"), f if isinstance(f, int) and f >= 0 else 1 << 20


def arc(sizes, dist=ARC_DISTANCE):
    """Head-frame poses ({"pos", "face", "roll"}) for panels of sizes [(w, h)] in metres: fanned in
    front of you on a curve (user choice, 2026-11-07), each turned to face you, row by row
    upwards once a row spans ARC_SPAN degrees."""
    span = lambda m: 2 * math.degrees(math.atan(m / 2 / dist))
    rows, row, used = [], [], 0.0
    for k, (w, _) in enumerate(sizes):
        a = span(w + ARC_GAP)
        if row and used + a > ARC_SPAN:
            rows.append(row)
            row, used = [], 0.0
        row.append(k)
        used += a
    if row:
        rows.append(row)
    out, pitch, last = [None] * len(sizes), ARC_PITCH, 0.0
    for r, ks in enumerate(rows):
        tall = max(span(sizes[k][1]) for k in ks)
        if r:
            pitch += last / 2 + span(ARC_GAP) + tall / 2
        last = tall
        cp, sp = math.cos(math.radians(pitch)), math.sin(math.radians(pitch))
        for k, (x, z, yaw) in zip(ks, L._chain([sizes[k][0] for k in ks], dist, ARC_GAP)):
            out[k] = {"pos": [x * cp, math.hypot(x, z) * sp, z * cp], "face": [yaw, pitch], "roll": 0.0}
    return out


def place(sock, n, t, eye, heading):
    """Panel n at head-frame pose t."""
    c = tuple(e + v for e, v in zip(eye, L.turn_yaw(t["pos"], heading)))
    sock.ask("place %d %.4f %.4f %.4f %.3f %.3f %.3f" % (n, *c, t["face"][0] + heading, t["face"][1], t["roll"]))


def read_seen():
    try:
        with open(SEEN_PATH) as f:
            seen = json.load(f)
        return {k: set(v) for k, v in seen.items()} if isinstance(seen, dict) else {}
    except (OSError, ValueError):
        return {}


def write_seen(seen):
    L.ft_store.write_json(SEEN_PATH, {k: sorted(v) for k, v in seen.items()})


def conceal(sock, numbers, hidden):
    for n in numbers:
        try:
            sock.ask(f"{'conceal' if hidden else 'reveal'} {n}")
        except RuntimeError as e:
            L.log(f"desktopless: screen {n}: {e}")


def displays_of(layout, hosts):
    """[(number, display)]: the display panels (not window panels) of these PCs."""
    ids = {id(h) for h, _ in hosts}
    return [(n, d) for n, h, d in L.remote_displays(layout) if id(h) in ids and not d.get("window")]


def open_windows(layout, sock, eye, heading, seen, extra=()):
    """Every PC window not out yet (nor sent back by you since) as a panel, in order, while
    panels are free: at its app's place from last time, or fanned in front of you with extra
    [(screen number, (w, h))] (the Frame's new floating panels). Returns how many opened."""
    places = {k: list(v) for k, v in state(layout).get("places", {}).items()}
    todo = []
    for host, port in window_hosts(layout):
        try:
            answer = L.window_api(host, port, "GET", "/api/frametop/windows")
        except RuntimeError:
            continue  # not on (or not reachable): it gives its windows up once it answers
        windows = answer.get("windows", [])
        L.window_prune(layout, host, windows)
        out = {d["app"][7:] for d in host.get("displays", []) if d.get("window")}
        mine = seen.setdefault(host_name(host), set())
        mine &= {w.get("address") for w in windows}  # (closed ones: their addresses come back new)
        for w in windows:
            a = w.get("address")
            # (one on a special workspace stays there: the host won't give it up)
            if a in out or a in mine or w.get("output") or str(w.get("workspace", "")).startswith("special"):
                continue
            todo.append((host, port, w))
    free = L.REMOTE_MAX - len(L.remote_displays(layout))
    todo = sorted(todo, key=lambda t: rank(t[2]))[:max(0, free)]
    fan, new = [], []
    for host, port, w in todo:
        size = L.window_size({}, (min(w.get("width") or L.WINDOW_SIZE[0], L.WINDOW_MAX[0]),
                                  min(w.get("height") or L.WINDOW_SIZE[1], L.WINDOW_MAX[1])))
        saved = places.get(app_key(host, w)) or []
        pose = saved.pop(0) if saved else None
        new.append((host, port, w, size, pose))
        if pose is None:
            fan.append(len(new) - 1)
    def metres(size):  # a new window panel's size in VR (window_add's width)
        w = max(0.4, size[0] / L.WINDOW_PIXELS_PER_METRE)
        return w, w * size[1] / size[0]
    poses = arc([s for _, s in extra] + [metres(new[k][3]) for k in fan])
    for (n, _), t in zip(extra, poses):
        try:
            place(sock, n, t, eye, heading)
        except RuntimeError as e:
            L.log(f"desktopless: floating panel {n}: {e}")
    fanned = dict(zip(fan, poses[len(extra):]))
    room = room_anchor(sock, layout) if len(fanned) < len(new) else None
    opened = 0
    for k, (host, port, w, size, saved) in enumerate(new):
        fields = {"key": app_key(host, w)}
        if saved:
            fields["metres"] = saved.get("metres")
        fields.update({k2: v for k2, v in (saved or fanned[k]).items() if k2 in ("pos", "face", "roll")})
        try:
            d = L.window_add(layout, host, port, w["address"], str(w.get("title") or w.get("class") or ""), size,
                             fields=fields)
        except RuntimeError as e:
            L.log(f"desktopless: {host_name(host)} {w.get('address')}: {e}")
            continue
        seen.setdefault(host_name(host), set()).add(w["address"])
        opened += 1
        if saved and room:
            # (window_add placed it from your head: its place from last time is in the room)
            n = next((m for m, _, e in L.remote_displays(layout) if e is d), None)
            try:
                if n is not None:
                    L.place_remote(sock, n, d, saved, *room)
            except RuntimeError as e:
                L.log(f"desktopless: {d['id']}: {e}")
    return opened


def room_anchor(sock, layout):
    """(eye, heading) the screens were arranged from: places kept relative to it stay put in the
    room, as the Frame's floating windows' do (relative to the primary screen). None: the head's."""
    try:
        if not L.native_count(layout):
            return L.stored_anchor()
        return L.inferred_anchor(layout, L.parse_get(sock.ask("get 1")))
    except RuntimeError:
        return None


def remember_places(layout, sock, eye, heading):
    """Where each PC window panel is now, by app, for next time."""
    places = {}
    for host, _ in window_hosts(layout):
        for n, h, d in L.remote_displays(layout):
            if h is not host or not d.get("window"):
                continue
            try:
                g = L.parse_get(sock.ask(f"get {n}"))
            except RuntimeError:
                continue
            pose = L.relative_pose(g["center"], g["x"], g["z"], eye, heading)
            pose["metres"] = round(g["metres"], 4)
            places.setdefault(d.get("key") or f"{host_name(host)}|?", []).append(pose)
    return places


def turn_on(layout):
    s = state(layout)
    sock = L.screens_socket()
    eye, heading = L.remote_anchor(sock, layout)
    hosts = window_hosts(layout)
    if not s.get("on"):
        n = L.screen_count(layout)
        displays = displays_of(layout, hosts)
        s = {"on": True, "places": s.get("places", {}),
             "before": {"screens": [i for i in range(n) if L.screen_entry(layout, i).get("hidden")],
                        "remotes": [d["id"] for _, d in displays if d.get("hidden")]}}
        screens = layout.setdefault("screens", [])
        while len(screens) < n:
            screens.append({})
        for i in range(n):
            screens[i]["hidden"] = True
        for _, d in displays:
            d["hidden"] = True
        layout["desktopless"] = s
        L.save_layout(layout)
        conceal(sock, list(range(1, n + 1)) + [k for k, _ in displays], True)
        write_seen({})
    extra = []
    reply = L.ask_float("float all", timeout=30.0)
    if reply and reply.startswith("ok"):
        for w in reply.split()[1:]:
            try:
                g = L.parse_get(sock.ask(f"get {int(w)}"))
                extra.append((int(w), (g["metres"], g["height"])))
            except (RuntimeError, ValueError) as e:
                L.log(f"desktopless: floating panel {w}: {e}")
    elif reply is not None:
        L.log(f"desktopless: ft-floatd: {reply}")
    seen = read_seen()
    opened = open_windows(layout, sock, eye, heading, seen, extra)
    write_seen(seen)
    start_watch()
    L.log(f"desktopless on: {len(extra)} Frame window(s) fanned out, {opened} PC window(s) opened")


def turn_off(layout):
    s = state(layout)
    if not s.get("on"):
        L.log("desktopless is off")
        return
    stop_watch()
    sock = L.screens_socket()
    hosts = window_hosts(layout)
    try:
        eye, heading = room_anchor(sock, layout) or L.remote_anchor(sock, layout)
        s["places"] = remember_places(layout, sock, eye, heading) or s.get("places", {})
    except RuntimeError as e:
        L.log(f"desktopless: places not kept: {e}")
    for host, _ in hosts:
        for d in [d for d in host.get("displays", []) if d.get("window")]:
            L.window_close(layout, host, d)
    before = s.get("before", {})
    n = L.screen_count(layout)
    screens = layout.setdefault("screens", [])
    while len(screens) < n:
        screens.append({})
    shown = []
    for i in range(n):
        if i in before.get("screens", []):
            continue
        screens[i].pop("hidden", None)
        shown.append(i + 1)
    for k, d in displays_of(layout, hosts):
        if d["id"] not in before.get("remotes", []):
            d.pop("hidden", None)
            shown.append(k)
    layout["desktopless"] = {"on": False, "places": s.get("places", {})}
    L.save_layout(layout)
    conceal(sock, shown, False)
    # (after the screens show: a window whose screen is still hidden docks onto the first that shows)
    reply = L.ask_float("dock all", timeout=10.0)
    if reply is not None and not reply.startswith("ok"):
        L.log(f"desktopless: ft-floatd: {reply}")
    write_seen({})
    L.log("desktopless off: the windows are back on their desktops")


def start_watch():
    here = os.path.dirname(os.path.realpath(__file__))
    subprocess.run(["systemctl", "--user", "stop", f"{UNIT}.service"], capture_output=True)
    r = subprocess.run(["systemd-run", "--user", f"--unit={UNIT}", "--collect", "--quiet",
                        "-p", "CPUQuota=10%", sys.executable, os.path.join(here, "ft_layout.py"), "desktopless", "watch"],
                       capture_output=True, text=True)
    if r.returncode:
        L.log(f"desktopless: the watcher didn't start: {(r.stderr or r.stdout).strip()}")


def stop_watch():
    if os.environ.get("INVOCATION_ID") and os.environ.get("FT_DESKTOPLESS_WATCH"):
        return  # (the watcher itself: it ends on its own)
    subprocess.run(["systemctl", "--user", "stop", f"{UNIT}.service"], capture_output=True)


def watch():
    os.environ["FT_DESKTOPLESS_WATCH"] = "1"
    while True:
        layout = L.load_layout()
        if not state(layout).get("on"):
            return
        try:
            sock = L.screens_socket()
            eye, heading = L.remote_anchor(sock, layout)
            seen = read_seen()
            open_windows(layout, sock, eye, heading, seen)
            write_seen(seen)
        except L.NoAnswer:
            pass  # the desktop isn't up (yet)
        except RuntimeError as e:
            L.log(f"desktopless: {e}")
        time.sleep(POLL_SECONDS)


def command(args):
    what = args[0] if len(args) == 1 else ""
    layout = L.load_layout()
    if what == "status":
        print("on" if state(layout).get("on") else "off")
    elif what == "on" or (what == "toggle" and not state(layout).get("on")):
        turn_on(layout)
    elif what in ("off", "toggle"):
        turn_off(layout)
    elif what == "watch":
        watch()
    else:
        raise RuntimeError("desktopless on|off|toggle|status|watch")
