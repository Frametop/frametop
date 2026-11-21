"""ft-layout remote kwin-windows|kwin-add|kwin-size: a KDE Plasma PC's windows as panels, from
utrack's KWin Window Stream host service (github.com/utrack/kwin-window-stream; issue 81;
docs/remote-displays.md, "A KDE PC's windows").

The service streams an open KWin window by itself, HEVC over its own TLS connection on TCP 14501,
and knows clients by the certificates paired with the PC's Sunshine. So a window panel has no
client of its own: it uses one of its host's displays' clients, which is already paired, and
removing the panel leaves that pairing alone while another panel uses it (ft_layout's remove).
Its app is "kwin:{UUID}", KWin's id for the window, which lasts as long as the window does. The
PC's window sets its size; ft-screens keeps the panel's density when it changes and saves the
new size and width here (kwin-size). Based on utrack's custom-window-streamer branch
(github.com/utrack/frametop).
"""
import json
import math
import re
import subprocess
import time

import ft_layout as L

KWIN_APP = re.compile(r"kwin:\{[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\}")
CLIENT = re.compile(r"[A-Za-z0-9._-]+")
WINDOW_PIXELS_PER_METRE = 1400  # a new window panel's width in VR (as a PC app window's, ft_windows)
WINDOW_DISTANCE = 1.1


def printable(text, most=80):
    """A window's title for its panel's label, on one line: the PC chooses it."""
    return "".join(c for c in str(text) if c.isprintable()).strip()[:most]


def app_of(window_id):
    """KWin's id for a window ("{8d3e...}", braces or not) as a panel's app, or ValueError."""
    app = "kwin:{" + str(window_id).strip().strip("{}").lower() + "}"
    if not KWIN_APP.fullmatch(app):
        raise ValueError(f"{window_id!r} isn't a KWin window's id")
    return app


def host_clients(host):
    """The clients of the host's displays: one paired with it is what a window connects with."""
    clients = [d.get("client") for d in host.get("displays", [])]
    return [c for c in dict.fromkeys(clients) if c and CLIENT.fullmatch(c)]


def find_client(host, preferred=None, timeout=20, cursor=None):
    """(the client the PC took, its windows), asking with each of the host's clients in turn
    (`preferred` first) until one is taken. A poll with a short deadline keeps `cursor`, a dict,
    across calls, so one client that's refused can't use up every poll's time.
    RuntimeError: none was taken (no service, none paired), with the last reason."""
    clients = host_clients(host)
    if preferred in clients:
        clients.remove(preferred)
        clients.insert(0, preferred)
    if not clients:
        raise RuntimeError("Add one of this PC's displays first: its windows come with that display's pairing")
    cursor = {} if cursor is None else cursor
    if cursor.get("next") in clients:
        start = clients.index(cursor["next"])
        clients = clients[start:] + clients[:start]
    deadline = time.monotonic() + timeout
    error = "No client of this PC's displays is paired with it"
    for index, client in enumerate(clients):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        cursor["next"] = clients[(index + 1) % len(clients)]
        try:
            code, out, err = L.run_stream("kwin-windows", host["address"], "--id", client, timeout=remaining)
        except subprocess.TimeoutExpired:
            error = "KWin Window Stream didn't answer in time"
            if index:  # cut short by the ones before it: it goes first next time
                cursor["next"] = client
            break
        if code:
            lines = (err or out).strip().splitlines()
            error = lines[-1] if lines else f"ft-stream kwin-windows exited with {code}"
            continue
        try:
            windows = json.loads(out.strip().splitlines()[-1])["windows"]
        except (ValueError, KeyError, IndexError, TypeError):
            error = "KWin Window Stream's list didn't read"
            continue
        if not isinstance(windows, list):
            error = "KWin Window Stream's list didn't read"
            continue
        cursor["next"] = client
        return client, [w for w in windows if isinstance(w, dict)]
    raise RuntimeError(error)


def host_named(layout, name):
    host = next((h for h in layout.get("hosts", []) if h.get("name") == name), None)
    if host is None:
        raise RuntimeError(f"no host called {name}")
    return host


