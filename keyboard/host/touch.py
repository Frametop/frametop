"""Fingertips on Full Keyboard's panel: ft-screens' pokes (through the relay, "kb poke SIDE VERB
[X Y Z]") as the hosted keyboard's forwarded pointer.

ft-screens' hand input decides hovers and presses, as on Frametop's screens; this only puts the
point on the panel. Forwarding turns the keyboard's own lasers off, so it's on only while a
fingertip is near: from the first poke until both hands' last one is IDLE seconds old.
"""
import placement

IDLE = 0.6
STUCK = 3.0   # a hand's press heard nothing more of for this long is cancelled (it never types)
SIDES = ("left", "right")


class Touch:
    def __init__(self, send):
        self.send = send          # Hosted.send
        self.forwarded = False
        self.last = {}            # side -> when it was last heard of
        self.pressed = set()

    def poke(self, now, words, geometry):
        """words: SIDE VERB [X Y Z]; geometry: (matrix, width, height) as last reported, or None."""
        if len(words) not in (2, 5) or words[0] not in SIDES or geometry is None:
            return
        side, verb = words[0], words[1]
        if verb == "end":
            if self.forwarded:
                self.send("pointer", hand=side, phase="cancel")
            self.pressed.discard(side)
            self.last[side] = now
            return
        if len(words) != 5:
            return
        try:
            point = [float(w) for w in words[2:]]
        except ValueError:
            return
        if not self.forwarded:
            self.send("pointer_mode", mode="forwarded")
            self.forwarded = True
            self.pressed.clear()
        self.last[side] = now
        u, v = placement.touch(point, *geometry)
        where = {"u": round(u, 5), "v": round(v, 5)}
        if verb in ("hover", "move"):
            self.send("pointer", hand=side, phase="move", **where)
        elif verb == "press":
            self.send("pointer", hand=side, phase="move", **where)
            self.send("pointer", hand=side, phase="down", **where)
            self.pressed.add(side)
        elif verb == "up":
            self.send("pointer", hand=side, phase="up", **where)
            self.pressed.discard(side)

    def tick(self, now):
        if not self.forwarded:
            return
        for side in sorted(self.pressed):
            if now - self.last.get(side, 0.0) >= STUCK:   # its up or end was lost
                self.send("pointer", hand=side, phase="cancel")
                self.pressed.discard(side)
        if not self.pressed and now - max(self.last.values(), default=0.0) >= IDLE:
            self.native()

    def native(self):
        if self.forwarded:
            self.send("pointer_mode", mode="native")
        self.forwarded = False
        self.pressed.clear()
        self.last.clear()

    def reset(self):
        """The keyboard restarted: it begins with its own pointers."""
        self.forwarded = False
        self.pressed.clear()
        self.last.clear()
