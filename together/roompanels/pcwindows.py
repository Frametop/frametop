"""The windows of its person's PC that stand on this Frame as panels (docs/remote-displays.md,
"PC app windows"): which of ft-screens' remote screens each is, and where its PC is asked.

They are read from the saved layout (frametop-layout.json), which ft-layout writes: a window
panel is a display of a host with "window": true, its "app" is "window:" and the window's
address on the PC, its "screen" the remote screen it stands on, and its "port" the PC's
Frametop windows instance.
"""
import collections
import json
import os

REMOTE_FIRST, REMOTE_LAST = 101, 116    # ft-screens' remote screens (screens/remote.h)

# address, port: the PC and its windows instance. window: the window's address there.
# client: the stream's client, which ft-screens' "remotes" names. label: its panel's title.
Window = collections.namedtuple("Window", "address port window client label")


def _text(value, most):
    return value if isinstance(value, str) and 0 < len(value) <= most else None


def windows(layout):
    """{remote screen: Window} of a layout as it is in the file. One that isn't connected
    ("off") isn't there, nor anything that doesn't read as a window panel."""
    found = {}
    hosts = layout.get("hosts") if isinstance(layout, dict) else None
    for host in hosts if isinstance(hosts, list) else []:
        displays = host.get("displays") if isinstance(host, dict) else None
        address = _text(host.get("address"), 64) if isinstance(host, dict) else None
        for d in displays if isinstance(displays, list) and address else []:
            if not isinstance(d, dict) or d.get("window") is not True or d.get("off"):
                continue
            screen, port = d.get("screen"), d.get("port")
            app, client = _text(d.get("app"), 80), _text(d.get("client"), 80)
            if type(screen) is not int or not REMOTE_FIRST <= screen <= REMOTE_LAST or type(port) is not int or not 0 < port < 65535:
                continue
            if not app or not app.startswith("window:") or len(app) == 7 or not client:
                continue
            label = d.get("label")
            found[screen] = Window(address, port, app[7:], client, label if isinstance(label, str) else "")
    return found


def reader(path):
    """windows() of the layout in `path`, read again when the file changed: {} while there is
    no file, None while it doesn't read."""
    seen = [None, {}]

    def read():
        try:
            stamp = os.stat(path).st_mtime_ns
        except OSError:
            return {}
        if stamp != seen[0]:
            try:
                with open(path, encoding="utf-8") as f:
                    seen[:] = [stamp, windows(json.load(f))]
            except (OSError, ValueError):
                return None
        return seen[1]

    return read
