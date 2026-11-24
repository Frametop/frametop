"""ft-layout window: a PC's app windows as panels (DEE-347, DEE-350; docs/remote-displays.md).

The host's Frametop windows instance (a Vibepollo on its own port) lists its windows and apps,
opens one onto an output of its own, resizes it, finds the window under a point and puts it
back. Each opened window becomes a remote display of that host (ft_layout's), streamed by its
own client from the host's pool. ft_layout keeps the layout, the screens and the streams; this
module only speaks to the host's windows API and turns its answers into those displays.
"""
import contextlib
import fcntl
import json
import math
import re

import ft_layout as L
import ft_hostapi  # noqa: E402  (questions to a PC's Vibepollo Web UI API; on the path ft_layout sets)
import ft_windowscale

WINDOW_SIZE = (1280, 800)  # a launched app's first size
WINDOW_MAX = (1920, 1200)  # an opened window's first size at most: a tiled one can be 5096 wide
# A PC app a window launch can start: a Linux desktop file's id, a Windows Start menu
# shortcut, its path under the Programs folder ("Accessories/Paint.lnk"), or a Windows Store
# app's AUMID ("Microsoft.WindowsCalculator_8wekyb3d8bbwe!App").
WINDOW_APP = re.compile(r"[A-Za-z0-9._@+-]+\.desktop|[^\x00-\x1f\\:*?\"<>|]{1,240}\.lnk"
                        r"|[A-Za-z0-9.-]{1,200}_[a-z0-9]{13}![A-Za-z0-9._-]{1,100}")


def window_app_title(app, opened=None):
    """A PC app's name for its panel: its window's title when the PC gave one (`opened`, the
    open answer), else from its id: "org.gnome.Calculator", "Character Map",
    "Microsoft.WindowsCalculator"."""
    title = (opened or {}).get("title")
    title = printable(title) if isinstance(title, str) else ""
    if title:
        return title
    app = re.sub(r"_[a-z0-9]{13}!.*$", "", app)
    return re.sub(r"\.(desktop|lnk)$", "", app.rsplit("/", 1)[-1])
WINDOW_DISTANCE = 1.1      # metres in front of you, a little below eye height
WINDOW_PIXELS_PER_METRE = 1400
WINDOW_RESIZE_MIN = (320, 240)    # a window panel resized by hand (the host's own floor)
WINDOW_RESIZE_MAX = (3840, 2160)  # ... at most: the Frame's decoder, next to the other streams


WINDOW_ADDRESS = re.compile(r"0x[0-9a-fA-F]{1,16}")   # a window as the host names it


def address_of(d):
    """The window a panel shows, as its PC names it."""
    return d["app"][len("window:"):]


def number(value, default, low, high):
    """A number from a PC's answer, within [low, high]; its default when the PC sent something
    else (a PC's odd answer: its default, not a traceback)."""
    try:
        x = float(value)
    except (TypeError, ValueError):
        return default
    return min(max(x, low), high) if math.isfinite(x) else default


def printable(text, most=80):
    """A host's text for a panel's label, on one line: a window's title is the PC's to choose."""
    return "".join(c for c in str(text) if c.isprintable()).strip()[:most]


def window_host(layout, name):
    host = next((h for h in layout.get("hosts", []) if h.get("name") == name), None)
    if host is None:
        raise RuntimeError(f"no host called {name!r}")
    port = host.get("windows", {}).get("port") if isinstance(host.get("windows"), dict) else None
    if not isinstance(port, int):
        raise RuntimeError(f"{name} has no Frametop windows instance (ft-layout remote host {name} windows=PORT)")
    return host, port


def window_api(host, port, method, path, body=None):
    """The host's Frametop windows instance: its JSON answer, or RuntimeError with its why."""
    if method == "GET":
        status, answer = ft_hostapi.request(host["address"], "GET", path, timeout=5.0, port=port)
    else:
        status, answer = ft_hostapi.post_json(host["address"], path, body, timeout=25.0, port=port)
    if status == 200 and isinstance(answer, dict):
        return answer
    why = answer.get("error") if isinstance(answer, dict) else None
    if not status:
        why = why or f"no answer (a token in {ft_hostapi.token_path(host['address'], port)}?)"
    raise RuntimeError(f"{host.get('name') or host['address']}: {why or f'HTTP {status}'}")


