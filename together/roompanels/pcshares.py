"""The windows of its person's PC in the room (docs/together.md, "A PC's panels"). A window of
the PC's that stands on this Frame as a panel (pcwindows.py) can be shared like a screen. The
PC then sends its pictures itself (pclink.py keeps it at that), and this tells the room where
the panel stands and how large its video is, as the keeper does for the desktop's screens.

The rules only: `together` and `screens` are the keeper's two questions, `windows()` is
{remote screen: Window} as the saved layout has them now (None when it doesn't read), and
`link` is the PC's side (pclink.PcLink: want, sizes, take_gone, why).

One PC at a time: a Frame takes one PC's call.
"""
from . import placing
from .keeper import plain, share_words

FIRST, LAST = 3, 6    # a PC's panels among its person's (together/proto/sender.h)


def _ok(reply):
    return reply is not None and reply.startswith("ok")


class PcShares:
    def __init__(self, together, screens, windows, link):
        self.together, self.screens, self.windows, self.link = together, screens, windows, link
        self.wishes = {}    # a window's remote screen -> its panel's number
        self.told = {}      # a panel's number -> the "share" the room has from us
        self.mine = {}      # the room's id of a panel -> (its screen here, that screen's (width, height))

    # ---- what its person asks ----

    def share(self, number):
        if number in self.wishes:
            return f"ok {self.wishes[number]}"
        windows = self.windows()
        if windows is None:
            return "error the saved layout doesn't read"
        if number not in windows:
            return f"error screen {number} isn't a window of your PC's"
        pcs = {(windows[n].address, windows[n].port) for n in self.wishes if n in windows}
        if pcs and (windows[number].address, windows[number].port) not in pcs:
            return "error windows of another PC are shared already: one PC at a time"
        free = [k for k in range(FIRST, LAST + 1) if k not in self.wishes.values()]
        if not free:
            return f"error {LAST - FIRST + 1} windows are shared already"
        self.wishes[number] = free[0]
        return f"ok {free[0]}"

    def unshare(self, number):
        if number not in self.wishes:
            return f"error screen {number} isn't shared"
        slot = self.wishes.pop(number)
        self.told.pop(slot, None)
        self.mine = {ident: m for ident, m in self.mine.items() if m[0] != number}
        self.together(f"unshare {slot}")
        return "ok"

    def state(self):
        """Its part of ft-roompanels' state: the shared windows, the windows there are to
        share, and what holds the PC up."""
        windows = self.windows() or {}
        return {"shares": [{"screen": n, "panel": k, "told": k in self.told} for n, k in sorted(self.wishes.items())],
                "windows": [{"screen": n, "name": plain(w.label, f"Window {n}")} for n, w in sorted(windows.items())],
                "pc": self.link.why()}

    def brought(self):
        """{a panel's number: its screen}, for the ones the room has: where the others put
        them is where their screens go (carrying.py)."""
        return {slot: number for number, slot in self.wishes.items() if slot in self.told}

    # ---- every tick of the keeper's, in a room ----

    def tick(self, room, floor):
        me = room.get("me")
        for panel in room.get("panels", []):
            if panel["owner"] == me and FIRST <= panel["number"] <= LAST and panel["number"] not in self.wishes.values():
                self.together(f"unshare {panel['number']}")   # an earlier ft-roompanels' share
        windows, remotes = (self.windows(), self._remotes()) if self.wishes else ({}, {})
        if windows is None or remotes is None:
            return   # asked again next tick
        for number in [n for n in self.wishes if n not in windows or remotes.get(n, ("",))[0] != windows[n].client]:
            self.unshare(number)   # its panel here went
        self.link.want({slot: windows[number] for number, slot in self.wishes.items()})
        gone = self.link.take_gone()
        for number in [n for n, slot in self.wishes.items() if slot in gone]:
            self.unshare(number)   # its window went from the PC
        sizes = self.link.sizes()
        for number, slot in sorted(self.wishes.items()):
            got = placing.parse_get(self.screens(f"get {number}") or "") if slot in sizes and floor is not None else None
            if got is None:
                continue
            words = share_words(slot, sizes[slot], placing.to_room(floor, got), plain(windows[number].label, f"Window {slot - FIRST + 1}"))
            if words != self.told.get(slot) and _ok(self.together(words)):
                self.told[slot] = words
        ids = {p["number"]: p["id"] for p in room.get("panels", []) if p["owner"] == me}
        self.mine = {ids[slot]: (number, remotes[number][1]) for number, slot in self.wishes.items() if slot in self.told and slot in ids}

    def leave(self):
        """Out of the room, or ft-roompanels is ending: nothing of the PC's is shared, and it
        is told to stop."""
        for slot in self.told:
            self.together(f"unshare {slot}")   # (the PC's pictures don't end with this program, as a screen's do)
        self.wishes.clear(), self.told.clear(), self.mine.clear()
        self.link.want({})

    def _remotes(self):
        """{remote screen: (its client, its picture's (width, height))} of ft-screens', or None."""
        words = (self.screens("remotes") or "").split()
        if len(words) < 2 or words[0] != "ok":
            return None
        found = {}
        for entry in words[2:]:
            parts = entry.split(":")
            size = parts[3].split("x") if len(parts) == 4 else []
            if len(size) != 2 or not all(v.isdigit() for v in [parts[0]] + size):
                return None
            found[int(parts[0])] = (parts[1], (int(size[0]), int(size[1])))
        return found
