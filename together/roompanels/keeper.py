"""What ft-roompanels keeps true while this Frame is in a room (docs/together.md, "What a guest
sees"): every panel the others share stands as a panel of ft-screens' here, where the room says;
and every screen this Frame's person shares is in the room where it stands here, with ft-screens
sending its pictures to an ft-sharepanel.

The rules only. Its questions go out through two functions it is given, each a text in and the
answer or None back: `together` (ft-together's local socket) and `screens` (ft-screens'). So the
tests ask it everything without either program.

What the room says of someone else's panel comes from them: its name is cut down to plain
characters before it goes into a command, and its numbers were bounded by ft-together
(proto/panel.h).
"""
import json
import math

from . import placing

SLOTS = 3                               # the panels a Frame shares at most (screens/share.h)
REMOTE_FIRST, REMOTE_LAST = 101, 116    # ft-screens' remote screens (screens/remote.h)
FPS = 30
MOST_WIDE, MOST_HIGH = 1920, 1200       # a shared screen's video, at most
# The encoder's sizes. Across, only multiples of 128: any other width resets the Frame as the
# encoder starts (media/irisenc.h, kWidthStep, which refuses them). Down, multiples of 8, as
# every height it has been seen to take is.
ACROSS, DOWN = 128, 8
FLOOR_WAIT = 5.0    # seconds in a room with no head pose before the floor is laid without one
MISSES = 8          # questions to ft-together in a row with no answer: it is taken for gone
PLAIN = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .,:;'()+-_#&/")