def window_client(layout, host, port):
    """A client of the host's window pool not streaming a window now, paired with its windows
    instance. Each window streams as its own client; the pool keeps the host's client list short."""
    used = {d.get("client") for d in host.get("displays", []) if d.get("window")}
    base = f"frametop-{L.slug(host.get('name') or host['address'])}-win"
    for k in range(1, L.REMOTE_MAX + 1):
        client = f"{base}{k}"
        if client in used:
            continue
        code, out, err = L.run_stream("pair", f"{host['address']}:{port}", "--id", client, timeout=30)
        if code:
            raise RuntimeError(f"pairing {client} with {host['address']}:{port} failed: {(err or out).strip()}")
        return client
    raise RuntimeError(f"at most {L.REMOTE_MAX} windows")


def window_size(opts, default):
    if "--size" not in opts:
        return L.stream_size(*default)
    if not re.fullmatch(r"\d+x\d+", opts["--size"].lower()):
        raise RuntimeError(f"window: --size is WxH, not {opts['--size']!r}")
    w, h = (int(v) for v in opts["--size"].lower().split("x"))
    return L.stream_size(min(max(w, 320), 7680), min(max(h, 240), 4320))


def window_opening(host, answer, app, size):
    """(size, fields) of a window opening there, with the scale its app starts at: window_add's, and
    the open's "scale" (`answer`: the PC's window list)."""
    return ft_windowscale.opening(host, answer, app, size, WINDOW_RESIZE_MIN, WINDOW_RESIZE_MAX)


def window_launch(host, port, app, size, fields):
    """The PC starts `app` in a window of its own there: its answer (address, title)."""
    body = {"app": app, "width": size[0], "height": size[1]}
    if "scale" in fields:
        body["scale"] = fields["scale"]
    return window_api(host, port, "POST", "/api/frametop/windows/open", body)


def window_add(layout, host, port, address, title, size, fields=None):
    """The window's panel, saved and started in front of you (or at fields' pos, face and roll:
    desktopless mode, which keeps its app's "key" there too)."""
    if not WINDOW_ADDRESS.fullmatch(str(address)):
        raise RuntimeError(f"{host.get('name') or host['address']} named no window ({address!r})")
    for _, h, d in L.remote_displays(layout):
        if h is host and d.get("window") and d["app"] == f"window:{address}":
            if d.pop("off", None):   # (one disconnected: connected again)
                L.save_layout(layout)
            n = next(k for k, _, e in L.remote_displays(layout) if e is d)
            try:
                running = L.remotes_running(L.screens_socket())
            except L.NoAnswer:
                running = None   # (no desktop: nothing to start it in)
            if running is not None and n not in running:
                L.start_remotes_now(layout, {d["id"]})   # its stream had stopped: the panel opens again
            return d  # already a panel
    if len(L.remote_displays(layout)) >= L.REMOTE_MAX:
        raise RuntimeError(f"at most {L.REMOTE_MAX} remote displays and windows")
    ids = {d["id"] for _, _, d in L.remote_displays(layout)}
    display_id = base = L.slug(f"{host.get('name') or host['address']}-window-{address[2:]}")
    for k in range(2, 100):
        if display_id not in ids:
            break
        display_id = f"{base}-{k}"
    w, h = size
    metres = round(max(0.4, w / WINDOW_PIXELS_PER_METRE), 3)
    d = {"id": display_id, "client": window_client(layout, host, port), "app": f"window:{address}",
         "label": printable(title) or address, "size": [w, h], "fps": 60, "bitrate": 0, "port": port, "window": True,
         "metres": metres, "pos": [0.0, -0.1, -WINDOW_DISTANCE]}
    d.update({k: v for k, v in (fields or {}).items() if v is not None})
    host.setdefault("displays", []).append(d)
    L.remote_displays(layout)
    L.save_layout(layout)
    L.start_remotes_now(layout, {display_id})
    return d


