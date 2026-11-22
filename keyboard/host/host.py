"""ft-kbhost's policy: what Full Keyboard (hosted) shows, where, and where its typing goes.

Typing goes, newest reason first:
- into a Steam text field that opened it (steamhook): the text sink, then steamhook.js;
- to Frametop's desktop while the relay says typing goes there: the sink, then the relay as
  "kbtext" and "kbkey" (keys.py);
- otherwise to gamescope's keyboard focus (Steam, games, gamescope apps): the keyboard's own
  output (libei), with the sink off.

It opens when the relay says ("kb show", from ft-screens' "vrkeyboard" commands, placed by
placement.spot) or a Steam field asks (relayed as "kbsteam", so the relay's Keyboard setting and
connected keyboards decide, and ft-screens says where). It closes for "kb hide", when the Steam
field that opened it closes, when the screens it opened over hide, and while Steam's PIN
lock holds (it then opens for nothing).

Collaborators come in (DIP): keyboard (hosted.Hosted's send), relay (relaylink.RelayLink),
steam (steamhook.SteamHook), allowed (ft_pinlock.allowed), clock.
"""
import collections
import time

import keys
import placement
from touch import Touch

STEAM_FALLBACK = 0.6   # s: a Steam field's opening the relay didn't answer opens where it is
PIN_EVERY = 0.25
ECHO_WAIT = 1.0        # s: the target event a request causes comes well within this (one local round trip)


