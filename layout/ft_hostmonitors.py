"""A PC's monitors following its display panels (docs/remote-displays.md, "A PC's monitors").

With a host's "monitors" switch on (ft-layout remote host NAME monitors=on, Frametop Settings),
Frametop's build of Vibepollo on that PC gets a plan made from the host's display panels: each
monitor with a connected panel stays on, in the mode picked for its panel (`mode`, else its
own), and every other monitor (no panel here, or its panel disconnected) goes off. The PC
applies it only while one of those panels streams, and puts every monitor back two minutes
after the last one ends (/api/frametop/monitors).

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
    """The mode picked for a display panel, {width, height, refresh_mhz}, or None (its own)."""
    mode = d.get("mode")
    if not isinstance(mode, dict):
        return None
    try:
        w, h, r = int(mode["width"]), int(mode["height"]), int(mode["refresh_mhz"])
    except (KeyError, TypeError, ValueError):
        return None
    return {"width": w, "height": h, "refresh_mhz": r} if w > 0 and h > 0 and r > 0 else None


def wishes(host):
    """{monitor id: {on, mode?}} from the host's display panels. A monitor with two panels is on
    when either is connected."""
    out = {}
    for d in host.get("displays", []):
        monitor = monitor_of(d)
        if monitor is None:
            continue
        on = not d.get("off")
        if out.get(monitor, {}).get("on") and not on:
            continue
        wish = {"on": on}
        if on and mode_of(d):
            wish["mode"] = mode_of(d)
        out[monitor] = wish
    return out


def plan_body(host, listed):
    """The POST body for the host, or None when there is nothing to send: no display panel is
    connected (the PC's grace then runs as it would). listed: the PC's monitors as it answered
    ({id: monitor}); a panel of a monitor it doesn't have (unplugged) or a mode it doesn't list
    is left out, so the rest of the plan still takes."""
    want = wishes(host)
    if not any(w["on"] for w in want.values()):
        return None
    monitors = {}
    for monitor, wish in want.items():
        m = listed.get(monitor)
        if m is None:
            continue
        mode = wish.get("mode")
        modes = m.get("modes") or []
        if mode and modes and not any(_same_mode(mode, x) for x in modes):
            wish = {"on": wish["on"]}
        monitors[monitor] = wish
    if not any(w["on"] for w in monitors.values()):
        return None  # only unplugged monitors have connected panels: leave the PC as it is
    return {"monitors": monitors, "others_off": True}


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
        status, answer = ask(host, request=request)
        if status != 200 or not isinstance(answer, dict):
            return log(f"{name}: monitors not changed (it answered {status})")
        listed = {m.get("id"): m for m in answer.get("monitors", []) if isinstance(m, dict)}
        body = plan_body(host, listed)
        if body is None:
            return
        status, answer = request(host["address"], "POST", PATH, body=body, timeout=20.0, port=port_of(host))
        if status != 200:
            error = answer.get("error") if isinstance(answer, dict) else None
            log(f"{name}: monitors not changed: {error or f'it answered {status}'}")
    except (OSError, ValueError) as e:
        log(f"{name}: monitors not changed: {e}")


def release(host, log, request=None):
    """The switch turned off: no plan, every monitor back as it was. False when the PC didn't
    take it: ft_layout then keeps `monitors_release` on the host, and sync_all tries again."""
    if host.get("kind", "vibepollo") != "vibepollo":
        return True
    request = request or ft_hostapi.request
    try:
        status, _ = request(host["address"], "POST", PATH, body={"monitors": {}}, timeout=20.0, port=port_of(host))
        if status == 200:
            return True
        log(f"{host.get('name')}: its monitors weren't put back (it answered {status}); asked again "
            "before its next display stream")
    except (OSError, ValueError) as e:
        log(f"{host.get('name')}: its monitors weren't put back: {e}; asked again before its next display stream")
    return False


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
