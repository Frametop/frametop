"""A PC's monitors following its display panels (docs/remote-displays.md, "A PC's monitors").

With a host's "monitors" switch on ("Let Frametop control NAME's displays" in Frametop
Settings; ft-layout remote host NAME monitors=on), Frametop's build of Vibepollo on that PC gets
a plan made from the host's display panels: each monitor with a connected panel stays on at
its panel's stream size (the PC picks a mode of that size, or makes one on Hyprland) and the
scale picked for it (`scale`, else its own), and every other monitor (no panel here, or its
panel disconnected) goes off. The PC applies it only while one of those panels streams, and
puts every monitor back two minutes after the last one ends (/api/frametop/monitors). A size
its monitor refuses (one bigger than its panel, or any made mode on some drivers) is in the
PC's answer as its `error` and in that monitor's `refused`: the monitor stays as it was.

ft_layout sends the plan whenever the panels change (connect, disconnect, set, remove, and
before it starts their streams). Nothing here stops a stream: a PC that doesn't answer, or
can't do it, is logged and its panels stream as before.
"""
import ft_hostapi  # conf/, on ft_layout's path

PATH = "/api/frametop/monitors"


def controls(host):
    """Whether Frametop turns the host's monitors on and off and sets their modes."""
    return bool(host.get("monitors")) and host.get("kind", "vibepollo") == "vibepollo"


def monitor_of(d):
    """The id of the PC monitor a display panel streams ("display:ID"), or None: a virtual
    display, a window panel, an app."""
    app = str(d.get("app", ""))
    return app[len("display:"):] if app.startswith("display:") and not d.get("window") else None


def mode_of(d):
    """The mode picked for a display panel with ft-layout's mode= (it pins a refresh rate),
    {width, height, refresh_mhz}, or None: its stream size is its monitor's (size_of)."""
    mode = d.get("mode")
    if not isinstance(mode, dict):
        return None
    try:
        w, h, r = int(mode["width"]), int(mode["height"]), int(mode["refresh_mhz"])
    except (KeyError, TypeError, ValueError):
        return None
    return {"width": w, "height": h, "refresh_mhz": r} if w > 0 and h > 0 and r > 0 else None


def size_of(d):
    """A display panel's stream size as the size its monitor is asked to be: a mode with refresh
    0, which the PC makes a mode (its own one when that's its own size)."""
    try:
        w, h = (int(v) for v in d.get("size", []))
    except (TypeError, ValueError):
        return None
    return {"width": w, "height": h, "refresh_mhz": 0} if w > 0 and h > 0 else None


def listed_sizes(m):
    """The stream sizes the PC's monitor (as it answered) lists a mode in, [(width, height)],
    largest first; height by width for one turned a quarter."""
    try:
        sizes = {(int(mode["width"]), int(mode["height"])) for mode in m.get("modes") or []}
    except (KeyError, TypeError, ValueError):
        return []
    turned = [(h, w) if m.get("sideways") else (w, h) for w, h in sizes if w > 0 and h > 0]
    return sorted(turned, key=lambda s: (s[0] * s[1], s[0]), reverse=True)


def largest_size(m):
    """The largest stream size the PC's monitor can be put at, (width, height), or None when it
    lists no modes: its widest and its tallest listed mode. A PC that makes modes (Hyprland)
    makes smaller ones; a bigger one than its panel has it takes and the panel refuses."""
    sizes = listed_sizes(m)
    return (max(s[0] for s in sizes), max(s[1] for s in sizes)) if sizes else None


def cant_be(m, width, height):
    """Whether the PC's monitor can't be put at a stream size, so a panel of that size crops or
    fits it as it is: a size it didn't go into when asked (its `refused`: a made mode its
    driver refuses), or one it doesn't list on a PC that makes no modes (Windows, KDE)."""
    w, h = (height, width) if m.get("sideways") else (width, height)  # modes list unrotated
    if any((x.get("width"), x.get("height")) == (w, h) for x in m.get("refused") or []):
        return True
    return bool(m.get("modes")) and "any_mode" in m and not _takes(m, {"width": w, "height": h, "refresh_mhz": 0})


SCALES = (0.5, 4.0)  # the scales the PC takes in a plan


def scale_of(d):
    """The scale picked for a display panel's monitor (1.25: 125%), or None (its own)."""
    scale = d.get("scale")
    if isinstance(scale, bool) or not isinstance(scale, (int, float)):
        return None
    return float(scale) if SCALES[0] <= scale <= SCALES[1] else None


def wishes(host):
    """{monitor id: {on, mode?, scale?}} from the host's display panels. A monitor with two
    panels is on when either is connected."""
    out = {}
    for d in host.get("displays", []):
        monitor = monitor_of(d)
        if monitor is None:
            continue
        on = not d.get("off")
        if out.get(monitor, {}).get("on") and not on:
            continue
        wish = {"on": on}
        if on and (mode_of(d) or size_of(d)):
            wish["mode"] = mode_of(d) or size_of(d)
        if on and scale_of(d):
            wish["scale"] = scale_of(d)
        out[monitor] = wish
    return out


