"""Remote displays wait while the headset is off (docs/remote-displays.md, "While the headset is
off").

ft-screens watches SteamVR (screens/remoteaway.h) and runs `ft-layout remote away on` once the
headset has been off for ten seconds (or SteamVR says standby), and `remote away off` when it's
back on. Away, the stream of every connected remote display stops: the PC's sound no longer
plays in a headset nobody wears, nothing is encoded for nobody, and a PC whose monitors follow
its panels (ft_hostmonitors) puts them back at once, for whoever sat down at it. Back on, the
plans go out again and the streams start, each panel where it was.

The panels stay connected in the layout throughout: `off` is the user's. ft-screens itself
starts no display's stream while the headset is off, so one connected or applied meanwhile
waits too, and starts here.

L is ft_layout (its layout, its socket to ft-screens, its stream helpers), passed in.
"""
import fcntl
import os

import ft_hostmonitors

# One at a time, in the order ft-screens said them. Each is an ft-layout of its own, and "on" can
# still be on its way to a PC (a slow network) when the headset is back on: without the wait, its
# "monitors back" could land after "off" sent the plans, and the PC would end with no plan.
LOCK = os.path.join(os.environ.get("XDG_RUNTIME_DIR") or "/tmp", "frametop-remote-away.lock")


def connected(L, layout):
    """[(number, host, display)] of the remote displays that are connected (not `off`)."""
    return [(n, h, d) for n, h, d in L.remote_displays(layout) if not d.get("off")]


def away(L):
    """The headset came off: the connected displays' streams stop (their panels come back where
    they were), and the PCs' monitors go back as they were."""
    on = connected(L, L.load_layout())
    try:
        sock = L.screens_socket()
        running = L.remotes_running(sock) or {}
        for n, _, d in on:
            if n in running:
                sock.ask(f"remote {n} stop")
                L.log(f"{d['id']}: stopped while the headset is off")
    except RuntimeError as e:
        L.log(f"not stopped now: {e}")
    ft_hostmonitors.put_back_all([h for _, h, d in on if ft_hostmonitors.monitor_of(d)], L.log)


def back(L):
    """The headset is back on: the PCs' monitors follow their panels again, and every connected
    display's stream starts. One whose panel ft-screens kept comes back where it was, one that
    runs already is left alone, and a new one is placed like the others."""
    layout = L.load_layout()
    on = connected(L, layout)
    if not on:
        return
    L.sync_monitors([h for _, h, _ in on])
    new = set()
    try:
        sock = L.screens_socket()
        for n, host, d in on:
            try:
                if L.start_remote(sock, n, host, d) == "ok":
                    new.add(d["id"])
            except L.NoAnswer:
                raise
            except RuntimeError as e:
                L.log(f"remote display {d.get('id')}: {e}")
        if new:
            eye, heading = L.remote_anchor(sock, layout)
            L.place_remotes(sock, layout, L.native_count(layout), eye, heading, only=new)
    except L.NoAnswer as e:
        L.log(f"not started now: {e}")
    except RuntimeError as e:
        L.log(f"not placed now: {e}")


def command(L, args):
    """ft-layout remote away on|off."""
    if args not in (["on"], ["off"]):
        raise RuntimeError("remote away on|off")
    with open(LOCK, "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        return away(L) if args == ["on"] else back(L)
