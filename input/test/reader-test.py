#!/usr/bin/env python3
"""Offline test of the relay's controller reader against a vrserver that stops answering for a
controller, as SteamVR 2.19 did on 2026-11-27 (the left controller, awake and in hand, was never
heard: gamepad mode had no d-pad, no left stick and no way out by the grips). vrws.Hearing by
itself, then game_pause.ControllerWatch's session on a fake web socket and a fake clock: no
SteamVR, no sockets, no waiting.

  input/test/reader-test.py
"""
import json
import os
import sys
import types

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import game_pause as gp  # noqa: E402
import vrws  # noqa: E402

failures = []


def check(label, got, want):
    ok = got == want
    print(("ok    " if ok else "FAIL  ") + label + ("" if ok else f": got {got!r}, want {want!r}"), flush=True)
    if not ok:
        failures.append(label)


# ---- vrws.Hearing
QUIET, WITHIN, MESSAGES = vrws.QUIET, vrws.BACK_WITHIN, vrws.BACK_MESSAGES
L, R = "/user/hand/left", "/user/hand/right"
h = vrws.Hearing(100.0)
check("nothing is quiet at once", h.quiet([L, R], 100.0 + QUIET / 2), [])
check("never heard, QUIET on: quiet", h.quiet([L, R], 100.0 + QUIET), [L, R])
h.message(R, 100.9)
check("one that spoke isn't", h.quiet([L, R], 101.5), [L])
h.ask(L, 101.5)
check("asked for: not again within QUIET", h.quiet([L], 101.5 + QUIET / 2), [])
check("...and again after it", h.quiet([L], 101.5 + QUIET), [L])

h = vrws.Hearing(0.0)
h.looked_up({L, R}, 2.0)
h.looked_up({L, R}, 5.0)
h.ask(L, 6.0)
back = [h.message(L, 6.0 + 0.01 * n) for n in range(MESSAGES + 2)]
check("connected at lookups while silent, asked again, and it streams: back, said once, with how long "
      "it was silent and connected", back, [None] * (MESSAGES - 1) + [4.0] + [None, None])

h = vrws.Hearing(0.0)
h.looked_up({R}, 2.0)
h.looked_up({L, R}, 5.0)
h.ask(L, 5.0)
check("first seen connected at this very lookup (it only just woke): nothing to say",
      [h.message(L, 5.0 + 0.01 * n) for n in range(MESSAGES)], [None] * MESSAGES)

h = vrws.Hearing(0.0)
h.looked_up({R}, 5.0)
h.ask(L, 6.0)
check("not connected at the lookup (asleep, and it wakes just then): nothing to say",
      [h.message(L, 6.0 + 0.01 * n) for n in range(MESSAGES)], [None] * MESSAGES)

h = vrws.Hearing(0.0)
h.looked_up({L}, 5.0)
h.message(L, 5.5)
h.ask(L, 7.0)
check("the lookup was before it went silent: nothing to say",
      [h.message(L, 7.0 + 0.01 * n) for n in range(MESSAGES)], [None] * MESSAGES)

h = vrws.Hearing(0.0)
h.looked_up({L}, 5.0)
h.ask(L, 6.0)
answers = [h.message(L, 6.01)] + [h.message(L, 6.0 + WITHIN + 0.1 * n) for n in range(1, MESSAGES + 1)]
check("a sleeping controller's one answer, and messages later than BACK_WITHIN: not back", answers, [None] * (MESSAGES + 1))

h = vrws.Hearing(0.0)
h.looked_up({L}, 0.2)
h.message(L, 0.3)
h.ask(L, 0.5)
check("asked for while it talks (gamepad mode starting): nothing to say",
      [h.message(L, 0.5 + 0.01 * n) for n in range(MESSAGES)], [None] * MESSAGES)


# ---- the reader's session on a fake vrserver
class Clock:
    n, t = 0, 0.0  # (t from whole steps, so 30 s is 30 s)


