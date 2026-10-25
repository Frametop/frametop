"""Gamepad mode for Frametop panels: the Frame controllers as one Xbox pad for a panel (the input
relay runs it; input-relay.py's "Gamepad mode").

SteamVR's own Gamepad mode decides when it's on, as for a game: with the Steam menu open,
ft-screens shows an "Enter Gamepad Mode" pill above the panel typing goes to. Its click has
ft-dashpatch push SteamVR's input focus onto ft-screens' hidden dashboard overlay frametop.gamepad
and close the Steam menu, so SteamVR's controllers go to Gamepad mode for it; the grips bring the
laser back, as anywhere in SteamVR. ft-dashpatch reads which it is in the dashboard's page, and
ft-screens tells the relay "gamepad desktop|remote" each second while Gamepad mode is Frametop's
(for the panel clicked), "gamepad off" as it ends. SteamVR gives an overlay no input in Gamepad
mode (2026-10-25: no actions, no button events), so the relay's controller reader
(game_pause.ControllerWatch, vrserver's web socket) reads every controller button, both sticks and
both triggers raw, and says the pad's state ("vrpad B LT RT LX LY RX RY", XInput's buttons and
up-positive sticks) on each change and every 250 ms. The relay sends it on:
  - a remote display: to ft-screens ("pad ..."), which hands it to that display's ft-stream, which
    gives the host a Moonlight Xbox controller (stream/ft-stream.cpp)
  - a Frametop desktop panel: VirtualPad, an Xbox 360 pad on uinput, which its apps read.
It ends too when Frametop pauses, a VR game starts, or ft-screens goes quiet for LEASE. With the
dashboard open (the helper's "vrdash 1") the pad lets go, and buttons held as it comes on or the
dashboard closes (the pill's click) wait for their release. A pad with no word from the reader for
STALE lets go of everything, and one that changes targets lets go on the old one.
"""

import fcntl
import os
import struct
import subprocess

STALE = 1.0  # no "vrpad" line from the controller reader this long: everything comes up
LEASE = 3.0  # no "gamepad desktop|remote" from ft-screens this long: gamepad mode ends
NEUTRAL = (0, 0, 0, 0, 0, 0, 0)  # buttons, LT, RT, LX, LY, RX, RY
SOUNDS = {True: "/usr/share/sounds/ocean/stereo/message-contact-in.oga",
          False: "/usr/share/sounds/ocean/stereo/message-contact-out.oga",
          None: "/usr/share/sounds/ocean/stereo/completion-fail.oga"}  # couldn't start

# XInput's button bits (the helper's kPad*, Moonlight's *_FLAG).
PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT = 0x0001, 0x0002, 0x0004, 0x0008
PAD_START, PAD_BACK, PAD_LS, PAD_RS = 0x0010, 0x0020, 0x0040, 0x0080
PAD_LB, PAD_RB, PAD_A, PAD_B, PAD_X, PAD_Y = 0x0100, 0x0200, 0x1000, 0x2000, 0x4000, 0x8000
PAD_ALL = 0xF3FF

# Linux input constants (input-event-codes.h, uinput.h).
EV_SYN, EV_KEY, EV_ABS = 0x00, 0x01, 0x03
SYN_REPORT = 0
BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL, BTN_TR = 0x130, 0x131, 0x133, 0x134, 0x136, 0x137
BTN_SELECT, BTN_START, BTN_THUMBL, BTN_THUMBR = 0x13A, 0x13B, 0x13D, 0x13E
ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ, ABS_HAT0X, ABS_HAT0Y = 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x10, 0x11
BUS_USB = 0x03
# What xpad makes of an Xbox 360 pad: buttons, the d-pad as a hat, sticks down-positive.
KEYS = {PAD_A: BTN_A, PAD_B: BTN_B, PAD_X: BTN_X, PAD_Y: BTN_Y, PAD_LB: BTN_TL, PAD_RB: BTN_TR,
        PAD_BACK: BTN_SELECT, PAD_START: BTN_START, PAD_LS: BTN_THUMBL, PAD_RS: BTN_THUMBR}
AXES = ((ABS_X, -32768, 32767, 16, 128), (ABS_Y, -32768, 32767, 16, 128), (ABS_RX, -32768, 32767, 16, 128),
        (ABS_RY, -32768, 32767, 16, 128), (ABS_Z, 0, 255, 0, 0), (ABS_RZ, 0, 255, 0, 0),
        (ABS_HAT0X, -1, 1, 0, 0), (ABS_HAT0Y, -1, 1, 0, 0))