def window_forget(layout, host, d):
    """The window panel goes from the desktop and the layout, and from the profiles: a later
    window with its id starts fresh."""
    n = next(n for n, h, e in L.remote_displays(layout) if e is d)
    try:
        L.screens_socket().ask(f"remote {n} stop")
    except RuntimeError:
        pass
    host["displays"].remove(d)
    for profile in layout.get("profiles", {}).values():
        profile.get("remote", {}).pop(d["id"], None)
    L.save_layout(layout)


def window_close(layout, host, d):
    """The window panel goes, and its window goes back on the host now: the host's minute of
    grace is for a stream that drops, not for one the Frame ended on purpose."""
    window_forget(layout, host, d)
    try:
        window_api(host, int(d.get("port") or 0), "POST", "/api/frametop/windows/release",
                   {"address": address_of(d)})
        L.log(f"closed {d['id']}: the window is back on {host.get('name') or host['address']}")
    except RuntimeError as e:
        if "no window output" in str(e):  # closed on the PC, or back after the grace period
            L.log(f"closed {d['id']}: the window had already left it")
        else:
            L.log(f"closed {d['id']}; the PC will put the window back within a minute ({e})")


def window_prune(layout, host, windows):
    """Forgets the host's window panels whose window closed there (ft-screens already dropped
    their panels: the host gives a window a new address each time), and ones disconnected
    ("off"): a window panel has nothing to come back to (its window went back on the PC), so
    one left off by an older build kept the window from being opened or pulled again
    (Daenon, 2026-11-07: Brave wouldn't pull off oma7's display, GrokBot would)."""
    have = {w.get("address") for w in windows}
    for d in [d for d in host.get("displays", []) if d.get("window") and (address_of(d) not in have or d.get("off"))]:
        window_forget(layout, host, d)
        why = "it was disconnected" if address_of(d) in have else "its window closed"
        L.log(f"forgot {d['id']}: {why} on {host.get('name') or host['address']}")


def fractions(words, what):
    """Numbers from 0 to 1, or RuntimeError naming the command."""
    try:
        values = [float(w) for w in words]
    except ValueError:
        values = []
    if len(values) != len(words) or not all(0.0 <= v <= 1.0 for v in values):
        raise RuntimeError(f"window {what}: the fractions go from 0 to 1, not {' '.join(words)!r}")
    return values


def numbered(layout, word, what):
    """(host, display) of remote screen number `word`."""
    if not word.isdecimal():
        raise RuntimeError(f"window {what}: {word!r} isn't a screen number")
    found = next(((h, d) for n, h, d in L.remote_displays(layout) if n == int(word)), None)
    if found is None:
        raise RuntimeError(f"window {what}: no remote display or window panel {word}")
    return found


def window_drop(layout, args):
    """Window drag (DEE-345): a window panel let go on (or pushed into) one of its own PC's
    displays goes back onto that monitor where it was let go, not where it came from."""
    host, d = numbered(layout, args[0], "drop")
    onto_host, onto = numbered(layout, args[1], "drop")
    u, v, fx, fy = fractions(args[2:6], "drop")
    if not d.get("window"):
        raise RuntimeError(f"window drop: {d['id']} isn't a window")
    if onto_host is not host or not str(onto.get("app", "")).startswith("display:"):
        raise RuntimeError(f"window drop: {onto['id']} isn't one of {host.get('name') or host['address']}'s displays")
    monitor = onto["app"][8:]
    window_forget(layout, host, d)
    try:
        window_api(host, int(d.get("port") or 0), "POST", "/api/frametop/windows/release",
                   {"address": address_of(d), "monitor": monitor, "x": u, "y": v, "grab_x": fx, "grab_y": fy})
        L.log(f"dropped {d['id']} onto {monitor} at {u:.2f},{v:.2f}")
    except RuntimeError as e:
        if "no window output" in str(e):
            L.log(f"dropped {d['id']}: the window had already left it")
        else:
            L.log(f"dropped {d['id']}; the PC will put the window back within a minute ({e})")
            raise