def stream_size(width, height):
    """The video's pixels for a screen of width x height: it whole, no larger than may be, in a
    size the encoder takes, and as near the screen's shape as that allows. Across, the nearest
    multiple of ACROSS that fits; down, what keeps the shape, to a multiple of DOWN. The screen
    is drawn over the whole of it, and shown at the panel's shape, so a pixel or two of stretch
    is all a screen of another size pays."""
    k = min(1.0, MOST_WIDE / width, MOST_HIGH / height)
    across = max(ACROSS, min(MOST_WIDE // ACROSS * ACROSS, int(width * k / ACROSS + 0.5) * ACROSS))
    while across > ACROSS and across * height / width > MOST_HIGH:
        across -= ACROSS
    down = int(across * height / width / DOWN + 0.5) * DOWN
    return across, min(MOST_HIGH // DOWN * DOWN, max(128, down))


def plain(name, otherwise):
    """A peer's name for a panel, as a label of ours: plain characters only."""
    kept = " ".join("".join(c if c in PLAIN else " " for c in str(name)).split())[:48]
    return kept.strip() or otherwise


def share_words(slot, size, placed, name):
    """The "share" that tells ft-together a panel as `placed` (placing.to_room)."""
    metres = min(20.0, max(0.15, placed["metres"]))
    curve = 0.0 if placed["curve"] <= 0 else min(100.0, max(0.5, placed["curve"]))
    at = " ".join(f"{v:.3f}" for v in placed["at"])
    turn = " ".join(f"{v:.4f}" for v in placed["turn"])
    return f"share {slot} {size[0]} {size[1]} {at} {turn} {metres:.3f} {curve:.3f} {name}"


def _ok(reply):
    return reply is not None and reply.startswith("ok")


def _numbers(reply, count):
    """`count` numbers after "ok", or None."""
    words = (reply or "").split()
    if len(words) != count + 1 or words[0] != "ok":
        return None
    try:
        got = [float(w) for w in words[1:]]
    except ValueError:
        return None
    return got if all(math.isfinite(v) for v in got) else None


def _listed(reply, parts):
    """The entries of an "ok <count> a:b:c ..." answer, each split in `parts`; or None."""
    words = (reply or "").split()
    if len(words) < 2 or words[0] != "ok":
        return None
    entries = [w.split(":") for w in words[2:]]
    return entries if all(len(e) == parts for e in entries) else None


class Keeper:
    def __init__(self, together, screens, now):
        self.together, self.screens, self.now = together, screens, now
        self.wishes = {}     # screen number -> slot: what this Frame's person shares
        self.told = {}       # slot -> the "share" the room has from us
        self.shown = {}      # a peer's panel id -> [its remote screen, the room's panel as placed]
        self.in_room = False
        self.floor = None
        self.tidy = False    # nothing of an earlier room is left in ft-screens
        self._floorless = None   # since when: in a room, no floor, no head pose
        self._misses = 0

    # ---- what its person asks (ft-roompanels' own socket) ----

    def share(self, number):
        if not self.in_room:
            return "error not in a room"
        if number in self.wishes:
            return f"ok {self.wishes[number]}"
        sizes = self._sizes()
        if sizes is None:
            return "error no answer from ft-screens"
        if number not in sizes:
            return f"error screen {number} isn't one of this desktop's screens"
        free = [k for k in range(SLOTS) if k not in self.wishes.values()]
        if not free:
            return f"error {SLOTS} screens are shared already"
        self.wishes[number] = free[0]
        return f"ok {free[0]}"

    def unshare(self, number):
        if number not in self.wishes:
            return f"error screen {number} isn't shared"
        slot = self.wishes.pop(number)
        self.told.pop(slot, None)
        self.together(f"unshare {slot}")
        self.screens(f"share {number} stop")
        return "ok"

    def state(self):
        return json.dumps({
            "room": self.in_room, "floor": self.floor,
            "shares": [{"screen": n, "panel": k, "told": k in self.told} for n, k in sorted(self.wishes.items())],
            "panels": [{"id": i, "screen": s[0], "placed": s[1] is not None} for i, s in sorted(self.shown.items())]})

    # ---- every tick ----

    def tick(self):
        room = self._room()
        if room is None:
            return
        if not room:
            self._leave()
            return
        self.in_room, self.tidy = True, False
        self.floor = self._floor(room)
        self._others(room)
        self._mine(room)

    def _room(self):
        """The room's state; {} when this Frame is in none; None when ft-together said nothing
        this time (it may yet)."""
        reply = self.together("state")
        state = None
        if reply is not None and reply.startswith("ok {"):
            try:
                state = json.loads(reply[3:])
            except ValueError:
                pass
        if not isinstance(state, dict):
            self._misses += 1
            return {} if self._misses >= MISSES else None
        self._misses = 0
        return state if state.get("mode") in ("hosting", "joined") else {}

    def end(self):
        """ft-roompanels is ending: nothing of the room stays behind in ft-screens."""
        self.tidy = False
        self._leave()

    def _leave(self):
        """Out of the room: its panels go, and nothing is shared. Also what a start finds left
        of an earlier ft-roompanels' room."""
        self.in_room, self.floor, self._floorless = False, None, None
        self.wishes.clear(), self.told.clear(), self.shown.clear()
        if self.tidy:
            return
        remotes, shares = self._remotes(), self._shares()
        if remotes is None or shares is None:
            return   # ft-screens isn't there: asked again next tick
        for number, client in remotes.items():
            if client.startswith("peer-"):
                self.screens(f"remote {number} stop")
        for number in shares:
            self.screens(f"share {number} stop")
        self.tidy = True

    def _floor(self, room):
        """Where this Frame's floor lies in the room: as ft-together has it, or laid now from
        where its person stands (placing.lay)."""
        said = self.together("floor")
        got = _numbers(said, 3)
        if got is not None:
            return tuple(got)
        if said is None or said.split() != ["ok", "none"]:
            return None
        me = next((p for p in room.get("people", []) if p.get("id") == room.get("me")), None)
        if me is None:
            return None
        head = _numbers(self.screens("head"), 4)
        if head is None:
            # Nobody wears it. A floor laid without a head has its middle on the spot.
            if self._floorless is None:
                self._floorless = self.now()
            if self.now() - self._floorless < FLOOR_WAIT:
                return None
            head = [0.0, 0.0, 0.0, 0.0]
        laid = placing.lay(me["spot"], (head[0], head[2], math.radians(head[3])))
        self.together(f"floor {laid[0]:.4f} {laid[1]:.4f} {laid[2]:.5f}")
        return None   # read back next tick: someone else may have laid one first

    # ---- the others' panels ----

    def _others(self, room):
        remotes = self._remotes()
        if remotes is None:
            return
        wanted = {p["id"]: p for p in room.get("panels", []) if p["owner"] != room.get("me")}
        for number, client in sorted(remotes.items()):
            if not client.startswith("peer-") or not client[5:].isdigit():
                continue
            ident = int(client[5:])
            known, panel = self.shown.get(ident), wanted.get(ident)
            ours = known is not None and known[0] == number
            if ours and panel is not None and (known[1] is None or known[1]["size"] == panel["size"]):
                continue
            # Left the room, another video size, or a screen we don't know (an earlier
            # ft-roompanels', or one on its way out): it goes, and a wanted one is made anew.
            if _ok(self.screens(f"remote {number} stop")):
                del remotes[number]   # its number is free again
            if ours:
                del self.shown[ident]
        for ident in [i for i, s in self.shown.items() if s[0] not in remotes or i not in wanted]:
            del self.shown[ident]
        if self.floor is None:
            return
        for ident, panel in sorted(wanted.items()):
            if ident not in self.shown and not self._make(ident, panel, remotes):
                continue
            self._place(ident, panel)

    def _make(self, ident, panel, remotes):
        """A remote screen for a peer's panel: from the last number down, clear of ft-layout's
        remote displays, which count up from the first."""
        number = next((n for n in range(REMOTE_LAST, REMOTE_FIRST - 1, -1) if n not in remotes), None)
        if number is None:
            return False
        width, height = panel["size"]
        label = plain(panel.get("name"), f"Panel {ident}")
        if not _ok(self.screens(f"remote {number} peer {ident} {width}x{height} {panel['metres']:.3f} {label}")):
            return False
        remotes[number] = f"peer-{ident}"
        self.shown[ident] = [number, None]
        return True

    def _place(self, ident, panel):
        """Puts the panel where the room has it, when the room moved it (or it is new here).
        Between those it stays where its viewer left it."""
        number, placed = self.shown[ident]
        want = {k: panel[k] for k in ("size", "at", "turn", "metres", "curve")}
        if placed == want:
            return
        pose = " ".join(f"{v:.5f}" for v in placing.to_floor(self.floor, panel))
        asked = [f"pose {number} {pose}", f"width {number} {panel['metres']:.3f}", f"curve {number} {panel['curve']:.3f}"]
        if all([_ok(self.screens(a)) for a in asked]):
            self.shown[ident][1] = want

    # ---- this Frame's own ----

    def _mine(self, room):
        shares, sizes = self._shares(), self._sizes()
        if shares is None or sizes is None:
            return
        for number, (slot, _) in shares.items():
            if self.wishes.get(number) != slot:
                self.screens(f"share {number} stop")
        for panel in room.get("panels", []):
            if panel["owner"] == room.get("me") and panel["number"] not in self.wishes.values():
                self.together(f"unshare {panel['number']}")   # an earlier ft-roompanels' share
        for number, slot in sorted(self.wishes.items()):
            if number not in sizes:
                self.unshare(number)   # the screen went (unplugged, a layout with fewer)
                continue
            size = stream_size(*sizes[number])
            got = placing.parse_get(self.screens(f"get {number}") or "") if self.floor is not None else None
            if got is not None:
                words = share_words(slot, size, placing.to_room(self.floor, got), f"Screen {number}")
                if words != self.told.get(slot) and _ok(self.together(words)):
                    self.told[slot] = words
            if slot in self.told and shares.get(number) != (slot, f"{size[0]}x{size[1]}"):
                self.screens(f"share {number} start {slot} {size[0]}x{size[1]} {FPS}")

    # ---- ft-screens' lists ----

    def _remotes(self):
        """{remote screen number: its client}, or None."""
        entries = _listed(self.screens("remotes"), 4)
        if entries is None or not all(e[0].isdigit() for e in entries):
            return None
        return {int(e[0]): e[1] for e in entries}

    def _shares(self):
        """{screen number: (slot, "WxH")} of ft-screens' share tap, or None."""
        entries = _listed(self.screens("shares"), 4)
        if entries is None or not all(e[0].isdigit() and e[1].isdigit() for e in entries):
            return None
        return {int(e[0]): (int(e[1]), e[3]) for e in entries}

    def _sizes(self):
        """{screen number: (width, height)} of the desktop's own screens, or None."""
        entries = _listed(self.screens("screens"), 3)
        try:
            sizes = None if entries is None else {int(e[0]): tuple(int(v) for v in e[1].split("x")) for e in entries}
        except ValueError:
            return None
        if sizes is None or not all(len(s) == 2 and min(s) > 0 for s in sizes.values()):
            return None
        return sizes
