"""Pause Frametop for VR games, so it leaves the headset's CPU and GPU to the game. The input
relay (input-relay.py) runs this: it's the one part of Frametop that always runs.

Paused:
  - The gaze service stops (frametop-gaze: ft-gazed, ft-gaze, our own eye tracker ft-eyes, the
    gaze panel), and with it every read of SteamVR's eye tracking. Our frame grabber, the root
    service ft-eyegrab, goes idle by itself 3 seconds after ft-eyes stops asking for frames.
  - Hand tracking stops if it runs (frametop-camd, frametop-hands). With hand interaction on
    (POINTER_HANDS=1) it's parked instead (PARK_PATH: ft-hands stops processing and ft-camd stops
    copying, the models stay loaded), and it unparks while the dashboard is open, so the hands work
    there during the game. With HANDS_MODE=always it keeps running, unparked: the hands show in
    the game (hands/handsmode.py).
  - The desktop, by "pause_desktop": "hide" (the default) hides every screen and floating window
    and slows the desktop down ("pause on" to ft-screens: KWin gets one frame callback a second
    instead of 90, so it and the apps on it hardly draw), and remote desktop stops if it runs
    (session/remote-ctl.sh: krdp, Xvnc, FreeRDP). Windows stay open. "close" closes the desktop
    (desktops.sh stop): its windows close, and resuming starts it again (about 12 seconds), in
    its start profile if it has one.
  - In the relay: the 3D mouse lets go and the mouse is a plain mouse for SteamVR, typing goes to
    Steam, and mapped buttons and key combinations do nothing but PAUSED_ACTIONS and commands.
Resuming starts again only what pausing stopped. The pointer helper and ft-powerd keep running:
they cost little, and the helper is what says a VR game started.

Toggled by:
  - The pause_toggle action: a mouse button, a key combination, or a controller button outside
    games (the pointer helper only reads those outside games).
  - A controller gesture, by default both thumbsticks clicked together twice ("pause_gesture").
    It's read from vrserver's web socket (vrws.py), which works in games and takes nothing from
    them, so the game sees the clicks too. Once it goes off, it's ignored for GESTURE_COOLDOWN:
    on 2026-10-16 a resume was followed by a pause 2 s later, which left Frametop paused.
  - input/ft-pause on|off|toggle|status, which uses the relay's control socket
    ("pause on|off|toggle|?").
  - VR games, with "pause_auto" (on by default): the pointer helper says "vrgame 1|0" as a SteamVR
    scene app starts and ends (and every 5 seconds). A game starting pauses; a pause that starts
    while a game runs ends RESUME_DELAY after the game does, unless another game starts first.
    Toggled back on during a game, Frametop stays on until that game ends. A pause that starts
    outside a game lasts until it's toggled off.

Settings in ~/.config/frametop-input.json (Frametop Input Settings, Game optimization page): "pause_auto"
(bool), "pause_gesture" ({"buttons": one or two controller buttons, "presses": 1 or 2}, or null
for none; one button always takes two presses), "pause_desktop" ("hide" or "close"), and
"pause_sound" (a sound on pause and resume; bool). The state outlives a relay restart in
STATE_PATH, and a desktop that starts while paused is told (screens_seen), so it hides too.
"""
import json
import os
import queue
import socket
import subprocess
import threading
import time

import vrws

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DESKTOPS = os.path.join(REPO, "desktops.sh")
REMOTE_CTL = os.path.join(REPO, "session", "remote-ctl.sh")
RELAY = "\0frametop_relay"
SCREENS = "\0ft_screens"
HELPER = "\0ft_pointer_helper"
STATE_PATH = f"/run/user/{os.getuid()}/frametop-pause.json"
# User services that pausing stops, if they run.
UNITS = ("frametop-gaze.service", "frametop-hands.service", "frametop-camd.service")
# With hand interaction on (POINTER_HANDS=1), pausing parks hand tracking instead of stopping it, so
# the hands are there when the dashboard opens during the game: ft-hands stops processing while
# PARK_PATH exists (and ft-camd stops copying frames for it), and starts again within a palm search
# (about 200 ms) once it goes. The pointer helper says when the dashboard opens and closes ("vrdash").
HAND_UNITS = ("frametop-hands.service", "frametop-camd.service")
PARK_PATH = f"/run/user/{os.getuid()}/frametop-hands/park"
# What still works while paused, besides commands ("command:CMD"): the toggle itself, Steam's menu,
# and dictation (Frametop Voice types into games too).
PAUSED_ACTIONS = ("pause_toggle", "steam_menu", "dictate", "dictate_hold")
DEFAULT_GESTURE = {"buttons": ["left/thumbstick", "right/thumbstick"], "presses": 2}
DESKTOP_MODES = ("hide", "close")
SOUNDS = {True: "/usr/share/sounds/ocean/stereo/device-removed.oga",
          False: "/usr/share/sounds/ocean/stereo/device-added.oga"}