EVENT = struct.Struct("llHHi")


def _iow(nr, size):
    return (1 << 30) | (size << 16) | (ord("U") << 8) | nr


UI_DEV_CREATE = 0x5501
UI_DEV_SETUP = _iow(3, 92)
UI_ABS_SETUP = _iow(4, 28)  # struct uinput_abs_setup: u16 code, struct input_absinfo (6 x s32)
UI_SET_EVBIT = _iow(100, 4)
UI_SET_KEYBIT = _iow(101, 4)
UI_SET_ABSBIT = _iow(103, 4)
ABS_SETUP = struct.Struct("H2x6i")
STORE_NAME = "vpad"  # in systemd's fd store, so a relay restart keeps the same pad


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
    """An Xbox 360 pad on uinput (045e:028e, as xpad names it), made on first use."""

    def __init__(self, log, stored, notify):
        self.state = NEUTRAL
        if STORE_NAME in stored:
            self.fd = stored[STORE_NAME]
            os.set_blocking(self.fd, False)
            log("reusing the gamepad from the fd store")
            return
        self.fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
        fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_KEY)
        for code in KEYS.values():
            fcntl.ioctl(self.fd, UI_SET_KEYBIT, code)
        fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_ABS)
        for code, low, high, fuzz, flat in AXES:
            fcntl.ioctl(self.fd, UI_SET_ABSBIT, code)
            fcntl.ioctl(self.fd, UI_ABS_SETUP, ABS_SETUP.pack(code, 0, low, high, fuzz, flat, 0))
        name = b"frametop virtual gamepad"
        fcntl.ioctl(self.fd, UI_DEV_SETUP, struct.pack("HHHH80sI", BUS_USB, 0x045E, 0x028E, 0x0114, name, 0))
        fcntl.ioctl(self.fd, UI_DEV_CREATE)
        notify(f"FDSTORE=1\nFDNAME={STORE_NAME}", [self.fd])
        log("created frametop virtual gamepad")

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
    """Gamepad mode's state, as ft-screens says it. send(to, message): to "screens". make_pad():
    the VirtualPad (made when a desktop panel first gets the pad). read(on): the controller reader
    follows the pad or stops. sound: play the cues."""

    def __init__(self, log, send, make_pad, read, sound=True):
        self.log, self.send, self.make_pad, self.read, self.sound_on = log, send, make_pad, read, sound
        self.on = False
        self.target = None  # "desktop" or "remote" while on
        self.until = 0.0  # ft-screens' word lapses then
        self.state = NEUTRAL
        self.seen = 0.0  # the reader's last state came then
        self.pad = None
        self.dash = False  # the dashboard is open: the controllers are its
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
        """ft-screens' "gamepad desktop|remote|off" (target None: off). blocked: why it can't be
        on now (a VR game, a pause), or None."""
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
            self.log(f"gamepad mode on: the controllers are a gamepad for {self.where()}")
            self.read(True)
            self.sound(True)
        elif target != self.target:
            self.let_go()
            self.target, self.fresh = target, True
            self.log(f"gamepad mode: now for {self.where()}")

    def stop(self, why):
        if not self.on:
            return
        self.let_go()
        self.on, self.target = False, None
        self.read(False)
        self.log(f"gamepad mode off ({why})")
        self.sound(False)

    def dashboard_state(self, on):
        """The helper's "vrdash 1|0": while the dashboard is open it has the controllers."""
        if on == self.dash:
            return
        self.dash = on
        if on and self.on and self.state != NEUTRAL:
            self.let_go()
        if not on:
            self.fresh = True

    def where(self):
        return "a remote display" if self.target == "remote" else "a Frametop desktop panel"

    def let_go(self):
        """Everything comes up where the pad was going."""
        self.state = NEUTRAL
        if self.target == "remote":
            self.send("screens", "pad off")
        elif self.target == "desktop" and self.pad:
            self.pad.send(NEUTRAL)

    def reader_state(self, state, now):
        """A "vrpad" line from the controller reader."""
        if not self.on:
            return
        self.seen = now
        if self.dash:
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
        elif self.target == "desktop":
            if self.pad is None:
                try:
                    self.pad = self.make_pad()
                except OSError as e:
                    self.log(f"gamepad mode: can't make the virtual gamepad: {e}")
                    self.stop("no virtual gamepad")
                    return
            self.pad.send(state)

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