gp.time = types.SimpleNamespace(monotonic=lambda: Clock.t)
STEP = 0.02  # an awake controller's message each step: 50 a second
REST = {"left": {"/input/grip/click": False, "/input/grip/value": 0, "/input/dpad_up/click": False,
                 "/input/thumbstick/x": 0, "/input/thumbstick/y": 0, "/input/trigger/value": 0},
        "right": {"/input/grip/click": False, "/input/grip/value": 0, "/input/a/click": False,
                  "/input/thumbstick/x": 0, "/input/thumbstick/y": 0, "/input/trigger/value": 0}}


class Server:
    """vrserver's side: its controllers ({path: [side, connected, components]}), the mailbox's
    subscriptions, what the reader sent, and what happens when (events: [(time, call(server))])."""

    def __init__(self, events, end):
        self.devices = {L: ["left", True, dict(REST["left"])], R: ["right", True, dict(REST["right"])]}
        self.subscribed, self.requests, self.queue = set(), [], []
        self.late, self.answers = False, []  # late: an answer comes a read after what was already on its way
        self.events, self.end, self.tick = sorted(events, key=lambda e: e[0]), end, 0

    def states(self):
        return {path: (side, on) for path, (side, on, _) in self.devices.items()}

    def push(self, path, components, answer=False):
        if path in self.subscribed:
            (self.answers if answer else self.queue).append(
                json.dumps({"type": "update_component_states", "device": path, "components": components}))

    def set(self, path, **components):
        """A controller's inputs change (grip_click=True -> /input/grip/click)."""
        named = {"/input/" + k.replace("_", "/", 1) if not k.startswith("dpad") else "/input/" + k.replace("_click", "/click"): v
                 for k, v in components.items()}
        self.devices[path][2].update(named)
        self.push(path, named)

    def drop(self, path):
        """vrserver stops answering for a controller."""
        self.subscribed.discard(path)

    # the web socket, as vrws.VrSocket
    def open(self, mailbox):
        pass

    def close(self):
        pass

    def subscribe(self, path, on=True):
        self.requests.append((round(Clock.t, 2), path, on))
        if not on:
            self.subscribed.discard(path)
        elif path in self.devices:
            self.subscribed.add(path)
            self.push(path, dict(self.devices[path][2]), self.late)  # each request: the whole state

    def messages(self, timeout=None):
        Clock.n += 1
        Clock.t = Clock.n * STEP
        self.tick += 1
        if Clock.t > self.end:
            raise OSError("the test is over")
        while self.events and self.events[0][0] <= Clock.t:
            self.events.pop(0)[1](self)
        for path, (_, on, _) in self.devices.items():
            if on:  # an awake controller is never silent: its capacitive sensing
                self.push(path, {"/input/capsense/grip": self.tick % 7 / 10})
        out, self.queue, self.answers = self.queue, self.answers, []
        return out


class Sock:
    def __init__(self):
        self.sent = []

    def sendto(self, data, to):
        self.sent.append((round(Clock.t, 2), data.decode()))


def run(events, end, pad_at=None, wake=None, pad_then=()):
    """A session of the reader with the pause gesture set (so it reads from the start), gamepad
    mode from pad_at, then off and on as pad_then says ([(time, on)]). Returns (server, log lines,
    datagrams sent)."""
    Clock.n, Clock.t = 0, 0.0
    logs, sock = [], Sock()
    watch = gp.ControllerWatch(logs.append)
    watch.sock.close()
    watch.sock = sock
    watch.set_gesture((("left/thumbstick", "right/thumbstick"), 2))
    pad = ([(pad_at, True)] if pad_at is not None else []) + list(pad_then)
    server = Server(events + [(t, lambda s, on=on: watch.set_pad(on)) for t, on in pad], end)
    if wake:
        wake(server)
    vrws.VrSocket = lambda: server
    vrws.controller_states = lambda timeout=3: server.states()
    try:
        watch.session()
    except OSError as e:
        check("the session ran to the test's end", str(e), "the test is over")
    return server, logs, sock.sent