@contextlib.contextmanager
def taking_turns():
    """A resize or a scale at a time (a wheel's next step waits for the PC's answer to the last one)."""
    with open(L.LOCK_PATH + ".window-size", "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        yield


def fit_scale(d):
    """A window panel given a size: the nearest that takes its scale, if it has one."""
    scale = ft_windowscale.valid(d.get("scale"))
    if scale:
        ft_windowscale.shape(d, tuple(d["size"]), scale, WINDOW_RESIZE_MIN, WINDOW_RESIZE_MAX)


def window_resize(args):
    """A window panel's corner dragged (user, 2026-11-07: "resize and change aspect ratio"): the
    window on the PC gets that many pixels, and the stream starts over at that size. ft-stream
    asks for the window at the panel's size on every start, which resizes the host's output."""
    layout = L.load_layout()
    host, d = numbered(layout, args[0], "resize")
    if not d.get("window"):
        raise RuntimeError(f"window resize: {d['id']} isn't a window")
    if not re.fullmatch(r"[1-9][0-9]{0,4}x[1-9][0-9]{0,4}", args[1].lower()):
        raise RuntimeError(f"window resize: the size is WxH, not {args[1]!r}")
    try:
        metres = float(args[2])
    except ValueError:
        metres = math.nan
    if not (math.isfinite(metres) and 0.15 <= metres <= 12):
        raise RuntimeError(f"window resize: the width is 0.15 to 12 metres, not {args[2]!r}")
    w, h = (int(v) for v in args[1].lower().split("x"))
    w = min(max(w, WINDOW_RESIZE_MIN[0]), WINDOW_RESIZE_MAX[0])
    h = min(max(h, WINDOW_RESIZE_MIN[1]), WINDOW_RESIZE_MAX[1])
    w, h = d["size"] = list(L.stream_size(w, h))
    fit_scale(d)
    d["metres"] = round(metres * d["size"][0] / w, 4)   # (a few pixels more or less: the same density)
    L.save_layout(layout)
    n = int(args[0])
    L.log(f"resized {d['id']} to {d['size'][0]}x{d['size'][1]}, {d['metres']} m wide")
    if d.get("off"):
        return  # disconnected: its next connection has the size
    try:
        L.start_remote(L.screens_socket(), n, host, d)   # (ft-screens starts its stream over: the size changed)
    except RuntimeError as e:
        L.log(f"saved; not applied now: {e}")


def window_scale(args):
    """A window panel's scale (its bar's menu, Meta+scroll): the PC draws the app at it in the same
    pixels, so the stream goes on. Where the panel's size doesn't take that scale, the size moves a
    few pixels and the stream starts over. Kept for the app's next window on that PC."""
    layout = L.load_layout()
    host, d = numbered(layout, args[0], "scale")
    now = ft_windowscale.valid(d.get("scale")) if d.get("window") else None
    if not now:
        raise RuntimeError(f"window scale: {d['id']} isn't a window whose PC takes a scale")
    try:
        scale = ft_windowscale.valid(float(args[1]))
    except ValueError:
        scale = None
    if not scale:
        raise RuntimeError(f"window scale: a scale is {ft_windowscale.LIMITS[0]:g} to {ft_windowscale.LIMITS[1]:g}, not {args[1]!r}")
    n, before = int(args[0]), list(d["size"])
    ft_windowscale.shape(d, tuple(d.get("asked_size") or before), scale, WINDOW_RESIZE_MIN, WINDOW_RESIZE_MAX)
    live = d["size"] == before and not d.get("off")
    if live:
        try:
            # (with its size: a stream still starting over at it has the PC at the last one)
            answer = window_api(host, int(d.get("port") or 0), "POST", "/api/frametop/windows/resize",
                                {"address": address_of(d), "width": before[0], "height": before[1], "scale": d["scale"]})
        except RuntimeError:
            tell_scale(n, {"scale": now})   # (its menu goes back to what the PC has)
            raise
        scale = round(ft_windowscale.valid(answer.get("scale")) or d["scale"], 6)
        # The PC took its time: the layout as it is now, where a panel may have opened, or this one closed.
        layout = L.load_layout()
        found = next(((h, e) for _, h, e in L.remote_displays(layout) if (e["id"], e["app"]) == (d["id"], d["app"])), None)
        if found is None:
            L.log(f"{d['id']}: closed meanwhile")
            return
        host, d = found
        d["scale"] = scale
    ft_windowscale.keep(host, d.get("pc_app"), d["scale"])
    L.save_layout(layout)
    L.log(f"{d['id']}: scale {d['scale'] * 100:.0f}%" + ("" if d["size"] == before else f" at {d['size'][0]}x{d['size'][1]}"))
    if d.get("off"):
        return
    if live:
        tell_scale(n, d)
        return
    try:
        L.start_remote(L.screens_socket(), n, host, d)   # (its size changed: ft-stream opens it with both)
    except RuntimeError as e:
        L.log(f"saved; not applied now: {e}")


def tell_scale(n, d):
    """Panel n's scale to ft-screens, for its menu."""
    try:
        L.screens_socket().ask(ft_windowscale.told(n, d))
    except RuntimeError as e:
        L.log(f"screen {n}'s scale isn't in its menu: {e}")


def window_pull(layout, args):
    """Window drag (DEE-345): the window under a point of a PC's display, as a panel; ft-screens
    hears where it was held (with SEQ, its number for the pull), so the laser that pulled it
    carries it on."""
    seq = None
    if len(args) == 4:
        if not re.fullmatch(r"[1-9][0-9]{0,9}", args[3]):
            raise RuntimeError(f"window pull: SEQ is a number from 1, not {args[3]!r}")
        seq = int(args[3])
    host, d = numbered(layout, args[0], "pull")
    u, v = fractions(args[1:3], "pull")
    if not str(d.get("app", "")).startswith("display:"):
        raise RuntimeError(f"window pull: {d['id']} isn't a PC's display")
    _, port = window_host(layout, host.get("name"))
    hit = window_api(host, port, "POST", "/api/frametop/windows/at", {"monitor": d["app"][8:], "x": u, "y": v})
    address = str(hit.get("address", ""))
    if not WINDOW_ADDRESS.fullmatch(address):
        raise RuntimeError(f"window pull: the PC named no window ({address!r})")
    answer = window_api(host, port, "GET", "/api/frametop/windows")
    window_prune(layout, host, answer.get("windows", []))
    listed = next((w for w in answer.get("windows", []) if w.get("address") == address), hit)

    size = window_size({}, (int(number(hit.get("width") or None, WINDOW_SIZE[0], 1, WINDOW_MAX[0])),
                            int(number(hit.get("height") or None, WINDOW_SIZE[1], 1, WINDOW_MAX[1]))))
    size, fields = window_opening(host, answer, ft_windowscale.app_of(listed), size)
    panel = window_add(layout, host, port, address, str(hit.get("title") or hit.get("class") or ""), size, fields)
    grab = hit.get("grab") if isinstance(hit.get("grab"), list) and len(hit["grab"]) == 2 else [0.5, 0.5]
    fx, fy = (number(g, 0.5, 0.0, 1.0) for g in grab)
    n = next(k for k, _, e in L.remote_displays(layout) if e is panel)
    try:
        # SEQ: ft-screens' number for this pull, so an answer to an earlier one isn't taken for it
        if seq is not None:
            L.screens_socket().ask(f"pulled {n} {fx:.4f} {fy:.4f} {seq}")
    except (L.NoAnswer, RuntimeError) as e:
        L.log(f"pulled {panel['id']}; ft-screens didn't take it ({e}): it opened in front of you")
    L.log(f"pulled {address} off {d['app'][8:]} as {panel['id']} (screen {n})")
    print(panel["id"])


def window_command(args):
    """ft-layout window list|apps|open|launch|close (see the usage)."""
    what = args[0] if args else ""
    if what in ("list", "apps") and len(args) == 2:
        layout = L.load_layout()
        host, port = window_host(layout, args[1])
        if what == "apps":
            for app in window_api(host, port, "GET", "/api/frametop/windows/apps").get("apps", []):
                print(f"{app.get('id')}  {app.get('name')}")
            return
        answer = window_api(host, port, "GET", "/api/frametop/windows")
        window_prune(layout, host, answer.get("windows", []))
        panels = {address_of(d): d["id"] for d in host.get("displays", []) if d.get("window")}
        if answer.get("locked"):
            print(f"{host['name']} is locked: window panels stay black until it's unlocked")
        for w in answer.get("windows", []):
            out = w.get("output") or "-"
            panel = panels.get(w.get("address"), "-")
            title = "".join(c if c.isprintable() else " " for c in str(w.get("title", "")))  # a page's title, say
            print(f"{w.get('address')} {w.get('width')}x{w.get('height')} {out} {panel} {w.get('class')}: {title}")
        return
    if what == "catalog" and len(args) == 2:
        # One JSON line for frunner: the host's apps, and its windows with their panels' ids and screens.
        layout = L.load_layout()
        host, port = window_host(layout, args[1])
        apps = window_api(host, port, "GET", "/api/frametop/windows/apps").get("apps", [])
        answer = window_api(host, port, "GET", "/api/frametop/windows")
        window_prune(layout, host, answer.get("windows", []))
        panels = {address_of(d): (n, d["id"]) for n, h, d in L.remote_displays(layout) if h is host and d.get("window")}
        windows = []
        for w in answer.get("windows", []):
            n, panel = panels.get(w.get("address"), (None, None))
            windows.append({k: w.get(k) for k in ("address", "class", "title", "width", "height", "output")} |
                           {"panel": panel, "screen": n})
        print(json.dumps({"host": host.get("name"), "locked": bool(answer.get("locked")),
                          "apps": [{k: a.get(k) for k in ("id", "name", "icon")} for a in apps], "windows": windows}))
        return
    if what in ("open", "launch") and len(args) >= 3:
        layout = L.load_layout()
        host, port = window_host(layout, args[1])
        rest = args[3:]
        opts = dict(zip(rest[0::2], rest[1::2]))
        if set(opts) - {"--size"} or len(rest) % 2:
            raise RuntimeError(f"window {what}: the option is --size WxH")
        if what == "open":
            address = args[2]
            if not WINDOW_ADDRESS.fullmatch(address):
                raise RuntimeError("window open: ADDRESS is a window's address (0x...: window list)")
            answer = window_api(host, port, "GET", "/api/frametop/windows")
            windows = answer.get("windows", [])
            window_prune(layout, host, windows)  # frees their clients
            found = next((w for w in windows if w.get("address") == address), None)
            if found is None:
                raise RuntimeError(f"{host.get('name')} has no window {address}")
            size = window_size(opts, (int(number(found.get("width") or None, WINDOW_SIZE[0], 1, WINDOW_MAX[0])),
                                      int(number(found.get("height") or None, WINDOW_SIZE[1], 1, WINDOW_MAX[1]))))
            size, fields = window_opening(host, answer, ft_windowscale.app_of(found), size)
            title = found.get("title") or found.get("class") or ""
        else:
            if not WINDOW_APP.fullmatch(args[2]):
                raise RuntimeError("window launch: APP is an app's id from window apps")
            size = window_size(opts, WINDOW_SIZE)
            answer = window_api(host, port, "GET", "/api/frametop/windows")
            window_prune(layout, host, answer.get("windows", []))
            size, fields = window_opening(host, answer, args[2], size)
            opened = window_launch(host, port, args[2], size, fields)
            address = str(opened.get("address", ""))
            title = window_app_title(args[2], opened)
        d = window_add(layout, host, port, address, title, size, fields)
        print(d["id"])
        return
    if what == "drop" and len(args) == 7:
        window_drop(L.load_layout(), args[1:])
        return
    if what == "pull" and len(args) in (4, 5):
        window_pull(L.load_layout(), args[1:])
        return
    if what == "resize" and len(args) == 4:
        with taking_turns():
            window_resize(args[1:])
        return
    if what == "scale" and len(args) == 3:
        with taking_turns():
            window_scale(args[1:])
        return
    if what == "close" and len(args) == 2:
        layout = L.load_layout()
        _, host, d = L.find_display(layout, args[1])
        if not d.get("window"):
            raise RuntimeError(f"{args[1]} isn't a window (ft-layout remote remove)")
        window_close(layout, host, d)
        return
    raise RuntimeError("window list HOST | apps HOST | catalog HOST | open HOST ADDRESS [--size WxH] | "
                       "launch HOST APP [--size WxH] | close ID | drop N M U V FX FY | pull N U V [SEQ] | resize N WxH METRES | "
                       "scale N SCALE")
