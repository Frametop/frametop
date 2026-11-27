"""Gamepad mode for Frametop panels: the Frame controllers as one Xbox pad for a panel (the input
relay runs it; input-relay.py's "Gamepad mode").

SteamVR's own Gamepad mode decides when it's on, as for a game: with the Steam menu open,
ft-screens shows an "Enter Gamepad Mode" pill above the panel typing goes to. Its click has
ft-dashpatch push SteamVR's input focus onto ft-screens' hidden dashboard overlay frametop.gamepad
and close the Steam menu, so SteamVR's controllers go to Gamepad mode for it; the grips bring the
laser back, as anywhere in SteamVR. ft-dashpatch reads which it is in the dashboard's page, and
ft-screens tells the relay "gamepad desktop|game|remote" each second while Gamepad mode is Frametop's
(for the panel clicked), "gamepad off" as it ends. SteamVR gives an overlay no input in Gamepad
mode (2026-10-25: no actions, no button events), so the relay's controller reader
(game_pause.ControllerWatch, vrserver's web socket) reads every controller button, both sticks and
both triggers raw, and says the pad's state ("vrpad B LT RT LX LY RX RY", XInput's buttons and
up-positive sticks) on each change and every 250 ms. The relay sends it on:
  - a remote display: to ft-screens ("pad ..."), which hands it to that display's ft-stream, which
    gives the host a Moonlight Xbox controller (stream/ft-stream.cpp)
  - a Frametop desktop panel: VirtualPad, an Xbox 360 pad on uinput, which its apps read.
  - a gamescope app's panel (a Steam game left on a Frametop panel): a second VirtualPad under the
    identity of Steam's own virtual gamepad (STEAM), there only while gamepad mode is on for it.
    Steam starts a game with every other pad on SDL's ignore list, the desktop's 045e:028e among
    them (2026-11-27, Slay the Spire 2: it never opened that one, and opened this one).
It ends too when Frametop pauses, a VR game starts, or ft-screens goes quiet for LEASE. With the
dashboard opening during gamepad mode (the helper's "vrdash 1") the pad lets go until it closes, and
buttons held as it comes on or the dashboard closes (the pill's click) wait for their release. Only
an opening counts, not a dashboard the helper still calls open as gamepad mode starts: on SteamOS
0.4.5 IsDashboardVisible() stays true after the pill closed it, and that dropped every state
(issue 105). SteamVR leaves Gamepad mode itself when its dashboard really opens. A pad with no word from the reader for
STALE lets go of everything, and one that changes targets lets go on the old one.
"""

import fcntl
import os
import struct
import subprocess
from typing import NamedTuple

STALE = 1.0  # no "vrpad" line from the controller reader this long: everything comes up
LEASE = 3.0  # no "gamepad desktop|game|remote" from ft-screens this long: gamepad mode ends
NEUTRAL = (0, 0, 0, 0, 0, 0, 0)  # buttons, LT, RT, LX, LY, RX, RY
SOUNDS = {True: "/usr/share/sounds/ocean/stereo/message-contact-in.oga",
          False: "/usr/share/sounds/ocean/stereo/message-contact-out.oga",
          None: "/usr/share/sounds/ocean/stereo/completion-fail.oga"}  # couldn't start

# XInput's button bits (the helper's kPad*, Moonlight's *_FLAG).
PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT = 0x0001, 0x0002, 0x0004, 0x0008
PAD_START, PAD_BACK, PAD_LS, PAD_RS = 0x0010, 0x0020, 0x0040, 0x0080
PAD_LB, PAD_RB, PAD_A, PAD_B, PAD_X, PAD_Y = 0x0100, 0x0200, 0x1000, 0x2000, 0x4000, 0x8000
PAD_ALL = 0xF3FF