class Host:
    def __init__(self, keyboard, relay, steam, allowed, clock=time.monotonic):
        self.keyboard, self.relay, self.steam, self.allowed = keyboard, relay, steam, allowed
        self.clock = clock
        self.touch = Touch(self.send)
        self.visible = False
        self.geometry = None          # (matrix, width, height)
        self.pending_spot = None      # (kind, pose) to place once geometry is known
        self.desktop = self.surface = self.take = False
        self.steam_field = None       # {"id", "purpose"} while a Steam field holds it
        self.steam_asked_at = None
        self.opened_for = None        # "steam" | "frametop"
        self.route = None             # "steam" | "desktop" | "normal"
        self.context = None
        self.sink = False             # None: not known (a Stop may have raced the end of a context)
        self.stopped = None           # (route, context) the panel's "Stop shell typing" ended
        # The keyboard sends a target event for every sink change and context it's given, and for
        # the panel's "Stop shell typing": (owner, sent_at) for those our requests will cause.
        self.echoes = collections.deque()
        self.pin_ok, self.pin_checked = True, -1.0

    def send(self, op, **fields):
        if op == "sink":
            self.echoes.append(("sink" if fields.get("on") else "normal", self.clock()))
        elif op == "context" or (op == "show" and "context" in fields) or (op == "hide" and fields.get("end_context")):
            self.echoes.append(("sink" if self.sink else "normal", self.clock()))
        return self.keyboard.send(op, **fields)

    def is_echo(self, owner):
        """Whether a target event is one our own request caused (else it's the user's)."""
        stale = self.clock() - ECHO_WAIT
        while self.echoes and self.echoes[0][1] < stale:
            self.echoes.popleft()   # a request that was refused, or an echo that didn't come
        if self.echoes and self.echoes[0][0] == owner:
            self.echoes.popleft()
            return True
        self.echoes.clear()         # out of step: what it says now is what holds
        return False

    # ---- where typing goes
    def want_route(self, now):
        if self.steam_field is not None:
            return "steam"
        if self.desktop and self.relay.fresh(now):
            return "desktop"
        return "normal"

    def update_route(self, now):
        route = self.want_route(now)
        context = (f"steam-{self.steam_field['id']}" if route == "steam" else
                   "desktop" if route == "desktop" else None)
        if route == self.route and context == self.context:
            return
        if (route, context) == self.stopped:
            return  # the user stopped shell typing for this one: gamescope's, until it changes
        self.stopped = None
        sink = route != "normal"
        if sink != self.sink:
            self.send("sink", on=sink)
            self.sink = sink   # before the context below, whose echo says it
        hints = {"purpose": self.steam_field["purpose"]} if route == "steam" else {"purpose": "text"}
        self.send("context", context=context, hints=hints if context else {})
        self.route, self.context = route, context

    # ---- presentation
    def show(self, now, kind=None, pose=None):
        if not self.pin_ok:
            return
        self.opened_for = "steam" if self.steam_field is not None else "frametop"
        self.steam_asked_at = None
        self.update_route(now)
        if kind and pose:
            if self.geometry:
                self.place(kind, pose)
            else:
                self.pending_spot = (kind, pose)
        elif not self.visible:
            self.send("recenter")
        # No context here: update_route set it with its hints, and a show's context comes without them.
        self.send("show", view="keyboard")

    def place(self, kind, pose):
        width, height = self.geometry[1:3]
        self.send("set_pose", matrix=placement.spot(kind, pose, width, height))

    def hide(self):
        self.end_steam_field()
        self.touch.native()
        self.send("hide", end_context=True)
        self.opened_for = None
        self.steam_asked_at = None
        self.route = self.context = None  # the context ended; the next show sets one
        self.sink = None                  # and the sink: a Stop it raced no longer names a context

    def end_steam_field(self):
        if self.steam_field is not None:
            self.steam.close_session(self.steam_field["id"])
            self.steam_field = None

    # ---- from the relay
    def on_relay(self, now, words):
        if len(words) == 3 and all(w in ("0", "1") for w in words):
            surface = words[1] == "1"
            self.desktop, self.take = words[0] == "1", words[2] == "1"
            if self.surface and not surface and self.visible and self.opened_for == "frametop":
                self.hide()  # the screens it opened over hid, or Frametop paused
            self.surface = surface
            self.steam.take = self.take and self.pin_ok
            if self.visible:
                self.update_route(now)
        elif words[:1] == ["show"]:
            pose = placement.matrix(words[2:]) if len(words) == 14 and words[1] in ("head", "dash") else None
            self.show(now, words[1] if pose else None, pose)
        elif words == ["nosteam"]:
            # The relay won't open it for Steam (its Keyboard setting, a keyboard connected):
            # the field keeps its focus for that keyboard.
            self.end_steam_field()
            self.steam_asked_at = None
            if self.visible:
                self.update_route(now)   # what it shows types where it would without the field
        elif words == ["hide"]:
            if self.visible or self.opened_for:
                self.hide()
        elif words[:1] == ["poke"] and self.visible:
            self.touch.poke(now, words[1:], self.geometry)

    # ---- from Steam (steamhook)
    def on_steam(self, now, results):
        closed = False
        for item in results:
            if item[0] == "done":
                item[1](item[2])
            elif item[0] == "steam" and not item[1]:
                closed = closed or self.steam_field is not None
                self.steam_field = None
            elif item[0] == "event":
                event = item[1]
                if "open" in event:
                    self.steam_field = {"id": event["open"], "purpose": _purpose(event.get("purpose"))}
                    closed = False
                    if self.visible:
                        self.opened_for = "steam"
                        self.update_route(now)
                    else:
                        self.steam_asked_at = now
                        self.relay.steam()  # the relay decides; ft-screens says where
                elif "close" in event and self.steam_field and event["close"] == self.steam_field["id"]:
                    self.steam_field = None
                    closed = True
        if closed and self.steam_field is None:
            if self.opened_for == "steam" and (self.visible or self.steam_asked_at is not None):
                self.hide()
            else:
                self.update_route(now)

    # ---- from the keyboard
    def on_keyboard(self, now, message):
        event = message.get("event")
        if event == "ready":
            # A keyboard process (again, after an update or rollback): hidden, with no sink,
            # context, placement or forwarded pointer until they're sent again.
            self.end_steam_field()   # a new tap on the field opens it again
            self.visible = False
            self.sink = False
            self.route = self.context = self.stopped = None
            self.opened_for = self.steam_asked_at = None
            self.geometry = self.pending_spot = None
            self.echoes.clear()
            self.touch.reset()
        elif event == "visibility":
            visible = message.get("visible") is True
            if visible != self.visible:
                self.visible = visible
                self.relay.hello(visible)
                if not visible:  # the user's X, a lease or tracking loss: nothing holds it now
                    self.end_steam_field()
                    self.touch.native()
                    self.opened_for = None
                    self.route = self.context = None
                    self.sink = None
        elif event == "geometry":
            matrix, width, height = message.get("matrix"), message.get("width"), message.get("height")
            if (isinstance(matrix, list) and len(matrix) == 3 and isinstance(width, (int, float))
                    and isinstance(height, (int, float)) and width > 0 and height > 0):
                case = message.get("keys_uv")   # the case, whose bottom edge tops the move handle
                bottom = None
                if isinstance(case, dict) and all(isinstance(case.get(k), (int, float)) for k in ("v", "height")):
                    bottom = float(case["v"]) + float(case["height"])
                self.geometry = (matrix, float(width), float(height), bottom)
                if self.pending_spot:
                    self.place(*self.pending_spot)
                    self.pending_spot = None
        elif event == "target":
            # "Stop shell typing" on the panel: typing goes to gamescope until the route changes.
            # The keyboard also sends one for each sink and context request: those aren't it, and
            # nor is a late one for a context that's gone (a Stop names the context it stopped).
            owner = message.get("owner")
            if (not self.is_echo(owner) and owner == "normal" and self.sink
                    and self.context is not None and message.get("context") == self.context):
                self.stopped = (self.route, self.context)
                self.sink = False
                self.route = "normal"
        elif event in ("commit", "key"):
            self.on_typed(now, message)

    def on_typed(self, now, message):
        context, seq = message.get("context"), message.get("seq")
        ack = lambda ok: self.send("ack" if ok else "reject", context=context, seq=seq)  # noqa: E731
        if context is None or context != self.context or (self.route == "steam" and self.steam_field is None):
            ack(True)  # for a field it no longer types into: dropped
            return
        if message["event"] == "commit":
            text = message.get("text")
            if not isinstance(text, str):
                ack(True)
            elif self.route == "steam":
                self.steam.type(self.steam_field["id"], text, ack)
            else:
                ack(self.relay.text(now, text))
            return
        name, modifiers = message.get("key"), message.get("modifiers") or []
        if not isinstance(name, str) or not isinstance(modifiers, list):
            ack(True)
        elif self.route == "steam":
            self.steam.key(self.steam_field["id"], name, [m for m in modifiers if isinstance(m, str)], ack)
        else:
            ok = True
            for code, down in keys.presses(name, modifiers):
                ok = self.relay.key(now, code, down) and ok
            ack(ok)

    # ---- timers
    def tick(self, now):
        if now - self.pin_checked >= PIN_EVERY:
            self.pin_checked = now
            ok = self.allowed()
            if self.pin_ok and not ok and (self.visible or self.opened_for):
                self.hide()
            self.pin_ok = ok
            self.steam.take = self.take and ok
        if (self.steam_asked_at is not None and self.steam_field is not None and not self.visible
                and now - self.steam_asked_at >= STEAM_FALLBACK):
            self.show(now)  # ft-screens isn't there to say where: where it was, or recentred
        elif self.steam_asked_at is not None and self.steam_field is None:
            self.steam_asked_at = None
        if self.visible:
            self.update_route(now)
        self.touch.tick(now)
        self.relay.flush()


def _purpose(value):
    return value if value in ("text", "password", "number", "email", "url", "search") else "text"