RESUME_DELAY = 5.0  # after a VR game ends: loading the next one can end one scene app and start another
GAME_STALE = 12.0  # the helper repeats "vrgame" every 5 s; silence this long means no game (or no SteamVR)
CHORD_WINDOW = 0.3  # a two-button gesture's buttons go down within this of each other
DOUBLE_WINDOW = 0.7  # a double press's second press comes within this of the first
GESTURE_COOLDOWN = 3.0  # once the gesture goes off, it's ignored this long (a triple press, a repeat)
LOOKUP_EVERY = 30.0  # the controllers are looked up again this often (and sooner when they may have changed)
LOOKUP_GAP = 3.0  # but not more often than this for a message from a device it doesn't know


def device_of(text):
    """A web socket message's "device", without parsing all of it (there are about 160 a second)."""
    at = text.find('"device"')
    if at < 0:
        return None
    start = text.find('"', text.find(":", at) + 1)
    end = text.find('"', start + 1)
    return text[start + 1:end] if 0 <= start < end else None


def gesture_of(rules, buttons):
    """The rules' pause gesture as (buttons, presses), or None for none. `buttons` are the
    controller buttons there are (the relay's VR_BUTTONS)."""
    spec = rules.get("pause_gesture", DEFAULT_GESTURE)
    if not isinstance(spec, dict) or not isinstance(spec.get("buttons"), list):
        return None
    chosen = tuple(dict.fromkeys(b for b in spec["buttons"] if b in buttons))[:2]
    if not chosen:
        return None
    return chosen, 1 if len(chosen) == 2 and spec.get("presses") == 1 else 2


def settings_of(rules, buttons):
    return {"auto": rules.get("pause_auto", True) is not False,
            "gesture": gesture_of(rules, buttons),
            "desktop": rules.get("pause_desktop") if rules.get("pause_desktop") in DESKTOP_MODES else "hide",
            "sound": rules.get("pause_sound", True) is not False}


class Gesture:
    """The pause gesture, from button presses and releases. A press of a two-button gesture is
    both buttons going down within CHORD_WINDOW of each other, so a button held for a while (to
    sprint, say) and the other clicked doesn't count. Two presses within DOUBLE_WINDOW make a
    double press. Once it goes off, it's ignored for GESTURE_COOLDOWN."""

    def __init__(self, buttons, presses):
        self.buttons, self.presses = tuple(buttons), presses
        self.down = {}  # button -> when it went down
        self.first = None  # when the first press of a double press was
        self.fired = None
        self.ignored = None  # a gesture the cooldown dropped: how long after the last one (for the log)

    def reset(self):
        self.down.clear()
        self.first = None

    def feed(self, button, pressed, now):
        """A button's state; True when it completes the gesture."""
        if button not in self.buttons:
            return False
        if not pressed:
            self.down.pop(button, None)
            return False
        if button in self.down:
            return False  # still down
        self.down[button] = now
        if len(self.down) < len(self.buttons) or now - min(self.down.values()) > CHORD_WINDOW:
            return False
        if self.presses == 2 and (self.first is None or now - self.first > DOUBLE_WINDOW):
            self.first = now
            return False
        self.first = None
        if self.fired is not None and now - self.fired < GESTURE_COOLDOWN:
            self.ignored = now - self.fired
            return False
        self.fired = now
        return True


