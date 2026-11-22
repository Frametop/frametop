"""Fingertips on Full Keyboard's panel: ft-screens' pokes (through the relay, "kb poke SIDE VERB
[X Y Z]") as the hosted keyboard's forwarded pointer.

ft-screens' hand input decides hovers and presses, as on Frametop's screens; this only puts the
point on the panel. Forwarding turns the keyboard's own lasers off, so it's on only while a
fingertip is near: from the first poke until both hands' last one is IDLE seconds old, or the last
press lets go.
"""
import handle
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
        self.drags = {}           # side -> handle.Drag: a press on the move handle

    def poke(self, now, words, geometry):
        """words: SIDE VERB [X Y Z]; geometry: (matrix, width, height[, case bottom in v]) as last
        reported, or None."""
        if len(words) not in (2, 5) or words[0] not in SIDES or geometry is None:
            return
        side, verb = words[0], words[1]
        drag = self.drags.get(side)
        if drag and (verb == "end" or now - drag.last >= STUCK):   # let go, or its up was lost
            del self.drags[side]
            drag = None
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
        if drag:
            self.send("set_pose", matrix=drag.to(now, point))
            if verb == "up":
                del self.drags[side]
            return
        u, v = placement.touch(point, *geometry[:3])
        if side not in self.pressed and handle.in_zone(u, v, geometry[3] if len(geometry) > 3 else None):
            # The move handle: the keyboard's own line, so its pointer isn't forwarded there (it
            # hides the line while one is). A key's press that slides onto it stays the key's.
            if self.forwarded:
                self.send("pointer", hand=side, phase="cancel")
                self.last.pop(side, None)
                if not self.pressed and not self.last:
                    self.native()
            if verb == "press":
                self.drags[side] = handle.Drag(now, point, geometry[0])
            return
        if not self.forwarded:
            self.send("pointer_mode", mode="forwarded")
            self.forwarded = True
            self.pressed.clear()
        self.last[side] = now
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
            # Full Keyboard acts on its X (a hide, in hosted mode) only on its own pointers' frames:
            # a forwarded X did nothing until forwarding went idle, which a ray resting on the
            # panel never lets it (user, 2026-11-21: "It doesn't disapear when I click hide"). So
            # each release hands the panel back; the next poke forwards again.
            if not self.pressed:
                self.native()

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
        self.drags.clear()
        self.forwarded = False
        self.pressed.clear()
        self.last.clear()