def asks(server, path, after=-1.0):
    return [t for t, p, on in server.requests if p == path and on and t > after]


def pads(sent, after):
    return {int(m.split()[1], 16) for t, m in sent if m.startswith("vrpad ") and t > after}


BACK = "controller reader: the left controller had said nothing for %d s with SteamVR having it connected; " \
       "asked for its input again, and it's back"

# Nothing goes wrong: one request each, and nothing said.
server, logs, sent = run([], 70.0)
check("a healthy session: each controller asked for once, nothing in the log but the start",
      (asks(server, L), asks(server, R), logs),
      ([0.0], [0.0], ["controller reader: following left and right controllers on SteamVR's web socket"]))

# vrserver stops answering for the left controller, with no gamepad mode: the next lookup asks again.
server, logs, sent = run([(2.0, lambda s: s.drop(L))], 40.0)
check("the left controller's messages stop: asked for again at the next lookup, and said",
      (asks(server, L, 0.0), asks(server, R, 0.0), [m for m in logs if "said nothing" in m]),
      ([gp.LOOKUP_EVERY], [], [BACK % (gp.LOOKUP_EVERY - 2)]))

# The 2026-11-27 run: the left one is lost, then gamepad mode starts; d-pad and both grips after.
events = [(2.0, lambda s: s.drop(L)),
          (6.0, lambda s: s.set(L, dpad_up_click=True)), (6.5, lambda s: s.set(L, dpad_up_click=False)),
          (7.0, lambda s: s.set(L, grip_click=True, grip_value=1.0)),
          (7.0, lambda s: s.set(R, grip_click=True, grip_value=1.0))]
server, logs, sent = run(events, 9.0, pad_at=5.0)
check("gamepad mode starts with the left controller lost: it's asked for again as the mode starts, "
      "as every controller is, once", (asks(server, L, 0.0), asks(server, R, 0.0)), ([5.0], [5.0]))
check("...the d-pad arrives", 0x0001 in pads(sent, 6.0), True)
check("...and both grips leave the mode", [m for t, m in sent if m == "vrgrips"], ["vrgrips"])
check("...and the log says the left one was back (the mode's own lookup saw it connected)",
      [m for m in logs if "said nothing" in m], [BACK % 3])

# Lost during gamepad mode: asked for again within QUIET and a step or two.
server, logs, sent = run([(8.0, lambda s: s.drop(L)), (10.0, lambda s: s.set(L, dpad_up_click=True))], 12.0, pad_at=5.0)
again = asks(server, L, 5.1)
check("lost in gamepad mode: asked for again about QUIET later, once",
      (len(again), [8.0 + QUIET - STEP <= t <= 8.0 + QUIET + 4 * STEP for t in again]), (1, [True]))
check("...its d-pad arrives after, and the log says so",
      (0x0001 in pads(sent, 10.0), [m for m in logs if "said nothing" in m]), (True, [BACK % 1]))


# A controller asleep in gamepad mode (put down, or never picked up): asked for each QUIET, each
# answered once, nothing said; picked up, it's heard.
def asleep(server):
    server.devices[L][1] = False


def wakes(server):
    server.devices[L][1] = True


server, logs, sent = run([(9.0, wakes), (9.5, lambda s: s.set(L, dpad_up_click=True))], 11.0, pad_at=3.0, wake=asleep)
sleeping = asks(server, L, 0.0)
check("asleep in gamepad mode: asked for about each QUIET while it sleeps, and no more once it's awake",
      (5 <= len(sleeping) <= 7, min(sleeping, default=0) >= 3.0, max(sleeping, default=99) <= 9.0 + STEP), (True, True, True))
check("...nothing said about it, and its d-pad arrives once it's awake",
      ([m for m in logs if "said nothing" in m], 0x0001 in pads(sent, 9.5)), ([], True))