def size_of(text):
    if not re.fullmatch(r"[1-9][0-9]{1,3}x[1-9][0-9]{1,3}", text.lower()):
        raise RuntimeError(f"the size is WxH, not {text!r}")
    w, h = (int(v) for v in text.lower().split("x"))
    return list(L.stream_size(min(max(w, 64), 8192), min(max(h, 64), 8192)))


def kwin_add(args):
    """kwin-add HOST WINDOW [--label TEXT] [--size WxH] [--client NAME]: the window's panel,
    saved and started in front of you; the one there already is connected again."""
    opts = dict(zip(args[2::2], args[3::2]))
    if len(args) % 2 or set(opts) - {"--label", "--size", "--client"}:
        raise RuntimeError("remote kwin-add HOST WINDOW [--label TEXT] [--size WxH] [--client NAME]")
    layout = L.load_layout()
    host = host_named(layout, args[0])
    try:
        app = app_of(args[1])
    except ValueError as e:
        raise RuntimeError(str(e)) from None
    there = next((d for d in host.get("displays", []) if d.get("app") == app), None)
    if there is not None:
        L.remote_command(["connect", there["id"]])
        return print(there["id"])
    client = opts.get("--client")
    if client not in host_clients(host):
        client, _ = find_client(host, client)
    if len(L.remote_displays(layout)) >= L.REMOTE_MAX:
        raise RuntimeError(f"at most {L.REMOTE_MAX} remote displays and windows")
    label = printable(opts.get("--label", "")) or "Window"
    size = size_of(opts["--size"]) if "--size" in opts else [1280, 800]
    ids = {d["id"] for _, _, d in L.remote_displays(layout)}
    display_id = base = L.slug(f"{host.get('name') or host['address']}-{label}")
    for k in range(2, 100):
        if display_id not in ids:
            break
        display_id = f"{base}-{k}"
    host.setdefault("displays", []).append({
        "id": display_id, "client": client, "app": app, "label": label, "size": size, "fps": 60, "bitrate": 0,
        "metres": round(max(0.4, size[0] / WINDOW_PIXELS_PER_METRE), 3), "pos": [0.0, -0.1, -WINDOW_DISTANCE]})
    L.remote_displays(layout)  # gives it a screen number
    L.save_layout(layout)
    L.log(f"added {display_id}: {label} from {host.get('name')}, with {client}'s pairing")
    print(display_id)
    L.start_remotes_now(layout, {display_id})


def kwin_size(args):
    """kwin-size N WxH METRES (from ft-screens): the PC resized the window on panel N, and the
    panel kept its density: the layout keeps its new size and width."""
    layout = L.load_layout()
    found = next(((h, d) for n, h, d in L.remote_displays(layout) if str(n) == args[0]), None)
    if found is None or not KWIN_APP.fullmatch(str(found[1].get("app", ""))):
        raise RuntimeError(f"kwin-size: screen {args[0]} isn't a KWin window's panel")
    try:
        metres = float(args[2])
    except ValueError:
        metres = math.nan
    if not (math.isfinite(metres) and 0.15 <= metres <= 12):
        raise RuntimeError(f"kwin-size: the width is 0.15 to 12 metres, not {args[2]!r}")
    _, d = found
    d["size"], d["metres"] = size_of(args[1]), round(metres, 4)
    L.save_layout(layout)
    L.log(f"{d['id']}: the PC's window is {d['size'][0]}x{d['size'][1]} now, {d['metres']} m wide")


def command(what, args):
    """ft-layout remote kwin-windows HOST | kwin-add HOST WINDOW ... | kwin-size N WxH METRES."""
    if what == "kwin-windows" and len(args) == 1:
        client, windows = find_client(host_named(L.load_layout(), args[0]))
        return print(json.dumps({"client": client, "windows": windows}))
    if what == "kwin-add" and len(args) >= 2:
        return kwin_add(args)
    if what == "kwin-size" and len(args) == 3:
        return kwin_size(args)
    raise RuntimeError("remote kwin-windows HOST | kwin-add HOST WINDOW [--label TEXT] [--size WxH] [--client NAME]"
                       " | kwin-size N WxH METRES")