class TouchTracker:
    """Who holds a controller, for the pointer helper's handoff arbiter (pointer/helper/handoff.h):
    per side, whether any touch sensor of that side's controller is touched, a button of it went
    down, or it's gone. Fed the web socket's messages; returns the datagrams for the helper, only
    on a change:
      ctl <side> touch 0|1   any touch component of that side's controller(s) is true
      ctl <side> press       a click component went down
      ctl <side> gone        no controller on that side any more
      ctl ws 0|1             the web socket is up or down (ControllerWatch sends these)
    A side can have more than one controller (the testbench's synthetic ones beside a real one):
    any of them counts. vrserver's first message for a subscription has every component, so the
    state is known from the start; its clicks aren't presses."""

    def __init__(self):
        self.sides = {}  # root path -> side
        self.touch = {}  # root path -> {component: bool}
        self.clicks = {}  # root path -> {component: bool}
        self.sent = {}  # side -> the touch value last sent

    def set_sides(self, sides):
        """The controllers there are now ({root path: side}); "gone" for a side that has none left."""
        out = []
        for path in list(self.sides):
            if path not in sides:
                self.touch.pop(path, None)
                self.clicks.pop(path, None)
        old = set(self.sides.values())
        self.sides = dict(sides)
        for side in sorted(old - set(self.sides.values())):
            self.sent.pop(side, None)
            out.append(f"ctl {side} gone")
        for side in sorted(set(self.sides.values())):  # a touched controller of a side went
            if side in self.sent and self.sent[side] != self.side_touched(side):
                self.sent[side] = self.side_touched(side)
                out.append(f"ctl {side} touch {int(self.sent[side])}")
        return out

    def side_touched(self, side):
        return any(v for p, comps in self.touch.items() if self.sides.get(p) == side for v in comps.values())

    def feed(self, path, components):
        side = self.sides.get(path)
        if side is None:
            return []
        out = []
        first = path not in self.clicks
        clicks = self.clicks.setdefault(path, {})
        touch = self.touch.setdefault(path, {})
        pressed = False
        for name, value in components.items():
            if name.endswith("/touch"):
                touch[name] = bool(value)
            elif name.endswith("/click"):
                if value and not clicks.get(name, False) and not first:
                    pressed = True
                clicks[name] = bool(value)
        if pressed:
            out.append(f"ctl {side} press")
        now = self.side_touched(side)
        if self.sent.get(side) != now:
            self.sent[side] = now
            out.append(f"ctl {side} touch {int(now)}")
        return out

    def snapshot(self):
        """Every side's touch state, as datagrams (for a helper that just started)."""
        return [f"ctl {side} touch {int(self.side_touched(side))}" for side in sorted(set(self.sides.values()))]