# Linux input constants (input-event-codes.h, uinput.h): the event record, the event types and
# the uinput ioctls are ft_evdev's (shared with the relay); the pad's codes are here.
from ft_evdev import (  # noqa: E402
    EV_ABS,
    EV_KEY,
    EV_SYN,
    EVENT,
    SYN_REPORT,
    UI_ABS_SETUP,
    UI_DEV_CREATE,
    UI_DEV_SETUP,
    UI_SET_ABSBIT,
    UI_SET_EVBIT,
    UI_SET_KEYBIT,
)
BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL, BTN_TR = 0x130, 0x131, 0x133, 0x134, 0x136, 0x137
BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR = 0x13A, 0x13B, 0x13C, 0x13D, 0x13E
ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ, ABS_HAT0X, ABS_HAT0Y = 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x10, 0x11
BUS_USB = 0x03
# What xpad makes of an Xbox 360 pad: buttons, the d-pad as a hat, sticks down-positive.
KEYS = {PAD_A: BTN_A, PAD_B: BTN_B, PAD_X: BTN_X, PAD_Y: BTN_Y, PAD_LB: BTN_TL, PAD_RB: BTN_TR,
        PAD_BACK: BTN_SELECT, PAD_START: BTN_START, PAD_LS: BTN_THUMBL, PAD_RS: BTN_THUMBR}
AXES = ((ABS_X, -32768, 32767, 16, 128), (ABS_Y, -32768, 32767, 16, 128), (ABS_RX, -32768, 32767, 16, 128),
        (ABS_RY, -32768, 32767, 16, 128), (ABS_Z, 0, 255, 0, 0), (ABS_RZ, 0, 255, 0, 0),
        (ABS_HAT0X, -1, 1, 0, 0), (ABS_HAT0Y, -1, 1, 0, 0))
ABS_SETUP = struct.Struct("H2x6i")
STORE_NAME = "vpad"  # in systemd's fd store, so a relay restart keeps the same pad


class Identity(NamedTuple):
    """What a uinput pad says it is. keys: the buttons it declares besides KEYS'. store: its name in
    systemd's fd store, or "" for a pad that isn't kept (it's there only while gamepad mode uses it)."""
    name: bytes
    vendor: int
    product: int
    version: int
    keys: tuple
    store: str


# An Xbox 360 pad, as xpad names it: what a desktop panel's apps know.
XBOX = Identity(b"frametop virtual gamepad", 0x045E, 0x028E, 0x0114, (), STORE_NAME)
# Steam's own virtual gamepad, by its ids. Steam starts a game with SDL_GAMECONTROLLER_IGNORE_DEVICES
# naming every pad it knows (045e:028e too) and SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD=1,
# so 28de:11ff is the one pad such a game opens, and Steam itself leaves a 28de:11ff alone (no
# Steam Input on top of it; 2026-11-27: the game held this pad's event node open, Steam didn't).
# Steam's is named "Microsoft X-Box 360 pad <slot>"; this one says whose it is. It declares the
# guide button, never pressed: SDL's mapping for these ids ("Steam Virtual Gamepad") counts the
# stick clicks after it (SDL 3.4: guide b8, the sticks b9 and b10). Not kept: a game in SteamVR's
# theater could take an idle second 28de:11ff beside Steam's own for another player.
STEAM = Identity(b"Frametop gamepad for Steam games", 0x28DE, 0x11FF, 0x0001, (BTN_MODE,), "")
# The pad each kind of panel on this machine reads (ft-screens' word for it). A remote display's
# goes to its host instead.
LOCAL = {"desktop": XBOX, "game": STEAM}


def parse(words):
    """A "vrpad B LT RT LX LY RX RY" line's words after the first, as a state, or None."""
    if len(words) != 7:
        return None
    try:
        buttons = int(words[0], 16)
        values = [int(w) for w in words[1:]]
    except ValueError:
        return None
    if not 0 <= buttons <= 0xFFFF or not all(0 <= v <= 255 for v in values[:2]) \
            or not all(-32768 <= v <= 32767 for v in values[2:]):
        return None
    return (buttons & PAD_ALL, *values)


def screens_line(state):
    """ft-screens' (and ft-stream's) "pad" message for a state."""
    return "pad %x %d %d %d %d %d %d" % state


