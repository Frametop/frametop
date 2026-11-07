"""pc_game: whether a PC game streamed over Steam Link should make Frametop give way (DEE-346).

On the Frame, Steam Link's client is SteamVR's one scene app while a PC streams, with or without a
game on the PC, so the Frame alone can't tell a game from the PC's SteamVR Home. The pointer helper
says "vrlink 1|0" (on a change and every 5 s) while that client is the scene app; while it is, this
asks each PC in frametop-layout.json that has an API token what its SteamVR runs
(GET /api/frametop/vrscene on Frametop's Vibepollo: {"steamvr": bool, "scene": "none" | "home" |
"game", "app": "..."}), every POLL seconds, on its own thread.

The answer goes to $XDG_RUNTIME_DIR/frametop-pcgame, "1" or "0" and a newline, rewritten on every
poll; ft-screens and the pointer helper read it (conf/vrscene.h) and ignore one older than 15 s.
It's "1" only while a PC game runs and the "pc_games" setting (in ~/.config/frametop-input.json,
next to the pause's settings for games on the Frame) is "hide", the default. "visible" keeps
Frametop up over PC games. A host without the endpoint (an older Vibepollo, or Sunshine) never
answers "game", so Frametop stays up over Steam Link there.
"""
import json
import os
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "conf"))
import ft_hostapi  # noqa: E402

POLL = 3.0
LINK_STALE = 12.0  # the helper repeats "vrlink" every 5 s; silence this long means no Steam Link
MODES = ("hide", "visible")
LAYOUT_PATH = os.path.expanduser("~/.config/frametop-layout.json")
SCENE_PATH = "/api/frametop/vrscene"


def flag_path():
    return os.path.join(os.environ.get("XDG_RUNTIME_DIR") or "/tmp", "frametop-pcgame")


def layout_hosts(path=LAYOUT_PATH):
    try:
        with open(path) as f:
            hosts = json.load(f).get("hosts", [])
    except (OSError, ValueError, AttributeError):
        return []
    return [h for h in hosts if isinstance(h, dict) and h.get("address")]


def addresses(host):
    """Where to ask a host, as its route allows (ft_layout.host_answers): its Steam Link dongle on
    the Frame's hotspot first, which is where Steam Link reaches it then, and its address."""
    route = host.get("route", "auto")
    out = ([] if route == "network" else [a for a in host.get("direct", []) if isinstance(a, str)]) + \
        ([] if route == "dongle" else [host["address"]])
    return list(dict.fromkeys(a for a in out if a))


def write_flag(path, on):
    tmp = f"{path}.tmp"
    with open(tmp, "w") as f:
        f.write("1\n" if on else "0\n")
    os.replace(tmp, path)


class PcGame:
    def __init__(self, log, ask=None, hosts=layout_hosts, path=None, clock=time.monotonic):
        self.log = log
        self.ask = ask or (lambda address, host: ft_hostapi.get_json(address, SCENE_PATH, timeout=2.0,
                                                                      token_address=host["address"]))
        self.hosts = hosts
        self.path = path or flag_path()
        self.clock = clock
        self.mode = "hide"
        self.link_seen = None  # when the helper last said "vrlink 1"
        self.game = None       # the last answer: (host name, app) or None
        self.written = False  # nothing to say until Steam Link streams (a stale "1" lapses by itself)
        self.wake = threading.Event()
        self.lock = threading.Lock()

    def configure(self, rules):
        mode = rules.get("pc_games")
        with self.lock:
            self.mode = mode if mode in MODES else "hide"
        self.wake.set()

    def link_state(self, on, now):
        with self.lock:
            was = self.linked(now)
            self.link_seen = now if on else None
        if on and not was:
            self.log("Steam Link: streaming a PC's SteamVR")
            self.wake.set()
        elif was and not on:
            self.log("Steam Link: ended")
            self.wake.set()

    def linked(self, now):
        return self.link_seen is not None and now - self.link_seen < LINK_STALE

    def scene_game(self):
        """(host name, app) of the first host whose SteamVR runs a game, or None."""
        for host in self.hosts():
            for address in addresses(host):
                answer = self.ask(address, host)
                if isinstance(answer, dict):
                    if answer.get("scene") == "game":
                        return host.get("name") or address, str(answer.get("app") or "")
                    break  # it answered: no game there
        return None

    def step(self, now):
        """One poll: ask while Steam Link streams, write the flag. Returns what was written."""
        with self.lock:
            linked, mode = self.linked(now), self.mode
        game = self.scene_game() if linked else None
        if game != self.game:
            if game:
                self.log(f"Steam Link: {game[0]} runs a PC game ({game[1] or 'unnamed'})")
            elif self.game:
                self.log("Steam Link: the PC game ended")
            self.game = game
        on = bool(game) and mode == "hide"
        if linked or self.written is not False:
            try:
                write_flag(self.path, on)
                self.written = on
            except OSError as e:
                self.log(f"Steam Link: can't write {self.path}: {e}")
        return on

    def run(self):
        while True:
            self.step(self.clock())
            self.wake.wait(POLL)
            self.wake.clear()

    def start(self):
        threading.Thread(target=self.run, name="pc-game", daemon=True).start()