class ControllerWatch(threading.Thread):
    """Follows the Frame controllers on vrserver's web socket for the pause gesture, and sends
    the relay "pause toggle gesture" when it's made. With the touch feed on (hand interaction,
    POINTER_HANDS=1), it also sends the pointer helper who holds a controller (TouchTracker).
    Reconnects every 5 seconds while SteamVR is away. With no gesture set and no touch feed, it
    doesn't connect."""

    def __init__(self, log):
        super().__init__(name="pause-gesture", daemon=True)
        self.log = log
        self.lock = threading.Lock()
        self.gesture = None
        self.touch_feed = False
        self.tracker = TouchTracker()
        self.resend = False  # the helper (re)started: send it the touch state again
        self.changed = threading.Event()
        self.connected = False
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.lookup_at = None  # look the controllers up again then (see recheck)

    def set_touch_feed(self, on):
        if on != self.touch_feed:
            self.touch_feed = on
            self.log(f"controller touch feed for the pointer helper {'on' if on else 'off'}")
            if not on:
                self.to_helper(["ctl ws 0"])
            self.changed.set()

    def helper_started(self):
        self.resend = True

    def to_helper(self, msgs):
        for m in msgs:
            try:
                self.sock.sendto(m.encode(), HELPER)
            except OSError:
                pass  # the helper isn't running; it says vrhello when it starts

    def recheck(self, delay=1.5):
        """The controllers' root paths may change soon: the 3D mouse's virtual controller is taking
        or giving back its hand role. SteamVR takes a moment to hand the role over."""
        self.lookup_at = time.monotonic() + delay

    def set_gesture(self, spec):
        """(buttons, presses), or None for no gesture."""
        with self.lock:
            g = self.gesture
            if spec is None:
                self.gesture = None
            elif g is None or (g.buttons, g.presses) != spec:
                self.gesture = Gesture(*spec)
        self.changed.set()

    def current(self):
        with self.lock:
            return self.gesture

    def run(self):
        while True:
            if self.current() is None and not self.touch_feed:
                if self.connected:
                    self.log("pause gesture: none set, stopped reading the controllers")
                self.connected = False
                self.changed.wait()
                self.changed.clear()
                continue
            try:
                self.session()
            except (OSError, ValueError) as e:  # urllib's errors are OSErrors
                if self.connected:
                    self.log(f"pause gesture: SteamVR's web socket went away ({e}); retrying")
                self.connected = False
            if self.touch_feed:
                self.to_helper(["ctl ws 0"])
            self.changed.wait(5)
            self.changed.clear()

    def session(self):
        ws = vrws.VrSocket()
        try:
            ws.open(f"frametop_pause_{os.getpid()}")
            sides = {}  # root path -> side
            next_poll = 0.0
            last_poll = 0.0
            self.tracker = TouchTracker()
            touch_was = False
            while True:
                g = self.current()
                touch = self.touch_feed
                if g is None and not touch:
                    return
                if touch and (not touch_was or self.resend):
                    # the feed just started, or the helper restarted: where things stand
                    self.resend = False
                    self.to_helper(["ctl ws 1"] + self.tracker.snapshot())
                touch_was = touch
                now = time.monotonic()
                lookup_at = self.lookup_at
                if lookup_at is not None and now >= lookup_at:
                    self.lookup_at = None
                    next_poll = now
                if now >= next_poll:
                    # A controller connects later, or changes its root path when the 3D mouse's
                    # virtual controller takes or gives back its hand role. Each lookup is an HTTP
                    # request to vrserver, so it's at connect, every LOOKUP_EVERY, when a message
                    # comes from a device not on the list, and when the relay says (recheck).
                    next_poll, last_poll = now + LOOKUP_EVERY, now
                    found = vrws.controllers()
                    if found != sides:
                        for path in sides.keys() - found.keys():
                            ws.subscribe(path, on=False)
                        for path in found.keys() - sides.keys():
                            ws.subscribe(path)
                        sides = found
                        if g is not None:
                            g.reset()
                        msgs = self.tracker.set_sides(sides)
                        if touch:
                            self.to_helper(msgs)
                    if not self.connected:
                        self.connected = True
                        self.log(f"{'pause gesture' if g is not None else 'touch feed'}: following "
                                 f"{' and '.join(sorted(sides.values())) or 'no'} "
                                 f"controller{'s' if len(sides) != 1 else ''} on SteamVR's web socket")
                wanted = {} if g is None else \
                    {b: (b.split("/", 1)[0], f"/input/{b.split('/', 1)[1]}/click") for b in g.buttons}
                for text in ws.messages(timeout=1.0):
                    device = device_of(text)
                    if device and device not in sides:
                        next_poll = min(next_poll, last_poll + LOOKUP_GAP)  # its root path changed, maybe
                    # About 160 messages a second, nearly all capacitive sensing: parse only the
                    # few that carry one of the gesture's buttons, or with the touch feed on, the
                    # touch and click ones.
                    feed = touch and device in sides and ("/touch" in text or "/click" in text)
                    if not feed and not any(c in text for _, c in wanted.values()):
                        continue
                    try:
                        msg = json.loads(text)
                    except ValueError:
                        continue
                    side = sides.get(msg.get("device"))
                    components = msg.get("components") or {}
                    if feed:
                        self.to_helper(self.tracker.feed(msg.get("device"), components))
                    for button, (s, component) in wanted.items():
                        if s == side and component in components:
                            if g.feed(button, bool(components[component]), time.monotonic()):
                                self.log("pause gesture")
                                try:
                                    self.sock.sendto(b"pause toggle gesture", RELAY)
                                except OSError:
                                    pass
                            elif g.ignored is not None:
                                self.log(f"pause gesture ignored: {g.ignored:.1f} s after the last one")
                                g.ignored = None
        finally:
            ws.close()


def run(cmd, timeout=30):
    """A command's exit status (None if it couldn't run), its output going nowhere."""
    try:
        return subprocess.run(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL, timeout=timeout).returncode
    except (OSError, subprocess.TimeoutExpired):
        return None