class VirtualPad:
    """An Xbox pad on uinput under `identity`, made on first use."""

    def __init__(self, log, stored, notify, identity=XBOX):
        self.state = NEUTRAL
        self.log, self.name = log, identity.name.decode()
        if identity.store and identity.store in stored:
            self.fd = stored[identity.store]
            os.set_blocking(self.fd, False)
            log("reusing the gamepad from the fd store")
            # The relay before this one may have left buttons down or sticks over: everything
            # comes up and centres, as NEUTRAL says.
            out = bytearray()
            for code in KEYS.values():
                out += EVENT.pack(0, 0, EV_KEY, code, 0)
            for code, *_ in AXES:
                out += EVENT.pack(0, 0, EV_ABS, code, 0)
            out += EVENT.pack(0, 0, EV_SYN, SYN_REPORT, 0)
            try:
                os.write(self.fd, out)
            except BlockingIOError:
                pass
            return
        self.fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
        fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_KEY)
        for code in (*KEYS.values(), *identity.keys):
            fcntl.ioctl(self.fd, UI_SET_KEYBIT, code)
        fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_ABS)
        for code, low, high, fuzz, flat in AXES:
            fcntl.ioctl(self.fd, UI_SET_ABSBIT, code)
            fcntl.ioctl(self.fd, UI_ABS_SETUP, ABS_SETUP.pack(code, 0, low, high, fuzz, flat, 0))
        fcntl.ioctl(self.fd, UI_DEV_SETUP, struct.pack("HHHH80sI", BUS_USB, identity.vendor, identity.product,
                                                       identity.version, identity.name, 0))
        fcntl.ioctl(self.fd, UI_DEV_CREATE)
        if identity.store:
            notify(f"FDSTORE=1\nFDNAME={identity.store}", [self.fd])
        log(f"created {self.name}")

    def close(self):
        """The pad goes away (one that isn't kept)."""
        os.close(self.fd)
        self.log(f"removed {self.name}")

    def send(self, state):
        if state == self.state:
            return
        old, self.state = self.state, state
        out = bytearray()
        for bit, code in KEYS.items():
            if (state[0] ^ old[0]) & bit:
                out += EVENT.pack(0, 0, EV_KEY, code, 1 if state[0] & bit else 0)
        hat = (((state[0] & PAD_RIGHT) > 0) - ((state[0] & PAD_LEFT) > 0),
               ((state[0] & PAD_DOWN) > 0) - ((state[0] & PAD_UP) > 0))
        old_hat = (((old[0] & PAD_RIGHT) > 0) - ((old[0] & PAD_LEFT) > 0),
                   ((old[0] & PAD_DOWN) > 0) - ((old[0] & PAD_UP) > 0))
        # The helper's sticks are up-positive, evdev's down-positive.
        for code, new, was in ((ABS_Z, state[1], old[1]), (ABS_RZ, state[2], old[2]), (ABS_X, state[3], old[3]),
                               (ABS_Y, -state[4], -old[4]), (ABS_RX, state[5], old[5]), (ABS_RY, -state[6], -old[6]),
                               (ABS_HAT0X, hat[0], old_hat[0]), (ABS_HAT0Y, hat[1], old_hat[1])):
            if new != was:
                out += EVENT.pack(0, 0, EV_ABS, code, max(-32768, min(32767, new)))
        if out:
            out += EVENT.pack(0, 0, EV_SYN, SYN_REPORT, 0)
            try:
                os.write(self.fd, out)
            except BlockingIOError:
                pass  # nobody reads it fast enough: the next change says it all again