def plan_body(host, listed):
    """The POST body for the host, or None when there is nothing to send: no display panel is
    connected (the PC's grace then runs as it would). listed: the PC's monitors as it answered
    ({id: monitor}); a panel of a monitor it doesn't have (unplugged), a mode or size it can't
    take, or a scale it can't set (Windows, an older build) is left out, so the rest of the plan
    still takes: the stream then crops or fits the monitor as it is."""
    want = wishes(host)
    if not any(w["on"] for w in want.values()):
        return None
    monitors = {}
    for monitor, wish in want.items():
        m = listed.get(monitor)
        if m is None:
            continue
        mode = wish.get("mode")
        if mode and not mode["refresh_mhz"] and m.get("sideways"):
            mode = wish["mode"] = dict(mode, width=mode["height"], height=mode["width"])  # modes list unrotated
        if mode and not _takes(m, mode):
            wish = {k: v for k, v in wish.items() if k != "mode"}
        if "scale" in wish and m.get("on") and not m.get("scales"):
            wish = {k: v for k, v in wish.items() if k != "scale"}
        monitors[monitor] = wish
    if not any(w["on"] for w in monitors.values()):
        return None  # only unplugged monitors have connected panels: leave the PC as it is
    return {"monitors": monitors, "others_off": True}


def _takes(m, mode):
    """Whether the PC's monitor takes the mode, or (refresh 0) a mode of that size: one it
    lists, or any on a PC that makes them (Hyprland). An older build takes no size."""
    modes = m.get("modes") or []
    if not mode["refresh_mhz"]:
        return "any_mode" in m and (m["any_mode"] or any(
            (x.get("width"), x.get("height")) == (mode["width"], mode["height"]) for x in modes))
    return m.get("any_mode") or not modes or any(_same_mode(mode, x) for x in modes)


def _same_mode(a, b):
    """As the PC matches modes: Hyprland lists 239.76 Hz for the 239.761 it reports."""
    try:
        return (int(a["width"]) == int(b["width"]) and int(a["height"]) == int(b["height"])
                and abs(int(a["refresh_mhz"]) - int(b["refresh_mhz"])) <= 10)
    except (KeyError, TypeError, ValueError):
        return False


def port_of(host):
    """The port of the host's Vibepollo (its display panels'; a window panel's is its Frametop
    windows instance), 0 for the default."""
    return next((int(d["port"]) for d in host.get("displays", []) if d.get("port") and monitor_of(d)), 0)


def ask(host, timeout=3.0, request=None):
    """(HTTP status, answer) of the host's GET: its monitors, their modes and its plan."""
    return (request or ft_hostapi.request)(host["address"], "GET", PATH, timeout=timeout, port=port_of(host))


def sync(host, log, request=None):
    """Sends the host's plan made from its panels, if it controls its monitors. Never raises:
    what went wrong goes to log()."""
    if not controls(host):
        return
    request = request or ft_hostapi.request
    name = host.get("name") or host.get("address")
    try:
        status, answer = ask(host, timeout=10.0, request=request)  # a Wi-Fi stall outlasts 3 s
        if status != 200 or not isinstance(answer, dict):
            return log(f"{name}: monitors not changed (it answered {status})")
        listed = {m.get("id"): m for m in answer.get("monitors", []) if isinstance(m, dict)}
        body = plan_body(host, listed)
        if body is None:
            return
        status, answer = request(host["address"], "POST", PATH, body=body, timeout=20.0, port=port_of(host))
        error = answer.get("error") if isinstance(answer, dict) else None
        if status != 200:
            log(f"{name}: monitors not changed: {error or f'it answered {status}'}")
        elif error and answer.get("applies"):
            log(f"{name}: {error}")  # taken, and applied now: a size its monitor refused, one kept on
    except (OSError, ValueError) as e:
        log(f"{name}: monitors not changed: {e}")


def _drop_plan(host, request=None):
    """Asks the PC for no plan: every monitor back as it was, now. Why it didn't take it, as the
    log goes on (" (it answered 500)", ": timed out"), or "" when it did."""
    request = request or ft_hostapi.request
    try:
        status, _ = request(host["address"], "POST", PATH, body={"monitors": {}}, timeout=20.0, port=port_of(host))
        return "" if status == 200 else f" (it answered {status})"
    except (OSError, ValueError) as e:
        return f": {e}"


def release(host, log, request=None):
    """The switch turned off: no plan, every monitor back as it was. False when the PC didn't
    take it: ft_layout then keeps `monitors_release` on the host, and sync_all tries again."""
    if host.get("kind", "vibepollo") != "vibepollo":
        return True
    why = _drop_plan(host, request)
    if why:
        log(f"{host.get('name')}: its monitors weren't put back{why}; asked again before its next display stream")
    return not why


def put_back_all(hosts, log, request=None):
    """The headset came off (ft_remoteaway): each host that controls its monitors puts them back
    now, all at once, not two minutes after its streams ended. Its plan goes with that; sync
    sends it again before the streams start. One that doesn't take it still puts them back
    after its own two minutes."""
    import concurrent.futures
    hosts = [h for h in {id(h): h for h in hosts}.values() if controls(h)]
    if not hosts:
        return

    def one(h):
        why = _drop_plan(h, request)
        if why:
            log(f"{h.get('name')}: its monitors weren't put back now{why}; they go back two minutes after its streams ended")
    with concurrent.futures.ThreadPoolExecutor(len(hosts)) as pool:
        list(pool.map(one, hosts))


def owes_release(host):
    """The switch went off while the PC didn't take it: its plan is still there to take back
    (else its panels' streams would go on turning its monitors off)."""
    return bool(host.get("monitors_release")) and not controls(host)


def sync_all(hosts, log, request=None):
    """sync() for each host that controls its monitors, and release() for each that owes one,
    all at once (each may wait for its PC). The names of the hosts whose release went through."""
    import concurrent.futures
    hosts = [h for h in {id(h): h for h in hosts}.values() if controls(h) or owes_release(h)]
    if not hosts:
        return set()

    def one(h):
        if owes_release(h):
            return h.get("name") if release(h, log, request) else None
        sync(h, log, request)
    with concurrent.futures.ThreadPoolExecutor(len(hosts)) as pool:
        return {name for name in pool.map(one, hosts) if name}