def output(cmd, timeout=10):
    try:
        return subprocess.run(cmd, stdin=subprocess.DEVNULL, capture_output=True, text=True,
                              timeout=timeout).stdout
    except (OSError, subprocess.TimeoutExpired):
        return ""


def active(unit):
    return run(["systemctl", "--user", "-q", "is-active", unit], timeout=10) == 0


def desktop_running():
    return run(["pgrep", "-x", "ft-screens"], timeout=5) == 0


class Worker(threading.Thread):
    """Does the slow part of pausing and resuming, in order, one job at a time: stopping and
    starting services, remote desktop, and closing or starting the desktop. `stopped` is what
    the last pause stopped, for the resume to start again."""

    def __init__(self, log, save):
        super().__init__(name="pause-worker", daemon=True)
        self.log, self.save = log, save
        self.jobs = queue.Queue()
        self.lock = threading.Lock()
        self.stopped = {"units": [], "remote": False, "desktop": None}
        self.busy = False
        self.keep = ()  # units pausing leaves running (hand tracking, parked instead)

    def submit(self, job, *args):
        self.busy = True
        self.jobs.put((job, args))

    def run(self):
        while True:
            job, args = self.jobs.get()
            try:
                job(*args)
            except Exception as e:  # one failure mustn't stop the next pause or resume
                self.log(f"pause: {job.__name__} failed: {e!r}")
            self.busy = not self.jobs.empty()
            self.save()

    def snapshot(self):
        with self.lock:
            return {"units": list(self.stopped["units"]), "remote": self.stopped["remote"],
                    "desktop": self.stopped["desktop"]}

    def restore(self, stopped):
        with self.lock:
            self.stopped = {"units": [u for u in stopped.get("units", []) if u in UNITS],
                            "remote": bool(stopped.get("remote")),
                            "desktop": stopped.get("desktop") if stopped.get("desktop") in ("hidden", "closed") else None}

    def stop_units(self):
        """Stop the UNITS that run, adding them to what the resume starts again."""
        units = [u for u in UNITS if u not in self.keep and active(u)]
        if units:
            run(["systemctl", "--user", "stop", *units], timeout=30)
            with self.lock:
                self.stopped["units"] += [u for u in units if u not in self.stopped["units"]]
            self.log(f"pause: stopped {', '.join(units)}")

    def pause(self, desktop):
        if desktop == "close":
            if desktop_running():
                run([DESKTOPS, "stop"], timeout=40)  # remote desktop goes with it
                with self.lock:
                    self.stopped["desktop"] = "closed"
                self.log("pause: desktop closed")
        else:
            if "running=1" in output([REMOTE_CTL, "status"]):
                run([REMOTE_CTL, "stop"])
                with self.lock:
                    self.stopped["remote"] = True
                self.log("pause: remote desktop stopped")
            with self.lock:
                self.stopped["desktop"] = "hidden"  # ft-screens was told already (or isn't running)
        self.stop_units()

    def enforce(self):
        """While paused: SteamVR restarted, and started the gaze service with it."""
        self.stop_units()

    def resume(self):
        stopped = self.snapshot()
        units = stopped["units"]
        if units:
            # They need SteamVR (Requisite=steamvr.service): without it they start with it next time.
            if run(["systemctl", "--user", "start", *units], timeout=30) == 0:
                self.log(f"resume: started {', '.join(units)}")
            else:
                self.log(f"resume: couldn't start {', '.join(units)} (SteamVR not running?)")
        if stopped["remote"]:
            # In a scope of its own, not this service's, so a relay restart doesn't end it.
            run(["systemd-run", "--user", "--scope", "--collect", "--quiet", REMOTE_CTL, "start"])
            self.log("resume: remote desktop started")
        if stopped["desktop"] == "closed":
            if run([DESKTOPS, "start"], timeout=60) == 0:
                self.log("resume: desktop started")
            else:
                self.log("resume: the desktop didn't start (see /tmp/frametop-session.log)")
        with self.lock:
            self.stopped = {"units": [], "remote": False, "desktop": None}