class GamepadMode:
    """Gamepad mode's state, as ft-screens says it. send(to, message): to "screens".
    make_pad(identity): a VirtualPad (made as a panel on this machine gets the pad). read(on): the
    controller reader follows the pad or stops. sound: play the cues."""

    def __init__(self, log, send, make_pad, read, sound=True):
        self.log, self.send, self.make_pad, self.read, self.sound_on = log, send, make_pad, read, sound
        self.on = False
        self.target = None  # "desktop", "game" or "remote" while on
        self.until = 0.0  # ft-screens' word lapses then
        self.state = NEUTRAL
        self.seen = 0.0  # the reader's last state came then
        self.pads = {}  # the VirtualPad of each LOCAL target that has one
        self.dash = False  # the helper says the dashboard is open
        self.dash_opened = False  # ...and it opened during this gamepad mode: the controllers are its
        self.fresh = False  # the next state's buttons are held from before: they wait for release
        self.held = 0
        self.refused = None  # why the last "gamepad" from ft-screens was turned down (logged once)

    def sound(self, which):
        path = SOUNDS[which]
        if self.sound_on and os.path.exists(path):
            try:
                subprocess.Popen(["pw-play", path], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL, start_new_session=True)
            except OSError:
                pass

    def screens_word(self, target, now, blocked=None):
        """ft-screens' "gamepad desktop|game|remote|off" (target None: off). blocked: why it can't
        be on now (a VR game, a pause), or None."""
        if target is None:
            self.refused = None
            self.stop("SteamVR's Gamepad mode ended")
            return
        if blocked:
            if self.refused != blocked:
                self.log(f"gamepad mode: not on ({blocked})")
                if not self.on:
                    self.sound(None)
            self.refused = blocked
            self.stop(blocked)
            return
        self.refused = None
        self.until = now + LEASE
        if not self.on:
            self.on, self.target, self.state, self.seen, self.fresh = True, target, NEUTRAL, now, True
            self.dash_opened = False
            self.log(f"gamepad mode on: the controllers are a gamepad for {self.where()}")
            self.read(True)
            self.sound(True)
        elif target != self.target:
            self.let_go()
            self.drop()
            self.target, self.fresh = target, True
            self.log(f"gamepad mode: now for {self.where()}")
        else:
            return
        # A game needs a moment to find a new pad: its panel's is there before the first press.
        self.local_pad()

    def stop(self, why):
        if not self.on:
            return
        self.let_go()
        self.drop()
        self.on, self.target = False, None
        self.read(False)
        self.log(f"gamepad mode off ({why})")
        self.sound(False)

    def dashboard_state(self, on):
        """The helper's "vrdash 1|0": a dashboard opened during gamepad mode has the controllers
        until it closes."""
        if on == self.dash:
            return
        self.dash = on
        if on and self.on:
            self.dash_opened = True
            if self.state != NEUTRAL:
                self.let_go()
        if not on:
            self.dash_opened = False
            self.fresh = True

    def where(self):
        return {"remote": "a remote display", "game": "a gamescope app's panel"}.get(self.target, "a Frametop desktop panel")

    def local_pad(self):
        """The target's pad on this machine, made now if it has none yet. None for a remote
        display, and when it can't be made: gamepad mode is off then."""
        identity = LOCAL.get(self.target)
        if identity is None:
            return None
        if self.target not in self.pads:
            try:
                self.pads[self.target] = self.make_pad(identity)
            except OSError as e:
                self.log(f"gamepad mode: can't make the virtual gamepad: {e}")
                self.stop("no virtual gamepad")
                return None
        return self.pads[self.target]

    def drop(self):
        """The target's pad goes away, unless it's one that is kept (Identity.store)."""
        identity = LOCAL.get(self.target)
        if identity is not None and not identity.store and self.target in self.pads:
            try:
                self.pads.pop(self.target).close()
            except OSError as e:
                self.log(f"gamepad mode: removing the virtual gamepad: {e}")

    def let_go(self):
        """Everything comes up where the pad was going."""
        self.state = NEUTRAL
        if self.target == "remote":
            self.send("screens", "pad off")
        elif self.target in self.pads:
            self.pads[self.target].send(NEUTRAL)

    def reader_state(self, state, now):
        """A "vrpad" line from the controller reader."""
        if not self.on:
            return
        self.seen = now
        if self.dash_opened:
            return
        buttons = state[0]
        if self.fresh:
            self.held, self.fresh = buttons, False
        self.held &= buttons
        self.deliver((buttons & ~self.held, *state[1:]))

    def deliver(self, state):
        self.state = state
        if self.target == "remote":
            self.send("screens", screens_line(state))
        else:
            pad = self.local_pad()
            if pad:
                pad.send(state)

    def tick(self, now):
        if self.on and now >= self.until:
            self.stop(f"no word from ft-screens for {LEASE:.0f} s")
        if self.on and self.state != NEUTRAL and now - self.seen > STALE:
            self.log("gamepad mode: no word from the controller reader, letting go")
            self.let_go()

    def timeout(self, now):
        """How long the relay may wait before tick() has something to do."""
        if not self.on:
            return 3600.0
        t = self.until - now
        if self.state != NEUTRAL:
            t = min(t, self.seen + STALE - now)
        return max(0.05, t)