# A button held as the mode starts (council, 2026-11-27): the relay keeps from the game what is
# down in a mode's first "vrpad", so that one has to carry the controllers' answers. vrserver's
# answer to a request comes after the messages it had already sent: here, one read later.
def late(server):
    server.late = True


server, logs, sent = run([(2.0, lambda s: s.set(R, a_click=True))], 7.0, pad_at=5.0, wake=late)
first = [(t, int(m.split()[1], 16)) for t, m in sent if m.startswith("vrpad ")][:1]
check("a button held as gamepad mode starts is down in the mode's first state, sent PAD_SETTLE after it starts",
      [(5.0 + gp.PAD_SETTLE <= t <= 5.0 + gp.PAD_SETTLE + 2 * STEP, hex(b)) for t, b in first], [(True, "0x1000")])
check("...with both controllers asked for as it started", (asks(server, L, 4.9), asks(server, R, 4.9)), ([5.0], [5.0]))


# The mode ends and starts again within one turn of the reader's loop (council, 2026-11-27): the
# reader never sees it off, and it is a new mode all the same, read from nothing.
server, logs, sent = run([], 5.0, pad_at=1.0, pad_then=[(3.0, False), (3.0, True)])
restart = [t for t, m in sent if m.startswith("vrpad ") and t > 3.0][:1]
check("off and on again in one turn: every controller is asked for again, and the first state waits for the answers",
      (asks(server, L, 1.1), asks(server, R, 1.1), [3.0 + gp.PAD_SETTLE <= t <= 3.0 + gp.PAD_SETTLE + 2 * STEP for t in restart]),
      ([3.0], [3.0], [True]))


# Left by the grips, then entered again (council, 2026-11-27): the grips are let go, and a stick
# with them, after the mode is off, when nothing is read. The next mode starts from the
# controllers' state as it is then, not from the squeeze that ended the last one.
def sticks(sent, after):
    return {int(m.split()[4]) for t, m in sent if m.startswith("vrpad ") and t > after}


events = [(1.5, lambda s: s.set(L, thumbstick_x=1.0)),
          (2.0, lambda s: s.set(L, grip_click=True, grip_value=1.0)),
          (2.0, lambda s: s.set(R, grip_click=True, grip_value=1.0)),
          (2.8, lambda s: s.set(L, grip_click=False, grip_value=0, thumbstick_x=0)),
          (2.8, lambda s: s.set(R, grip_click=False, grip_value=0)),
          (7.0, lambda s: s.set(L, dpad_up_click=True)),
          (8.0, lambda s: s.set(L, grip_click=True, grip_value=1.0)),
          (8.0, lambda s: s.set(R, grip_click=True, grip_value=1.0))]
server, logs, sent = run(events, 9.0, pad_at=1.0, pad_then=[(2.4, False), (6.0, True)])
left = [t for t, m in sent if m == "vrgrips"]
check("the grips leave one mode, and are let go with it off: the next mode isn't left by them",
      (len(left), [2.0 + gp.GRIPS_HOLD <= left[0] <= 2.4] if left else None, [m for t, m in sent if m == "vrgrips" and 6.0 <= t < 8.0]),
      (2, [True], []))
check("...nor is the stick let go then still pushed", (32767 in sticks(sent, 1.6), sticks(sent, 6.0)), (True, {0}))
check("...each controller was asked for as the second mode started, and its d-pad arrives",
      (asks(server, L, 5.9), asks(server, R, 5.9), 0x0001 in pads(sent, 7.0)), ([6.0], [6.0], True))
check("...and a squeeze in it leaves it", [8.0 + gp.GRIPS_HOLD <= t <= 8.0 + gp.GRIPS_HOLD + 0.1 for t in left[1:]], [True])

print(f"{len(failures)} failed" if failures else "all passed")
sys.exit(1 if failures else 0)