class GamePause:
    """The pause state, its automatic side, and the relay's way in. `on_change(paused)` is
    called (from the relay's thread) when the state flips, for the relay's own part."""

    def __init__(self, log, on_change, buttons):
        self.log, self.on_change, self.buttons = log, on_change, buttons
        self.lock = threading.Lock()  # the state file, written from both threads
        self.paused = False
        self.reason = ""  # what paused it: gesture, button, key, controller, command, game
        self.since = 0.0  # wall clock
        self.ends_with_game = False  # resume when the VR game ends
        self.game = False
        self.game_seen = 0.0  # last "vrgame" from the helper
        self.resume_at = None
        self.settings = settings_of({}, buttons)
        self.hands = False  # hand interaction is on (set_hands)
        self.hands_always = False  # HANDS_MODE=always: hand tracking runs through games (set_hands)
        self.dashboard = False  # the SteamVR dashboard is open (the helper's "vrdash")
        self.parked = None  # hand tracking parked, as last applied (None: not yet)
        self.screens_pid = None  # the ft-screens that last said where typing goes (screens_seen)
        self.screens = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM | socket.SOCK_NONBLOCK)
        self.worker = Worker(log, self.save)
        self.watch = ControllerWatch(log)
        self.worker.start()
        self.watch.start()
        self.load()

    # --- state file ---
    def save(self):
        with self.lock:
            state = {"paused": self.paused, "reason": self.reason, "since": self.since,
                     "ends_with_game": self.ends_with_game, "stopped": self.worker.snapshot()}
            try:
                tmp = STATE_PATH + ".tmp"
                with open(tmp, "w") as f:
                    json.dump(state, f)
                os.replace(tmp, STATE_PATH)
            except OSError as e:
                self.log(f"pause: can't save {STATE_PATH}: {e}")

    def load(self):
        """A relay restarted while paused stays paused (the relay's part is applied by its first
        configure)."""
        try:
            with open(STATE_PATH) as f:
                state = json.load(f)
        except (OSError, ValueError):
            return
        if not isinstance(state, dict) or not state.get("paused"):
            return
        self.paused = True
        self.reason = str(state.get("reason", ""))
        self.since = float(state.get("since", 0) or 0)
        self.ends_with_game = bool(state.get("ends_with_game"))
        self.worker.restore(state.get("stopped") or {})
        self.log(f"paused since before this relay started ({self.reason})")
        self.worker.submit(self.worker.enforce)

    # --- settings ---
    def configure(self, rules):
        self.settings = settings_of(rules, self.buttons)
        self.watch.set_gesture(self.settings["gesture"])
        if not self.settings["auto"]:
            self.ends_with_game, self.resume_at = False, None

    # --- pausing and resuming ---
    def sound(self, paused):
        if self.settings["sound"] and os.path.exists(SOUNDS[paused]):
            try:
                subprocess.Popen(["pw-play", SOUNDS[paused]], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL, start_new_session=True)
            except OSError:
                pass

    def tell_screens(self, paused):
        try:
            self.screens.sendto(b"pause on" if paused else b"pause off", SCREENS)
        except OSError:
            pass  # the desktop isn't running

    def screens_seen(self, pid):
        """From the relay: ft-screens `pid` said where typing goes, as it does every second. One
        that started while paused never heard "pause on" and shows its screens, while the relay
        keeps the 3D mouse off: on 2026-10-16 an update restarted the desktop and the relay, the
        relay came back paused from STATE_PATH, and the desktop looked on with a dead pointer.
        So a new ft-screens hears the pause."""
        if not pid or pid == self.screens_pid:
            return
        self.screens_pid = pid
        if self.paused:
            self.log(f"telling ft-screens ({pid}) that Frametop is paused")
            self.tell_screens(True)

    def tell_helper(self):
        """The pointer helper's hand interaction runs in a VR game only while Frametop isn't paused
        (toggled back on during the game) or the dashboard is open: "paused 1|0", on a change and
        when the helper starts."""
        try:
            self.screens.sendto(b"paused 1" if self.paused else b"paused 0", HELPER)
        except OSError:
            pass  # the helper isn't running; it says vrhello when it starts

    def set_hands(self, on, always=False):
        """Hand interaction (POINTER_HANDS=1): feed the helper who holds a controller, and park hand
        tracking for games instead of stopping it. always (HANDS_MODE=always): neither."""
        self.hands, self.hands_always = on, always
        self.worker.keep = HAND_UNITS if on or always else ()
        self.watch.set_touch_feed(on)
        self.update_park()

    def dashboard_state(self, on):
        """From the pointer helper: the dashboard opened or closed ("vrdash 1|0")."""
        self.dashboard = on
        self.update_park()

    def update_park(self):
        """Hand tracking is parked while Frametop is paused with hand interaction on, unless the
        dashboard is open (hands work there during a game) or HANDS_MODE=always."""
        want = self.paused and self.hands and not self.hands_always and not self.dashboard
        if want == self.parked:
            return
        try:
            if want:
                if os.path.isdir(os.path.dirname(PARK_PATH)):  # hand tracking has run: it'll read it
                    with open(PARK_PATH, "w") as f:
                        f.write("parked by the input relay (Frametop paused for a VR game)\n")
            elif os.path.exists(PARK_PATH):
                os.remove(PARK_PATH)
        except OSError as e:
            self.log(f"hand tracking: can't {'park' if want else 'unpark'} it: {e}")
            return
        if self.parked is not None or want:
            self.log(f"hand tracking {'parked' if want else 'unparked'}"
                     f"{' (the dashboard is open)' if not want and self.paused else ''}")
        self.parked = want

    def pause(self, reason, now):
        if self.paused:
            return
        self.paused, self.reason, self.since = True, reason, time.time()
        self.ends_with_game = self.game and self.settings["auto"]
        self.resume_at = None
        self.log(f"Frametop paused ({reason}{', until the game ends' if self.ends_with_game else ''})")
        self.sound(True)
        self.tell_screens(True)
        self.tell_helper()
        self.update_park()  # at once, even when the desktop is closing: that takes a while
        self.on_change(True)
        self.worker.submit(self.worker.pause, self.settings["desktop"])
        self.save()

    def resume(self, reason, now):
        if not self.paused:
            return
        self.paused, self.resume_at, self.ends_with_game = False, None, False
        self.log(f"Frametop resumed ({reason})")
        self.sound(False)
        self.tell_screens(False)
        self.tell_helper()
        self.update_park()
        self.on_change(False)
        self.worker.submit(self.worker.resume)
        self.save()

    def toggle(self, reason, now):
        """Pausing only happens by itself as a game starts, so a game resumed during stays on."""
        (self.resume if self.paused else self.pause)(reason, now)

    def set(self, on, reason, now):
        if on != self.paused:
            self.toggle(reason, now)

    # --- VR games ---
    def game_state(self, running, now):
        """From the pointer helper: a VR game runs or not (on a change and every 5 s)."""
        self.game_seen = now
        if running == self.game:
            return
        self.game = running
        self.log(f"a VR game {'started' if running else 'ended'}")
        if running:
            self.resume_at = None  # the next game came before the resume
            if self.settings["auto"]:
                self.pause("game", now)
        elif self.paused and self.ends_with_game:
            self.resume_at = now + RESUME_DELAY

    def tick(self, now):
        if self.game and now - self.game_seen > GAME_STALE:
            self.game_state(False, now)  # the helper went quiet: SteamVR (or the helper) stopped
        if self.resume_at is not None and now >= self.resume_at:
            self.resume("the game ended", now)

    def timeout(self, now):
        """How long the relay may wait before tick() has something to do."""
        if self.resume_at is not None:
            return max(0.05, self.resume_at - now)
        return 1.0 if self.game else 3600.0

    def controllers_changed(self):
        """The 3D mouse took or gave back its hand role, which changes a controller's root path."""
        self.watch.recheck()

    def helper_started(self):
        """The pointer helper (re)started, so SteamVR did too, maybe with the gaze service."""
        self.tell_helper()
        self.watch.helper_started()
        if self.paused:
            self.worker.submit(self.worker.enforce)

    def status(self):
        return {"t": "pause", "paused": self.paused, "reason": self.reason, "since": self.since,
                "ends_with_game": self.ends_with_game, "game": self.game,
                "busy": self.worker.busy, "stopped": self.worker.snapshot(),
                "gesture_reader": self.watch.connected, "auto": self.settings["auto"],
                "desktop": self.settings["desktop"]}
