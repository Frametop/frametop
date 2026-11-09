#!/usr/bin/env python3
"""Input relay for the Steam Frame: stable virtual devices, the 3D pointer, device rules.

SteamVR opens /dev/input/event* only when it starts and never hotplugs, so a
Bluetooth mouse that sleeps and reconnects (new event nodes) stops working
until SteamVR restarts. This relay creates a virtual mouse and a virtual
keyboard through /dev/uinput once, before SteamVR starts, and feeds them from
the physical devices as they come and go.

Every USB or Bluetooth mouse and keyboard is a candidate. A physical device is
identified by its Bluetooth address (EVIOCGUNIQ) or USB bus:vendor:product:name,
so all its event nodes share one role (the Swiftpoint Z3 has a mouse node and a
keyboard node for its extra buttons), except with "combo". Roles, from
~/.config/frametop-input.json (written by the Frametop Settings app):
  pointer      grabbed; drives the universal 3D mouse (default for devices with a mouse node)
  passthrough  keys go to the desktop; grabbed only while typing goes there, otherwise only observed,
               for the key combinations below (default for keyboards)
  combo        a keyboard with a touchpad (its touchpad is a mouse node): mouse nodes are pointer,
               keyboard nodes passthrough, as if they were two devices. Keys that come on a mouse node
               (a keyboard with a mouse-key layer on one node) still make key combinations, unless the
               device's button map has that key. Never a default: the
               Z3's keyboard node has to stay with its mouse, and nothing on the nodes tells the two apart
  ignore       not grabbed, only observed for identification in the settings app
Buttons and keys of pointer devices go through a per-device map to actions
(left, right, middle, back, scroll_up, scroll_down, dashboard, recenter,
pointer_toggle, follow_toggle = head follow on or off, gaze_toggle = gaze mode on or off
(the pointer goes where you look; see pointer/helper/ft-pointer.cpp), gaze_precision = while
held, the pointer stops where you look and the mouse steers it, and the release clicks there,
gaze_drag = the same, but pressed at once, so it drags ("precision|gazedrag mouse|keyboard 1|0"
to the helper), gaze_left and gaze_right = keyboard clicks at the gaze: a tap clicks where you
look; held, the pointer stops there and your head steers it (it stays put in your view), and
the release clicks; held still for half a second, it's a real press that your head drags
("gazekey left|right 1|0" to the helper; by default Meta+J and Meta+K, DEFAULT_KEY_BINDINGS),
gaze_quickcal = the gaze service's one-dot check ("quickcal" to @ft_gazed), sens_up, sens_down,
layout_reset = open the profile in use again, or put the desktop screens back in their layout (ft-layout reset), screens_toggle = hide or show the desktop screens
("toggle" to ft-screens; Meta+Shift+H by default),
keyboard_toggle = open or close Frametop's keyboard, float_toggle = float the desktop window under the
pointer (else the active one) in VR, or put it back if it floats, dock_all = put every floating
window back (both to ft-floatd, @frametop_float), spin_next and spin_prev = turn every panel in the
room about your head so the next one to the right or left comes to the front (ft-screens' "spin",
Meta+Alt+Tab and Meta+Alt+Shift+Tab by default), profile:NAME = switch to that profile (ft-layout
use NAME: its screens and apps; docs/profiles.md), steam_menu = open the SteamVR dashboard on
Steam's menu, or close the dashboard (steam/ft-steam menu, through Steam's UI), pause_toggle =
pause Frametop for a VR game, or resume it (game_pause.py), dictate = start or stop dictation, and
dictate_hold = dictation while held (Frametop Voice, voice/ft-voiced: "toggle|start|stop <source>"
to @frametop_voice), frunner = open or close frunner's search for apps and remote displays
("toggle <source>" to @ft_frunner; Meta+Space by default), os_menu = open frunner with Frametop's
own actions listed, its OS menu ("toggle <source> actions"; no default), hands_toggle = stop hand tracking if
it runs, else start it, until the next desktop or SteamVR start (hands/handsmode.py toggle; Meta+H
by default), release_keys = let go of every key held anywhere: what the desktop, a remote
display and this relay hold, and every modifier in KWin, known to be held or not ("keys
release" to ft-screens; Ctrl+Alt+Shift+Esc by default), desktopless_toggle = desktopless mode on or off
(ft-layout desktopless toggle, docs/desktopless.md; Meta+Shift+D by default), run:NAME = run your command NAME in the background (commands/ft_commands.py: a
transient systemd user unit, output to the journal; docs/commands.md), command:CMD = run CMD with
sh -c (on the host, as this service: its environment, output to its log, and COMMAND_PATH, so
ft-layout, ft-float, ft-steam and ft-voice need no path), keys:CODES = tap that key combination
where typing goes, all its keys down in order, then up, on the press ("keys:125+32" is Meta+D:
evdev codes as in key_bindings, at most KEYS_MAX; a desktop shortcut from a controller button),
key = pass through as a key, none).

Frame controller buttons can be mapped too ("controller_buttons": {"right/a": action} in the
rules file; any action but key and the gaze ones, GAZE_ACTIONS: gaze mode is a mouse feature,
docs/gaze-controllers.md). So can key combinations on any keyboard ("key_bindings":
{"29+56+34": action}, evdev codes joined by "+", modifiers first and left-hand codes for
either side, here Ctrl+Alt+G): the combination does the action, and its last key isn't typed.
A modifier on its own ("125": Meta) is a tap: pressed with no other modifier held, and released
with no other key or mouse button going down or up, and no scroll, in between. It fires on the
release, never on the press. The desktop gets an F24 press before the release, so Plasma's
launcher doesn't open on a Meta tap that's bound. A key or button already down when the
modifier goes down keeps it from being one too (a drag sends nothing meanwhile), except for
hold to talk. "meta_tap": false in the rules turns a Meta tap's binding off, whatever it is
(either side's; the default is on): Meta comes and goes as an unbound one does. A tap's left-hand code is
either side's; a right-hand code ("97": Right Ctrl) is that key's alone, and comes first. A lone
modifier bound to dictate_hold is hold to talk instead: dictation starts on its press (alone) and
stops on its release, and a key, mouse button or scroll in between makes it a shortcut, which
cancels that dictation quietly ("cancel keyboard quiet"). A rules
file without "key_bindings" gets DEFAULT_KEY_BINDINGS (Meta tap: steam_menu, Meta+J: gaze_left,
Meta+K: gaze_right, Meta+Shift+F: float_toggle, Meta+Shift+H: screens_toggle, Ctrl+Alt+Shift+Esc:
release_keys), plus a Right Ctrl tap for dictate once Frametop Voice is installed
(VOICE_KEY_BINDINGS); one with its own, even an empty one, doesn't. A combination bound to
release_keys also fires with more modifiers held than it names, so one stuck here doesn't stop it.
The float, screens, profile, Steam menu, pause, dictation, hand tracking, release and command
actions work without pointer mode too. A combination with Meta also sends the desktop Meta's
release right away, after an F24 press when Meta is the only key it holds: so letting go of Meta
doesn't open Plasma's launcher (another held key, the Shift of Meta+Shift+H, already stops that,
and F24 would reach the app), and a gaze click isn't Meta+click (KWin's window move and resize).
Another key while Meta is still held gives the desktop Meta back, unless it's a combination
too (Meta+Shift+H twice: nothing more is sent). While typing goes to Steam, keyboards aren't grabbed, so Steam
or the game sees a combination's keys too. The controllers aren't input devices here, only SteamVR sees
them, so the pointer helper reads them with SteamVR input and sends "vrbtn <button> 1|0".
It only takes the buttons the relay tells it to ("vrbind <button>..." to @ft_pointer_helper,
sent on start, reload, and when the helper says "vrhello"), and only while no game runs,
unless "controller_in_games" is true in the rules file (then a mapped button no longer
reaches games; see pointer/helper/vrbuttons.h).

Gamepad mode (input/vrpad.py) makes the Frame controllers one Xbox pad for a Frametop panel, through
SteamVR's own Gamepad mode: ft-screens' "Enter Gamepad Mode" pill above the panel typing goes to
(with the Steam menu open) has ft-dashpatch give SteamVR's input focus to ft-screens' hidden
dashboard overlay, and ft-screens says "gamepad desktop|remote" here every second while SteamVR's
Gamepad mode is Frametop's, "gamepad off" as it ends (the grips, as anywhere in SteamVR). The
controller reader (game_pause.py, vrserver's web socket) then reads every controller button, both
sticks and both triggers raw, and says the pad's state here ("vrpad B LT RT LX LY RX RY"). A
remote display gets it through ft-screens ("pad ..."), a desktop panel as a virtual Xbox 360 pad
on uinput. It ends when Frametop pauses or a VR game starts, and with the dashboard open the pad
lets go.

In gaze mode outside games, the helper keeps the pointer ("gazeawake 1", repeated every 5
seconds; "gazeawake 0" or silence ends it): the pointer isn't released when the mouse is idle.
It says the same during ft-screens' carry-all, in games too (the head carries the panels then).
When something else let the pointer go, the gaze takes it back and the helper says "gazewoke"
(the pointer is on again here too). The hides here say why: "hide toggle" (pointer_toggle: the
gaze leaves it off until something wakes it), "hide idle", "hide pause".
Typing on a keyboard sends the helper "typing" (at most 4 times a second): it takes no hand
pinches right after a key, since typing touches thumb to index like a pinch.

Keys also go to ft-screens (@ft_screens, the Frametop desktop's compositor), which
types them into the desktop screen that has focus: from pass-through keyboards, a USB or
Bluetooth keyboard's media keys (its Consumer Control node, which has volume keys, so it's
never grabbed and gamescope has them too), and keys a pointer device passes through. In
pointer mode a mouse button passed through as a key (BTN_MOUSE..BTN_TASK, a side button for
Back) goes there too, and ft-screens gives it the screen the pointer is on, not the one
typing goes to (it releases the button itself when the pointer leaves the screens, they
hide, or Frametop pauses). Other buttons and keys from KEY_OK up don't go there.
Typing goes to the panel clicked last, and ft-screens says which ("keyboard
desktop|steam SHOWN PAUSED WHEEL" on the control socket, every second; SHOWN 1|0, whether the
screens show; PAUSED 1|0, its pause; WHEEL 1|0, whether the 3D mouse's laser is on a panel, so
its wheel goes to ft-screens as "wheel N" and not through SteamVR's joystick). While it's the desktop,
pass-through keyboards are grabbed, so gamescope, which reads every keyboard itself,
doesn't type them into its focused app too. Without word from ft-screens for 3 seconds
they're released.

Dictation (Frametop Voice) types here: its text comes as "type TEXT" on the control socket, only
from this user, and goes out key by key, TYPE_STEP apart, where typing goes: to ft-screens for the
desktop, else on the virtual keyboard, which SteamVR passes on to Steam and games. On the
desktop each character is the keys that type it on the desktop's keyboard layout (Plasma's
kxkbrc in Frametop's desktop config, worked out with libxkbcommon), Shift and AltGr included, so a
German QWERTZ desktop gets "z" from the Z key; Steam and games get a US layout. A character the
desktop's layout has no key for (ą on a US layout, a dead key's accent, another script) goes as
"commit CHARACTER" to the desktop's input method (input/ft-textinput), which commits it to the
focused text field, while one is focused: that's the address its "textfield 1" came from (this
user's only), until "textfield 0", or TEXT_INPUT_LAPSE after its last "textfield 1 renew" (sent
every few seconds while the field stays focused, so a restarted relay finds it again and a crashed
ft-textinput's address lapses). TYPE_SWITCH apart from keys either side, since the two ways
reach the app separately. Without a text field taking text that way (Xwayland and most Electron
apps), or for Steam, such a character is dropped, and the log says which.
A character waits while a modifier is held on a keyboard, which would make it a shortcut
(Ctrl+letter).

Frametop's keyboard (Full Keyboard, below): the desktop's input method (input/ft-textinput)
says "textfield 1|0" when a text field on the desktop gains or loses keyboard focus, and
the relay tells ft-screens to open ("vrkeyboard show") or close ("vrkeyboard hide") the
keyboard, depending on "vr_keyboard" in the rules file: "always", "no_keyboard" (the
default: only while no pass-through keyboard is connected; a program's uinput keyboard
doesn't count), "button" (only the keyboard_toggle action opens it), or "never"
(keyboard_toggle does nothing either). With "vr_keyboard_persist" (the default), it stays
open when the text field loses focus, until its close button, keyboard_toggle, or a layout reset
(ft-layout reset) closes it.

Full Keyboard (keyboard/, TaiKeid's, run with --frametop; keyboard/FRAMETOP.md) is that keyboard.
While it isn't running, nothing opens (ft-screens' old panel was removed in the 2026-10-31 cleanup). It says "kbhello OPEN" on the control socket every second, only taken from
this user, and gets "kb DESKTOP SURFACE" back: whether typing goes to the desktop, and whether
the screens show with Frametop not paused (it may show over them with the dashboard closed).
The relay tells ft-screens "fullkb OPEN", and while that's fresh ft-screens passes its
"vrkeyboard" commands on as "kbpanel show|hide", which the relay sends Full Keyboard as "kb
show|hide". A fingertip on its panel comes from ft-screens as "kbpoke SIDE VERB [x y z]" and goes
to it as "kb poke SIDE VERB [x y z]", so its keys take hands as our screens do, with no laser.
Its typing for the desktop comes as "kbkey CODE 0|1" and "kbtext TEXT", only from
the address that said hello: they go in order with dictation's typing, keys as they are (a held
modifier doesn't hold them back), text through the desktop's layout like dictation's, and
unlogged. Its keys come up where they went down, and when it stops (no hello for 3 seconds),
the ones it holds come up.

Volume keys, from every device that has them (the headset's own buttons included),
are handled here: wpctl steps the default output. Nothing else may see a volume key,
because gamescope aborts on one when no window has keyboard focus, which ends the
whole VR session. Devices with a keymap (the headset's gpio-keys, USB and Bluetooth
keyboards) get their volume entries remapped to unused stand-in codes, so their
other keys keep working for SteamVR; a device without a keymap that has only volume
keys (the headset's pmic_resin) is grabbed. The keymaps go back when the relay exits.

Hand interaction (POINTER_HANDS=1, or HANDS_NEAR=on for ft-screens' direct touch alone): the
pointer helper decides per side whether a hand holds a controller or is bare
(pointer/helper/handoff.h). The relay's web socket reader (game_pause.py, TouchTracker) sends it
"ctl <side> touch 0|1", "ctl <side> press", "ctl <side> gone" and "ctl ws 0|1", and the pause
state as "paused 1|0" (hands work in a VR game only with the dashboard open or Frametop toggled
back on).

The gaze calibration panel (gaze/panel/ft-gazepanel; the pointer helper says "calpanel 1" while
it's up): Enter on a pass-through keyboard (either Enter, with no modifier held) takes the
panel's dot instead of typing: "calkey 1" to the helper, which answers the panel as it does a
left click. Its repeats and its release go nowhere either. That works while the relay has the
keyboard (typing goes to the desktop); while typing goes to Steam, Steam sees the Enter too.

Frametop can pause for VR games (game_pause.py: by hand, with a controller gesture, or by itself
while a game runs). Paused, it stops the services that cost the game CPU and GPU, and the relay
plays a plain one: the pointer devices feed the virtual mouse and keyboard as without pointer
mode, typing goes to Steam, and mapped buttons and key combinations do only pause_toggle,
steam_menu, release_keys, run:NAME and command:CMD (game_pause.PAUSED_ACTIONS).

Host devices (docs/host-input.md): a computer whose displays stream into Frametop can lend
it its own keyboard and mouse. Their events come from that host's ft-stream on the control
socket, only from this user: "hostdev ID KIND NAME" (hello, every second; KIND mouse or
keyboard), "hostev ID KIND T C V..." (evdev type, code and value triples; a report ends with
0 0 0) and "hostbye ID KIND". Each host and kind is a node of its own (id "host:ID:KIND", bus
"network"), fed through a pipe so the loop below reads it like an event node, with the same
roles, mapped buttons and key bindings. Nothing else sees its events, so there's no grab: while
typing goes to Steam, a pass-through host keyboard's keys go out on the virtual keyboard. Its
held keys are counted here (no EVIOCGKEY), and with no word for HOST_TIMEOUT it goes away like
an unplugged device, its keys coming up.

Pointer mode (POINTER=1 in ~/.config/frametop.conf) sends pointer devices to
the ft-pointer helper (pointer/helper), which drives the ft_pointer
SteamVR driver. With POINTER=0, pointer devices go to the virtual mouse and
keyboard instead.

Control socket (abstract datagram @frametop_relay, JSON replies to the sender):
  devices           list event nodes with id, name, kinds, role, grabbed
  watch <seconds>   stream input events from every candidate node (identification)
  reload            re-read both config files, re-apply roles, tell the helper
  vrcapture <s>     take every controller button for s seconds (0: stop), so the settings
                    app can capture one; watchers see them as events with id frame_controller
  action NAME       from frunner: run one of Frametop's own actions (ft_input_actions.MENU_ACTIONS
                    only; from this user only), as a button would ({"t": "action"}, or an error)
  vrbtn, vrhello, gazeawake, gazewoke   from the pointer helper (above)
  vrgame 1|0        from the pointer helper: a VR game runs (on a change and every 5 s), for pausing
  vrlink 1|0        from the pointer helper: Steam Link's client is the scene app (on a change and
                    every 5 s), so pc_game.py asks the PC whether it runs a game
  vrdash 1|0        from the pointer helper: the dashboard is open (on a change and every 5 s), so
                    hand tracking parked for a game wakes (game_pause.py, PARK_PATH), and gamepad mode's pad lets go
  gamepad desktop|remote|off   from ft-screens: SteamVR's Gamepad mode is Frametop's, for a desktop
                    panel or a remote display (every second while it is), or it ended (above);
                    ignored from another user
  vrpad B LT RT LX LY RX RY   from the controller reader in gamepad mode: the pad's state (above);
                    ignored from another user
  calpanel 1|0      from the pointer helper: a gaze calibration panel is up (renewed every second,
                    lapses 3 s after the last), so Enter answers it (above)
  textfield 1|0     from the desktop's input method (above); "commit TEXT" goes back to it.
                    "textfield 1 renew" (every few seconds while focused) keeps its address, and
                    doesn't open the keyboard again
  pause on|off|toggle [reason]   pause Frametop or resume it (input/ft-pause; the gesture reader)
  pause ?           the pause state, as {"t": "pause", ...}
  type TEXT         from Frametop Voice: type TEXT where typing goes (above); ignored from
                    another user
  hostdev, hostev, hostbye   a host's keyboard and mouse, from ft-stream (above); ignored from
                    another user

Runs on the Frame host as a user service (frametop-input-relay.service). The
virtual devices are parked in systemd's file descriptor store, so a relay
restart gets the same devices back and SteamVR never loses them. The service is
Type=notify: READY=1 goes out only after the devices exist, so SteamVR (ordered
after it) always finds them. Dependency-free: Python standard library plus the
kernel's evdev and uinput interfaces.

  input-relay.py            the service
  input-relay.py --no-grab  never grab, for testing next to a running SteamVR
"""
import array
import atexit
import collections
import configparser
import ctypes
import ctypes.util
import errno
import fcntl
import json
import os
import re
import select
import signal
import socket
import struct
import subprocess
import sys
import time

import game_pause
import pc_game
from ft_evdev import (  # the Linux input ABI (input.h, uinput.h)
    EV_KEY,
    EV_REL,
    EV_SYN,
    EVENT,
    EVIOCGID,
    EVIOCGKEY,
    EVIOCGKEYCODE_V2,
    EVIOCGRAB,
    EVIOCSKEYCODE_V2,
    INPUT_KEYMAP_BY_INDEX,
    KEY_MAX,
    KEYMAP_ENTRY,
    SYN_REPORT,
    UI_DEV_CREATE,
    UI_DEV_SETUP,
    UI_SET_EVBIT,
    UI_SET_KEYBIT,
    UI_SET_RELBIT,
    eviocgbit,
    eviocgname,
    eviocguniq,
)
import vrpad

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "commands"))
import ft_commands  # noqa: E402  (your commands, "run:NAME")
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "conf"))
import ft_conf  # noqa: E402  (frametop.conf's grammar, docs/config.md)
import ft_pinlock  # noqa: E402
from ft_input_actions import (  # noqa: E402  (the rules file's shared vocabulary)
    DEFAULT_KEY_BINDINGS,
    GAZE_ACTIONS,
    MENU_ACTION_NAMES,
    VOICE_ACTIONS,
    VOICE_KEY_BINDINGS,
    VR_BUTTONS,
    renamed_actions,
)

# Linux input constants (include/uapi/linux/input-event-codes.h, input.h, uinput.h). The event
# record and the ioctl numbers are ft_evdev's (shared with vrpad.py); the codes are here.
BTN_MISC = 0x100
KEY_A = 30
REL_X, REL_Y, REL_WHEEL, REL_MAX = 0x00, 0x01, 0x08, 0x0F
SCROLLS = {0x06, REL_WHEEL, 0x0B, 0x0C}  # REL_HWHEEL, REL_WHEEL and their _HI_RES
BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA = 0x110, 0x111, 0x112, 0x113, 0x114
BTN_MOUSE, BTN_TASK = 0x110, 0x117  # mouse buttons, the first and the last
KEY_LEFTMETA, KEY_RIGHTMETA = 125, 126
KEY_ENTER, KEY_KPENTER = 28, 96
KEY_MUTE, KEY_VOLUMEDOWN, KEY_VOLUMEUP = 113, 114, 115
# Volume keys are remapped to KEY_MACRO28, KEY_MACRO29 and KEY_MACRO30: above 255, so X11
# can't carry them, and bound to nothing in the default keymap.
VOLUME_STANDIN = {KEY_MUTE: 0x2AB, KEY_VOLUMEUP: 0x2AC, KEY_VOLUMEDOWN: 0x2AD}
VOLUME_ORIGINAL = {v: k for k, v in VOLUME_STANDIN.items()}
VOLUME_CODES = set(VOLUME_STANDIN) | set(VOLUME_ORIGINAL)
BUS_USB, BUS_BLUETOOTH, BUS_VIRTUAL = 0x03, 0x05, 0x06
BUS_NETWORK = 0x1000  # ours, past the kernel's: a host's keyboard or mouse (see "Host devices")
HOST_TIMEOUT = 3.0  # a host device with no word for this long goes away
HOST_ID = re.compile(r"[A-Za-z0-9._-]{1,64}")

EV_NAMES = {EV_KEY: "key", EV_REL: "rel"}


VIRTUAL_PREFIX = "frametop virtual"
RULES_PATH = os.path.expanduser("~/.config/frametop-input.json")
ACTIONS = ("left", "right", "middle", "back", "scroll_up", "scroll_down", "dashboard", "recenter",
           "pointer_toggle", "follow_toggle", "gaze_toggle", "gaze_precision", "gaze_drag", "gaze_left", "gaze_right",
           "gaze_quickcal", "sens_up", "sens_down", "layout_reset", "screens_toggle", "keyboard_toggle", "float_toggle",
           "dock_all", "spin_next", "spin_prev", "carry_toggle", "steam_menu", "pause_toggle", "dictate", "dictate_hold",
           "frunner", "os_menu", "hands_toggle", "release_keys", "desktopless_toggle", "key", "none")
# GAZE_ACTIONS (mouse and keyboard only) and DEFAULT_KEY_BINDINGS, the key combinations a rules
# file without "key_bindings" gets, are ft_input_actions'.
# Frametop Voice (voice/): dictation starts and stops on its socket. With it installed (its unit
# file), a rules file without "key_bindings" gets these too: a Right Ctrl tap, Right Ctrl alone,
# so a Left Ctrl tap (crouch in many games) doesn't start it.
VOICE = "\0frametop_voice"
VOICE_UNIT = os.path.expanduser("~/.config/systemd/user/frametop-voice.service")
KEY_F24 = 194  # sent to the desktop with a Meta combination (see the top)
# Key combinations ("key_bindings"): modifiers, each side's code folded into the left one's.
MODIFIERS = {29: 29, 97: 29, 42: 42, 54: 42, 56: 56, 100: 56, 125: 125, 126: 125}
KEY_LEFTSHIFT = 42
KEY_RIGHTALT = 100  # AltGr: the third level of most layouts
# Dictation's text on a US layout (see the top), for Steam and games, and the desktop without a
# layout of its own: character -> (code, modifiers held while it's pressed).
US_KEYS = {" ": (57, ())}
for _row, _shifted, _first in (("1234567890-=", "!@#$%^&*()_+", 2), ("qwertyuiop[]", "QWERTYUIOP{}", 16),
                               ("asdfghjkl;'`", 'ASDFGHJKL:"~', 30), ("\\zxcvbnm,./", "|ZXCVBNM<>?", 43)):
    for _i, (_plain, _shift) in enumerate(zip(_row, _shifted)):
        US_KEYS[_plain], US_KEYS[_shift] = (_first + _i, ()), (_first + _i, (KEY_LEFTSHIFT,))
# The desktop's keyboard layout: Plasma's kxkbrc in the Frametop desktop's config folder (its
# XDG_CONFIG_HOME, session/frametop-session.sh), where its System Settings saves it.
KXKBRC = os.path.expanduser("~/.config/frametop/kxkbrc")
# The keys tried for each character on it, with these held (the first that types it wins): the
# main block and the key left of Z on ISO keyboards (KEY_102ND: "<" on a German one), not the
# keypad, which some apps read as keys of their own.
LAYOUT_CODES = (*range(2, 54), 57, 86)
LAYOUT_MODS = ((), (KEY_LEFTSHIFT,), (KEY_RIGHTALT,), (KEY_LEFTSHIFT, KEY_RIGHTALT))
TYPE_STEP = 0.004  # seconds from a typed key's press to its release, and on to the next key
# Between a character typed as keys and one committed through the input method (or back): they
# reach the app by different ways, so the first gets this long to arrive before the next goes.
TYPE_SWITCH = 0.05
TEXT_INPUT_LAPSE = 7.0  # ft-textinput renews its "textfield 1" every 2 s; its address lapses this long after
KB_POKES = ("hover", "press", "move", "up", "end")  # a fingertip on Full Keyboard (ft-screens' "kbpoke")
KB_COORD = re.compile(r"-?\d{1,3}(\.\d{1,6})?")
TYPE_MAX = 20000  # characters waiting to be typed, at most
VR_KEYBOARD_MODES = ("always", "no_keyboard", "button", "never")  # when Frametop's keyboard opens
HELPER = "\0ft_pointer_helper"
# VR_BUTTONS, the Frame controller buttons the pointer helper can read, is ft_input_actions'.
VR_DEVICE = "frame_controller"  # the id controller buttons have in watch events
SCREENS = "\0ft_screens"
GAZED = "\0ft_gazed"
FLOAT = "\0frametop_float"  # ft-floatd, floating windows in the Frametop desktop
# Actions for ft-floatd ("float_toggle", "dock_all"): they don't need pointer mode.
FLOAT_ACTIONS = {"float_toggle": b"float pointer", "dock_all": b"dock all"}
# Actions for ft-screens: spin_next and spin_prev turn every panel in the room about your head,
# so the next one to the right or left comes to the front, and screens_toggle hides or shows the
# screens. carry_toggle carries the panel under the 3D mouse (off any panel: the one nearest your gaze)
# with its ray until a click or Enter (Esc puts it back); another press meanwhile turns carry-all on or off (Meta+A is the
# Arteck HB305 touchpad's four-finger tap). They don't need pointer mode either.
SCREENS_ACTIONS = {"spin_next": b"spin next", "spin_prev": b"spin prev", "screens_toggle": b"toggle",
                   "carry_toggle": b"carry toggle"}
# frunner (frunner/ft-frunner, in the Frametop desktop) opens or closes its search; it's told
# what pressed it, since one from a mouse or controller opens Frametop's keyboard too. It doesn't
# need pointer mode, and does nothing while Frametop is paused.
FRUNNER = "\0ft_frunner"
# os_menu opens it with Frametop's own actions listed (ft_input_actions.MENU_ACTIONS, the OS
# menu): "toggle <source> actions". Picking one sends "action NAME" here.
# renamed_actions(): old action names rules files may still use, under their new ones (ft_input_actions).
# hands_toggle: hand tracking off if it runs, else on (HANDSMODE toggle, which says which in this
# service's log). It doesn't need pointer mode either.
PROFILE = "profile:"  # "profile:NAME": switch to that profile (doesn't need pointer mode either)
COMMAND = "command:"  # "command:CMD": run CMD (nor does this)
RUN = "run:"  # "run:NAME": run your command NAME (ft_commands; nor does this)
KEYS = "keys:"  # "keys:125+32": tap that key combination where typing goes (nor does this)
KEYS_MAX = 6  # keys in one "keys:" action at most
# Never in a "keys:" action: the volume keys (see the top) and power, sleep, wake and suspend,
# which logind may act on from the virtual keyboard. Input Settings' is_keys has the same list.
KEYS_REFUSED = {KEY_MUTE, KEY_VOLUMEDOWN, KEY_VOLUMEUP, 116, 142, 143, 205}


def keys_codes(a):
    """The key codes a "keys:" action presses, in order (evdev codes below BTN_MISC joined by "+",
    as in key_bindings), or None if it isn't one or has a key from KEYS_REFUSED."""
    if not isinstance(a, str) or not a.startswith(KEYS):
        return None
    parts = a[len(KEYS):].split("+")
    if not 0 < len(parts) <= KEYS_MAX or not all(p.isascii() and p.isdecimal() and 0 < int(p) < BTN_MISC
                                                 and int(p) not in KEYS_REFUSED for p in parts):
        return None
    return [int(p) for p in parts]


def known_action(a):
    return a in ACTIONS or keys_codes(a) is not None or (isinstance(a, str) and any(
        a.startswith(p) and a[len(p):].strip() for p in (PROFILE, COMMAND, RUN)))


def needs_pointer(a):
    return (a not in FLOAT_ACTIONS and a not in SCREENS_ACTIONS and a not in VOICE_ACTIONS
            and a not in ("steam_menu", "pause_toggle", "frunner", "os_menu", "hands_toggle", "release_keys",
                          "desktopless_toggle")
            and not a.startswith((PROFILE, COMMAND, RUN, KEYS)))


def works_paused(a):
    """An action that still does something while Frametop is paused (game_pause.py)."""
    return a in game_pause.PAUSED_ACTIONS or a.startswith((COMMAND, RUN))


# What pressed pause_toggle, for the log (do_action's source).
PAUSE_SOURCES = {"mouse": "mouse button", "keyboard": "key combination", "left": "controller button",
                 "right": "controller button", "frunner": "frunner"}
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FT_LAYOUT = os.path.join(REPO, "layout", "ft-layout")
FT_STEAM = os.path.join(REPO, "steam", "ft-steam")
HANDSMODE = os.path.join(REPO, "hands", "handsmode.py")
# Commands ("command:CMD") find Frametop's own tools (ft-layout, ft-float, ft-steam, ft-voice) on
# their PATH.
COMMAND_PATH = ":".join([os.path.join(REPO, d) for d in ("layout", "float", "steam", "voice")]
                        + [os.environ.get("PATH", "/usr/local/bin:/usr/bin")])
DEFAULT_BUTTONS = {BTN_LEFT: "left", BTN_RIGHT: "right", BTN_MIDDLE: "middle",
                   BTN_SIDE: "back", BTN_EXTRA: "back"}


def log(*args):
    print(*args, flush=True)


def notify(state, fds=()):
    """sd_notify, with optional file descriptors for the fd store. No-op outside systemd."""
    addr = os.environ.get("NOTIFY_SOCKET")
    if not addr:
        return
    if addr.startswith("@"):
        addr = "\0" + addr[1:]
    # socket.send_fds() ignores its address argument (Python 3.12), so use sendmsg.
    ancillary = [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array("i", fds))] if fds else []
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as sock:
            sock.sendmsg([state.encode()], ancillary, 0, addr)
    except OSError as e:
        log(f"sd_notify failed ({state.splitlines()[0]}): {e}")


def stored_fds():
    """File descriptors handed back by systemd's fd store, by name."""
    if os.environ.get("LISTEN_PID") != str(os.getpid()):
        return {}
    names = os.environ.get("LISTEN_FDNAMES", "").split(":")
    count = int(os.environ.get("LISTEN_FDS", "0"))
    return {names[i]: 3 + i for i in range(count) if i < len(names)}


class Virtual:
    """One uinput device, reused from systemd's fd store when possible."""

    def __init__(self, name, product, keys, rels, stored):
        self.dirty = False
        self.keys = keys
        self.reused = False
        store_name = f"vdev{product}"
        if store_name in stored:
            self.fd = stored[store_name]
            os.set_blocking(self.fd, False)
            self.reused = True
            log(f"reusing {name} from the fd store")
            return
        self.fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
        fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_KEY)
        for code in keys:
            fcntl.ioctl(self.fd, UI_SET_KEYBIT, code)
        if rels:
            fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_REL)
            for code in rels:
                fcntl.ioctl(self.fd, UI_SET_RELBIT, code)
        setup = struct.pack("HHHH80sI", BUS_VIRTUAL, 0x4D44, product, 1, name.encode(), 0)
        fcntl.ioctl(self.fd, UI_DEV_SETUP, setup)
        fcntl.ioctl(self.fd, UI_DEV_CREATE)
        notify(f"FDSTORE=1\nFDNAME={store_name}", [self.fd])
        log(f"created {name}")

    def emit(self, etype, code, value):
        os.write(self.fd, EVENT.pack(0, 0, etype, code, value))
        self.dirty = True

    def sync(self):
        if self.dirty:
            os.write(self.fd, EVENT.pack(0, 0, EV_SYN, SYN_REPORT, 0))
            self.dirty = False

    def release_all(self):
        """Every key comes up. A device reused from the fd store may still have down what the
        relay before this one pressed, and this one's node.held doesn't know it. The
        kernel drops the release of a key that isn't down, so this sends only what was held."""
        for code in self.keys:
            self.emit(EV_KEY, code, 0)
        self.sync()


def bits(fd, ev, count):
    buf = bytearray((count + 7) // 8)
    try:
        fcntl.ioctl(fd, eviocgbit(ev, len(buf)), buf)
    except OSError:
        return set()
    return {i for i in range(count) if buf[i // 8] >> (i % 8) & 1}


def read_config(path=os.path.expanduser("~/.config/frametop.conf")):
    """KEY=VALUE lines by the conf's grammar (ft_conf, docs/config.md). Missing file means defaults."""
    return ft_conf.read(path)


def pointer_settings(conf):
    """(pointer mode on, degrees per mouse count, idle s, wake counts) from frametop.conf. A value
    that isn't a number warns and keeps its default: one bad hand-edited line used to stop the
    relay at start, and systemd gave up restarting it."""
    return (conf.get("POINTER", "0") == "1",
            ft_conf.number(conf, "POINTER_SENSITIVITY", 0.03),
            ft_conf.number(conf, "POINTER_IDLE", 30.0),
            ft_conf.number(conf, "POINTER_WAKE_COUNTS", 40, int))


def kxkbrc_layout(path=KXKBRC):
    """The desktop's keyboard layout as (layout, variant, options, model): the first of kxkbrc's
    LayoutList and VariantList; None when there's no file, no list, or layouts are switched off
    there (Use=false: KWin's default, a US layout, then)."""
    cp = configparser.ConfigParser(interpolation=None, strict=False)
    cp.optionxform = str  # its keys are case-sensitive
    try:
        if not cp.read(path):
            return None
    except configparser.Error:
        return None
    sec = cp["Layout"] if cp.has_section("Layout") else {}

    def first(key):
        return (sec.get(key) or "").split(",")[0].strip()
    if (sec.get("Use") or "true").strip().lower() == "false" or not first("LayoutList"):
        return None
    options = sec.get("Options", "") if (sec.get("ResetOldOptions") or "").strip().lower() == "true" else ""
    return first("LayoutList"), first("VariantList"), options.strip(), (sec.get("Model") or "pc105").strip()


class XkbNames(ctypes.Structure):
    _fields_ = [("rules", ctypes.c_char_p), ("model", ctypes.c_char_p), ("layout", ctypes.c_char_p),
                ("variant", ctypes.c_char_p), ("options", ctypes.c_char_p)]


def layout_keys(layout, variant="", options="", model="pc105"):
    """{character: (code, modifiers)} for a keyboard layout, through libxkbcommon: every key in
    LAYOUT_CODES with each of LAYOUT_MODS held, and what it types. Dead keys type nothing, so
    their accents aren't in it. Raises OSError without libxkbcommon or a keymap for the layout."""
    lib = ctypes.CDLL(ctypes.util.find_library("xkbcommon") or "libxkbcommon.so.0")
    lib.xkb_context_new.restype = ctypes.c_void_p
    lib.xkb_context_new.argtypes = [ctypes.c_int]
    lib.xkb_context_unref.argtypes = [ctypes.c_void_p]
    lib.xkb_keymap_new_from_names.restype = ctypes.c_void_p
    lib.xkb_keymap_new_from_names.argtypes = [ctypes.c_void_p, ctypes.POINTER(XkbNames), ctypes.c_int]
    lib.xkb_keymap_unref.argtypes = [ctypes.c_void_p]
    lib.xkb_state_new.restype = ctypes.c_void_p
    lib.xkb_state_new.argtypes = [ctypes.c_void_p]
    lib.xkb_state_unref.argtypes = [ctypes.c_void_p]
    lib.xkb_state_update_key.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int]
    lib.xkb_state_key_get_utf8.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_char_p, ctypes.c_size_t]
    ctx = lib.xkb_context_new(0)
    if not ctx:
        raise OSError("libxkbcommon: no context")
    names = XkbNames(b"evdev", model.encode() or None, layout.encode(), variant.encode() or None,
                     options.encode() or None)
    keymap = lib.xkb_keymap_new_from_names(ctx, ctypes.byref(names), 0)
    if not keymap:
        lib.xkb_context_unref(ctx)
        raise OSError(f"no keymap for layout {layout}{f'({variant})' if variant else ''}")
    keys = {}
    buf = ctypes.create_string_buffer(16)
    try:
        for mods in LAYOUT_MODS:
            state = lib.xkb_state_new(keymap)
            for m in mods:
                lib.xkb_state_update_key(state, m + 8, 1)  # evdev code + 8: the XKB keycode; 1: down
            for code in LAYOUT_CODES:
                n = lib.xkb_state_key_get_utf8(state, code + 8, buf, len(buf))
                ch = buf.raw[:n].decode(errors="ignore") if 0 < n < len(buf) else ""
                if len(ch) == 1 and ch.isprintable() and ch not in keys:
                    keys[ch] = (code, mods)
            lib.xkb_state_unref(state)
    finally:
        lib.xkb_keymap_unref(keymap)
        lib.xkb_context_unref(ctx)
    return keys


class DesktopLayout:
    """The keys that type dictation's characters on the desktop (layout_keys), made again when
    kxkbrc changes; US_KEYS without a layout or libxkbcommon."""

    def __init__(self, path=None):
        self.path, self.stamp, self.keys, self.name = path, False, US_KEYS, "us"  # path None: KXKBRC

    def refresh(self):
        path = self.path or KXKBRC
        try:
            st = os.stat(path)
            stamp = (st.st_mtime_ns, st.st_size)
        except OSError:
            stamp = None
        if (path, stamp) == self.stamp:
            return
        self.stamp = (path, stamp)
        found = kxkbrc_layout(path) if stamp else None
        name = f"{found[0]}({found[1]})" if found and found[1] else found[0] if found else "us"
        keys = US_KEYS
        if found and name != "us":
            try:
                keys = layout_keys(*found)
            except OSError as e:
                log(f"typing on the desktop: {e}; a US layout instead")
                name = "us"
        if name != self.name or keys is not self.keys:
            log(f"typing on the desktop: layout {name}")
        self.name, self.keys = name, keys


def default_key_bindings():
    """The key combinations a rules file without its own gets."""
    return {**DEFAULT_KEY_BINDINGS, **(VOICE_KEY_BINDINGS if os.path.exists(VOICE_UNIT) else {})}


def read_rules(path=RULES_PATH):
    """{"devices": {id: {"role", "name"}}, "buttons": {id: {"<code>": action}},
    "controller_buttons": {"<hand>/<button>": action}, "controller_in_games": bool,
    "key_bindings": {"<code>+<code>...": action}, "meta_tap": bool,
    "vr_keyboard": one of VR_KEYBOARD_MODES, "vr_keyboard_persist": bool}."""
    try:
        with open(path) as f:
            rules = json.load(f)
    except (OSError, ValueError):
        rules = {}
    rules.setdefault("devices", {})
    rules.setdefault("buttons", {})
    rules.setdefault("controller_buttons", {})
    if not isinstance(rules.get("key_bindings"), dict):
        rules["key_bindings"] = default_key_bindings()
    for table in ("buttons", "controller_buttons", "key_bindings"):
        rules[table] = renamed_actions(rules[table])
    return rules


def remap_volume(fd, restore=False):
    """Point a device's volume keys at their stand-ins in its keymap, or back with restore.

    Returns how many keymap entries are volume keys or stand-ins, or None when the
    device has no keymap to change (uinput devices, some platform buttons).

    A swap that fails doesn't stop the others: every entry is still tried, then the
    first failure is raised. The entries that did swap stay swapped, for the caller
    to handle their stand-ins and restore them.
    """
    swap = VOLUME_ORIGINAL if restore else VOLUME_STANDIN
    found = 0
    failed = None  # the first swap that failed
    for index in range(8192):
        entry = bytearray(KEYMAP_ENTRY.pack(INPUT_KEYMAP_BY_INDEX, 0, index, 0, b""))
        try:
            fcntl.ioctl(fd, EVIOCGKEYCODE_V2, entry)
        except OSError:
            if not index:
                return None
            break  # past the last entry
        _, length, _, code, scancode = KEYMAP_ENTRY.unpack(entry)
        if code in VOLUME_CODES:
            found += 1
        if code in swap:
            try:
                fcntl.ioctl(fd, EVIOCSKEYCODE_V2,
                            KEYMAP_ENTRY.pack(INPUT_KEYMAP_BY_INDEX, length, index, swap[code], scancode))
            except OSError as e:
                if failed is None:
                    failed = e
    if failed is not None:
        raise failed
    return found


class Volume:
    """Volume keys: wpctl steps the default output, repeating while a key is held.

    The repeat is our own, since the headset's buttons have none; kernel autorepeat
    from keyboards is ignored so every device repeats the same way.
    """

    STEP = 5  # percent
    DELAY, RATE = 0.4, 0.1  # seconds before repeating, and between repeats

    def __init__(self):
        self.held = None  # (fd, code) of the key being held
        self.next_at = None

    def key(self, fd, code, value, now):
        if value == 1:
            if VOLUME_ORIGINAL.get(code, code) == KEY_MUTE:
                subprocess.Popen(["wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "toggle"],
                                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            else:
                self.held = (fd, code)
                self.step(code)
                self.next_at = now + self.DELAY
        elif value == 0 and self.held == (fd, code):
            self.release()

    def release(self):
        self.held = self.next_at = None

    def step(self, code):
        sign = "+" if VOLUME_ORIGINAL.get(code, code) == KEY_VOLUMEUP else "-"
        subprocess.Popen(["wpctl", "set-volume", "--limit", "1.0", "@DEFAULT_AUDIO_SINK@",
                          f"{self.STEP}%{sign}"],
                         stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def tick(self, now):
        if self.next_at is not None and now >= self.next_at:
            self.step(self.held[1])
            self.next_at = now + self.RATE

    def timeout(self, now, default):
        return default if self.next_at is None else max(0.0, min(default, self.next_at - now))


class Pointer:
    """Drives the ft_pointer SteamVR driver from a mouse (pointer mode).

    The virtual controller connects when the mouse is used (taking the right
    hand role and recentering on the gaze) and disconnects after `idle` seconds
    without mouse activity, so the real controllers get their role back: the
    last used device wins.
    """

    DRIVER_BUTTONS = {"left": "trigger", "right": "b", "middle": "x", "back": "joystick"}
    # SteamVR scrolls a step for each SCROLL_STEP seconds the joystick is fully deflected, about 20
    # steps a second at most, and adds up the time across short pushes. So each notch buys
    # SCROLL_STEP of deflection, and notches that come faster wait, up to SCROLL_BACKLOG seconds.
    SCROLL_STEP = 0.05
    SCROLL_BACKLOG = 0.5
    CLAIM_PULSE = 0.06  # seconds the claim button (switchlaserhand, no click) is held
    RESUME_PAUSE = 1.5  # mouse idle this long, then moving again, re-claims the laser
    WAKE_WINDOW = 1.0  # seconds in which WAKE_COUNTS of motion must add up
    QUEUE_MAX = 512  # commands kept while the helper is behind (see send)
    MOVE_EVERY = 0.004  # mouse motion goes to the helper at most this often (see flush)

    def __init__(self, sensitivity, idle, wake_counts=40):
        # Never blocks (see send): a stalled helper must not stall the keyboard, volume keys and pausing.
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM | socket.SOCK_NONBLOCK)
        # Commands the helper's full socket didn't take yet, in order: text, or [dyaw, dpitch] for
        # mouse moves, which add up into one while they wait.
        self.queue = collections.deque()
        self.behind_logged = -60.0
        self.sensitivity = sensitivity  # degrees per mouse count
        self.idle = idle
        self.active = False
        self.last_used = 0.0
        self.dx = self.dy = 0
        self.move_at = 0.0  # motion last went to the helper then
        self.scroll_until = None  # the joystick push ends then
        self.scroll_direction = 0
        # Scrolls notches on the panel the laser is on and says True, or says False: the joystick
        # then. PointerLease sets it (ft-screens' "keyboard", its fifth word).
        self.wheel_to = lambda notches: False
        self.claim_at = None  # when to press the claim button
        self.claim_release = None
        self.system_at = None  # dashboard toggle: when to press the virtual system button
        self.system_release = None
        # Waking (or re-claiming after a pause) needs deliberate movement, so sensor
        # jitter from a mouse lying on a desk can't steal the laser from a controller.
        self.wake_counts = wake_counts
        self.pending = 0
        self.pending_since = 0.0
        self.gaze_awake_until = 0.0  # the helper's gaze mode keeps the pointer until then
        # Driver buttons this pointer pressed and hasn't released (trigger, b, x, joystick: the
        # left, right, middle and back actions, from a mouse button, a mapped controller button
        # or a key combination): pausing drops releases, so stand_down has to send them itself.
        self.driver_down = set()

    def send(self, command, droppable=False):
        """To the helper, in order, without blocking. While the helper doesn't keep up (place and
        grabprobe hold it for seconds, and ft-gazed's 90 Hz gaze fills its socket meanwhile), commands
        wait in the queue and go out from tick(). A droppable one (a scroll notch: scrolling seconds
        late is no use) is dropped instead; presses, releases and the rest are kept, so no button
        stays down. Moves add up (_move)."""
        if not self.queue:
            try:
                self.sock.sendto(command.encode(), HELPER)
                return
            except BlockingIOError:
                pass
            except OSError:
                return  # helper not running (SteamVR not running)
        if not droppable:
            self._queue(command)

    def _move(self, dyaw, dpitch):
        if self.queue and isinstance(self.queue[-1], list):
            self.queue[-1][0] += dyaw
            self.queue[-1][1] += dpitch
            return
        if not self.queue:
            try:
                self.sock.sendto(f"move {dyaw:.4f} {dpitch:.4f}".encode(), HELPER)
                return
            except BlockingIOError:
                pass
            except OSError:
                return
        self._queue([dyaw, dpitch])

    def _queue(self, item):
        if not self.queue and time.monotonic() - self.behind_logged > 60:
            self.behind_logged = time.monotonic()
            log("pointer helper is behind: holding its commands (logged once a minute)")
        self.queue.append(item)
        if len(self.queue) > self.QUEUE_MAX:
            self.queue.popleft()  # stalled for long: the oldest goes

    def drain(self):
        """Send what waits, in order, as far as the helper takes it."""
        while self.queue:
            item = self.queue[0]
            text = f"move {item[0]:.4f} {item[1]:.4f}" if isinstance(item, list) else item
            try:
                self.sock.sendto(text.encode(), HELPER)
            except BlockingIOError:
                return
            except OSError:
                self.queue.clear()  # the helper went away: nothing to deliver to
                return
            self.queue.popleft()

    def wake(self, now):
        if not self.active:
            self.send("show")
            self.send("recenter")
            self.active = True
            self.claim_at = now + 0.3  # let SteamVR bind the freshly connected device first
            log("pointer on")
        elif now - self.last_used > self.RESUME_PAUSE and self.claim_at is None:
            self.claim_at = now  # another device may have taken the laser meanwhile
        self.last_used = now

    def motion(self, code, value, now):
        dormant = not self.active or now - self.last_used > self.RESUME_PAUSE
        if dormant and code in (REL_X, REL_Y):
            if now - self.pending_since > self.WAKE_WINDOW:
                self.pending, self.pending_since = 0, now
            self.pending += abs(value)
            if self.pending < self.wake_counts:
                return  # not yet deliberate movement
            self.pending = 0
        self.wake(now)
        if code == REL_X:
            self.dx += value
        elif code == REL_Y:
            self.dy += value
        elif code == REL_WHEEL and value:
            self.scroll(value, now)

    def action(self, name, value, now, source="mouse"):
        """A mapped button: value 1 press, 0 release, 2 autorepeat (ignored). source: what
        pressed it (mouse, left, right for a controller, keyboard), for the gaze actions,
        which take the mouse or the keyboard only."""
        if value == 2:
            return
        if name in ("gaze_left", "gaze_right"):
            if value == 1:
                self.wake(now)
            self.flush()
            self.send(f"gazekey {name[5:]} {value}")
            return
        if name in ("gaze_precision", "gaze_drag"):
            if source not in ("mouse", "keyboard"):
                return  # gaze mode is a mouse feature (GAZE_ACTIONS)
            if value == 1:
                self.wake(now)
            self.flush()
            self.send(f"{'precision' if name == 'gaze_precision' else 'gazedrag'} {source} {value}")
            return
        driver = self.DRIVER_BUTTONS.get(name)
        if driver:
            self.wake(now)
            self.flush()
            self.send(f"btn {driver} {value}")
            if value == 1:
                self.driver_down.add(driver)
            else:
                self.driver_down.discard(driver)
        elif value != 1:
            return  # the rest act on press
        elif name in ("scroll_up", "scroll_down"):
            self.wake(now)
            self.scroll(1 if name == "scroll_up" else -1, now)
        elif name == "dashboard":
            self.dashboard(now)
        elif name == "recenter":
            self.wake(now)
            self.send("recenter")
        elif name == "pointer_toggle":
            if self.active:
                self.send("hide toggle")  # gaze mode leaves it off until something wakes it
                self.active = False
                log("pointer off (toggle)")
            else:
                self.wake(now)
        elif name == "follow_toggle":
            self.send("follow toggle")  # until the next restart; the setting is POINTER_FOLLOW
            log("head follow toggled")
        elif name == "gaze_toggle":
            self.send("gaze toggle")  # until the next restart; the setting is POINTER_GAZE
            log("gaze mode toggled")
        elif name == "gaze_quickcal":
            try:
                self.sock.sendto(b"quickcal", GAZED)
            except OSError:
                pass  # the gaze service isn't running
        elif name == "layout_reset":
            # Runs a few seconds and borrows the pointer; ft-layout refuses a second copy.
            subprocess.Popen([FT_LAYOUT, "reset"], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL, start_new_session=True)
            log("layout reset")
        elif name in ("sens_up", "sens_down"):
            self.sensitivity *= 1.25 if name == "sens_up" else 0.8
            log(f"sensitivity {self.sensitivity:.4f} deg/count")

    def scroll(self, notches, now):
        """Scroll by notches (positive: up). On a Frametop panel, ft-screens takes them directly
        (wheel_to). Elsewhere, deflect the joystick for SCROLL_STEP per notch: a notch in the same
        direction adds its time to the push under way; the other direction starts again."""
        if self.wheel_to(notches):
            return
        direction = 1 if notches > 0 else -1
        if self.scroll_until is None or direction != self.scroll_direction:
            self.send(f"scroll 0 {direction}", droppable=True)
            self.scroll_direction, self.scroll_until = direction, now
        self.scroll_until = min(max(self.scroll_until, now) + abs(notches) * self.SCROLL_STEP,
                                now + self.SCROLL_BACKLOG)

    def flush(self, now=None):
        """Send the motion so far. With now (a mouse's SYN_REPORT), only once MOVE_EVERY has passed
        since the last: a 1000 Hz mouse sent the helper, which runs every 8 ms, 1000 datagrams a
        second. tick() sends the rest when it's due; a button sends it first, so it lands there."""
        if not (self.dx or self.dy) or (now is not None and now - self.move_at < self.MOVE_EVERY):
            return
        self.move_at = time.monotonic() if now is None else now
        # Mouse right turns the ray right (negative yaw); mouse down tilts it down.
        self._move(-self.dx * self.sensitivity, -self.dy * self.sensitivity)
        self.dx = self.dy = 0

    def dashboard(self, now=None):
        """Toggle the SteamVR dashboard with the virtual controller's system button.

        SteamVR needs the button held for a frame or two (press and release in the
        same instant is ignored), and the virtual controller must be connected and
        bound, so wake it first when needed.
        """
        now = time.monotonic() if now is None else now
        woke = not self.active
        self.wake(now)
        self.system_at = now + (0.4 if woke else 0.0)

    def tick(self, now):
        self.drain()
        self.flush(now)
        if self.system_at is not None and now >= self.system_at:
            self.send("btn system 1")
            self.system_at = None
            self.system_release = now + 0.12
        elif self.system_release is not None and now >= self.system_release:
            self.send("btn system 0")
            self.system_release = None
        if self.claim_at is not None and now >= self.claim_at:
            self.send("btn a 1")
            self.claim_at = None
            self.claim_release = now + self.CLAIM_PULSE
        elif self.claim_release is not None and now >= self.claim_release:
            self.send("btn a 0")
            self.claim_release = None
        if self.scroll_until is not None and now >= self.scroll_until:
            self.send("scroll 0 0")
            self.scroll_until = None
        if self.active and now - self.last_used > self.idle and now >= self.gaze_awake_until:
            self.send("hide idle")
            self.active = False
            log("pointer off (idle)")

    def timeout(self):
        pending = (self.scroll_until, self.claim_at, self.claim_release, self.system_at, self.system_release)
        wait = 0.02 if self.queue or any(t is not None for t in pending) else 0.5
        if self.scroll_until is not None:  # a push ends on time: its length is the number of steps
            wait = max(0.0, min(wait, self.scroll_until - time.monotonic()))
        if self.dx or self.dy:  # motion held back by flush
            wait = max(0.0, min(wait, self.move_at + self.MOVE_EVERY - time.monotonic()))
        return wait

    def stand_down(self, why="paused"):
        """Frametop is pausing (or POINTER=0 drops this pointer: why): a pulse under way ends
        now, and the pointer lets go.

        A click held into the pause (driver_down) comes up here, since pausing drops its
        release; otherwise the driver keeps the button down and the virtual controller
        reconnects with it pressed on resume. Gaze holds (gazekey, gazedrag, precision) aren't
        tracked here: the helper ends them itself when the pointer hides.

        The releases go before "hide", and "hide" goes even when the pointer was off already
        (the idle timeout or pointer_toggle with a button held): a helper built before releases
        stopped waking it would wake on one and connect the virtual controller during the game.
        And a relay that restarted doesn't know whether the helper still holds the pointer, so
        it can't tell from its own state.

        Not covered: gaze mode's held-back press (the helper's aim, before it becomes a real
        press) turns into a click on its release. The helper drops it without a click when it
        reads "hide" in the same loop as the release, as it normally does. If its socket was
        full, the rest of these wait in the queue (send), "hide" can land a loop later, and
        that press clicks once as the pause starts."""
        releases = [f"btn {driver} 0" for driver in sorted(self.driver_down)]
        self.driver_down.clear()
        if self.system_release is not None:
            releases.append("btn system 0")
        if self.claim_release is not None:
            releases.append("btn a 0")
        if self.scroll_until is not None:
            releases.append("scroll 0 0")
        for command in releases:
            self.send(command)
        self.system_at = self.system_release = self.claim_at = self.claim_release = self.scroll_until = None
        self.dx = self.dy = self.pending = 0
        self.gaze_awake_until = 0.0
        self.send("hide pause")
        if self.active:
            self.active = False
            log(f"pointer off ({why})")


class Node:
    """One input event node: a candidate device (mouse or keyboard, USB or Bluetooth),
    or any other device with volume keys (candidate False, role "volume")."""

    def __init__(self, path, fd, name, bus, vendor, product, uniq, is_mouse, is_keyboard,
                 candidate=True, volume_keys=False, only_volume=False, uinput=False, network=False):
        self.path, self.fd, self.name = path, fd, name
        # A host's keyboard or mouse (see "Host devices"): fd is a pipe's read end, wfd its write
        # end; until: it goes away then without word; down: the keys its host holds.
        self.network, self.wfd, self.until, self.down = network, None, 0.0, set()
        self.uinput = uinput  # made by a program (a remote desktop's keyboard, say), not a real device
        self.bus, self.vendor, self.product, self.uniq = bus, vendor, product, uniq
        self.is_mouse, self.is_keyboard = is_mouse, is_keyboard
        self.candidate, self.volume_keys, self.only_volume = candidate, volume_keys, only_volume
        # One physical device, whatever its node: Bluetooth address, else USB ids plus name.
        base = self.name.split(" Mouse")[0].split(" Keyboard")[0]
        self.id = uniq.lower() if uniq else f"usb:{vendor:04x}:{product:04x}:{base}"
        self.role = None if candidate else "volume"
        self.grabbed = False
        self.remapped = False  # volume keys remapped to their stand-ins
        self.held = set()  # keys and buttons currently down, released if the device vanishes
        self.actions = {}  # pointer-mode buttons down: code -> the action they pressed (drop)
        self.last_watch = 0.0

    def describe(self):
        kinds = [k for k, on in (("mouse", self.is_mouse), ("keyboard", self.is_keyboard)) if on]
        return {"path": self.path, "name": self.name, "id": self.id, "uniq": self.uniq,
                "bus": {BUS_USB: "usb", BUS_BLUETOOTH: "bluetooth", BUS_NETWORK: "network"}.get(self.bus, str(self.bus)),
                "kinds": kinds, "role": self.role, "grabbed": self.grabbed, "uinput": self.uinput}


def node_role(rule, node, nodes):
    """A candidate node's role, from its device's rule in frametop-input.json ({} for none) and
    the nodes there are: "combo" splits the device by node (see the top), the other roles
    take every node of it, and without one a device with a mouse node is a pointer."""
    if rule.get("role") == "combo":
        return "pointer" if node.is_mouse else "passthrough"
    if rule.get("role") in ("pointer", "passthrough", "ignore"):
        return rule["role"]
    has_mouse = any(n.is_mouse for n in nodes if n.id == node.id) or node.is_mouse
    return "pointer" if has_mouse else "passthrough"


def probe(path):
    """Open a node if it is a USB or Bluetooth mouse or keyboard, or has volume keys,
    else return None."""
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
    except OSError:
        return None
    try:
        buf = bytearray(256)
        fcntl.ioctl(fd, eviocgname(len(buf)), buf)
        name = buf.split(b"\0", 1)[0].decode(errors="replace")
        ident = bytearray(8)
        fcntl.ioctl(fd, EVIOCGID, ident)
        bus, vendor, product, _ = struct.unpack("HHHH", ident)
        if name.startswith(VIRTUAL_PREFIX):
            raise ValueError
        uniq_buf = bytearray(64)
        try:
            fcntl.ioctl(fd, eviocguniq(len(uniq_buf)), uniq_buf)
            uniq = uniq_buf.split(b"\0", 1)[0].decode(errors="replace")
        except OSError:
            uniq = ""
        keys = bits(fd, EV_KEY, KEY_MAX + 1)
        is_mouse = REL_X in bits(fd, EV_REL, REL_MAX + 1)
        is_keyboard = KEY_A in keys
        candidate = bus in (BUS_USB, BUS_BLUETOOTH) and (is_mouse or is_keyboard)
        volume_keys = bool(keys & VOLUME_CODES)  # stand-ins too: kept from before a relay restart
        if not (candidate or volume_keys):
            raise ValueError
        # uinput devices live here (Bluetooth LE ones come through uhid, under virtual/misc).
        sysfs = os.path.realpath(f"/sys/class/input/{os.path.basename(path)}/device")
        return Node(path, fd, name, bus, vendor, product, uniq, is_mouse, is_keyboard,
                    candidate, volume_keys, keys <= VOLUME_CODES, sysfs.startswith("/sys/devices/virtual/input/"))
    except (OSError, ValueError):
        os.close(fd)
        return None


class PointerLease:
    """The 3D mouse's hold on the pointer helper. conf: the Pointer frametop.conf asks for (None
    with POINTER=0). current: the one in use, conf but None while Frametop is paused. holding:
    conf had the pointer on as last seen; the pause gesture's reader looks the controllers up
    again when that changes, since the 3D mouse's virtual controller takes a hand role."""

    def __init__(self):
        self.conf = None
        self.current = None
        self.holding = False
        # Each Pointer's wheel_to (Pointer.scroll): the relay's, which sends notches to ft-screens.
        self.wheel_to = lambda notches: False

    def configure(self, on, sensitivity, idle, wake_counts, paused):
        """frametop.conf's pointer settings (pointer_settings), put in place."""
        if on:
            p = self.conf or Pointer(0.03, 30)
            p.wheel_to = self.wheel_to
            p.sensitivity, p.idle, p.wake_counts = sensitivity, idle, wake_counts
            self.conf = p
            log(f"pointer mode: {p.sensitivity} deg/count, idle {p.idle} s, wake {p.wake_counts} counts")
        else:
            if self.conf:
                # A button held through the change comes up on the helper now; its release
                # goes to the virtual mouse, which never had it down.
                self.conf.stand_down("pointer mode off")
            self.conf = None
            log("pointer mode off: pointer devices feed the virtual mouse and keyboard")
        self.current = None if paused else self.conf

    def pause(self, paused):
        """Frametop paused, and the pointer stands down so the helper lets go, or resumed."""
        if paused:
            if self.conf:
                self.conf.stand_down()
            self.current = None
        else:
            self.current = self.conf

    def holding_changed(self):
        """Whether holding changed since the last call."""
        holding = bool(self.conf and self.conf.active)
        if holding == self.holding:
            return False
        self.holding = holding
        return True


class HeldKeys:
    """What the keyboards hold, as the key combinations, the modifier taps and the desktop know it."""

    def __init__(self):
        self.modifiers = set()  # on any keyboard, folded (MODIFIERS)
        self.tap = None  # the modifier (folded) pressed alone, with nothing since: its release is a tap
        self.tap_code = None  # the tap's key as it is (a right-hand code isn't folded), see modifier_tap
        self.talk_code = None  # a modifier bound to dictate_hold, held alone: dictation runs until its release
        self.talk_hidden = False  # its press kept from the desktop, so the text typed meanwhile isn't a shortcut
        self.meta = set()  # the Meta keys held, as they are (KEY_LEFTMETA, KEY_RIGHTMETA)
        self.meta_hidden = set()  # held Meta keys the desktop was told came up (a combination; key_binding)
        self.combos = {}  # key code -> the action its combination started (released with it)
        self.cal = set()  # Enter keys whose press answered the gaze calibration panel (cal_enter)
        self.desktop = set()  # keys the desktop was told went down and not yet up (to_screens)

    def forget_up(self, down):
        """The Meta keys and modifiers no device holds are forgotten (down: the keys the devices hold)."""
        for code in [c for c in self.meta if c not in down]:
            self.meta.discard(code)
            self.meta_hidden.discard(code)
        self.modifiers.intersection_update({MODIFIERS[c] for c in down if c in MODIFIERS})


class Typing:
    """What the relay still types, and what it has typed down."""

    def __init__(self):
        # In order: a character (str) from dictation, ("quiet", character) from Full Keyboard
        # (its text isn't logged), or ("key", code, value), a key Full Keyboard pressed or released.
        self.queue = collections.deque()
        self.down = []  # the character pressed last, (code, modifiers, to the desktop): its release is next
        self.at = 0.0  # the next press or release goes then
        self.untyped = []  # characters with no key where typing went, logged when the queue is empty (None: quiet)
        self.committing = False  # the last character went through the input method, not as keys
        self.kb_held = {}  # keys Full Keyboard holds down: code -> to the desktop

    def drop_full_keyboard(self):
        """Full Keyboard went: its keys still to come are dropped. Returns the keys it held, as
        (code, to the desktop), for the caller to let go of."""
        kept = [item for item in self.queue if isinstance(item, str) or item[0] != "key"]
        self.queue.clear()
        self.queue.extend(kept)
        held = sorted(self.kb_held.items())
        self.kb_held.clear()
        return held


class FullKeyboardLink:
    """Full Keyboard (keyboard/, --frametop), as the control socket knows it."""

    def __init__(self):
        self.addr = None  # its address
        self.until = 0.0  # it counts as running until then (its hello renews it)
        self.said = None  # the "kb DESKTOP SURFACE" it was told last


CREDS = struct.Struct("3i")  # struct ucred: pid, uid, gid, as the kernel adds it to each datagram


def peer_creds(ancillary):
    """(pid, uid) of a datagram's sender, from its SCM_CREDENTIALS (recvmsg's ancillary data), or
    (None, None) without them."""
    return next((CREDS.unpack(d[:CREDS.size])[:2] for level, kind, d in ancillary
                 if level == socket.SOL_SOCKET and kind == socket.SCM_CREDENTIALS), (None, None))


class Relay:
    """The relay: the devices it reads, the control socket, and what it keeps between events.
    main() makes the virtual devices and the sockets, and runs it. A test can make one with fakes
    of its own and call its methods."""

    def __init__(self, mouse, keyboard, control, screens_sock, can_grab=True, stored=None):
        self.mouse, self.keyboard = mouse, keyboard  # the virtual mouse and keyboard
        self.control = control  # @frametop_relay, bound, with SO_PASSCRED
        self.screens_sock = screens_sock  # unbound: sends to ft-screens, the helper and the rest
        self.can_grab = can_grab  # not under --no-grab: that's the tests (input/test)
        self.stored = stored if stored is not None else {}  # systemd's fd store (stored_fds)
        self.watchers = {}  # address -> watch end time

        # desktop_until: typing goes to the Frametop desktop until then (ft-screens says so
        # every second); typing_applied: the grabs match that as of the last apply_roles().
        # vr_capture_until: every controller button is taken until then (the settings app capturing one).
        # shown_until: ft-screens shows the desktop's screens until then (it says so with "keyboard").
        # wheel_until: the 3D mouse's wheel goes to ft-screens until then (the same, its fifth word).
        self.rules = {}
        self.desktop_until, self.typing_applied = 0.0, None
        self.vr_capture_until, self.vr_bind_retry = 0.0, False
        self.calpanel_until, self.shown_until, self.wheel_until = 0.0, 0.0, 0.0
        self.text_field_said, self.text_field_said_at = None, 0.0  # text_field's last log line, and when
        # ft-textinput's address while a desktop text field is focused (its "textfield 1"), for the
        # characters the desktop's layout has no key for, until text_input_until; None without one.
        self.text_input, self.text_input_until = None, 0.0
        self.last_typing = 0.0  # the helper was last told of a key then (see "typing" at the top)
        self.lease = PointerLease()
        self.lease.wheel_to = self.wheel_to
        self.held = HeldKeys()
        self.typing = Typing()
        self.kb = FullKeyboardLink()

        self.nodes = {}  # fd -> Node
        # Nodes already probed (rejected or open): path -> inode. A device that disconnects and
        # reconnects between two scans often gets the same event numbers back, so the path alone
        # would hide it; the re-created node has a new inode.
        self.seen = {}
        self.next_scan = 0.0
        self.grab_waiting = False  # a keyboard's grab waits for its keys to come up
        self.volume = Volume()
        self.desktop_layout = DesktopLayout()

        self.pause = game_pause.GamePause(log, self.pause_changed, VR_BUTTONS)
        self.pc_game = pc_game.PcGame(log)
        self.gamepad = vrpad.GamepadMode(log, self.pad_send, lambda: vrpad.VirtualPad(log, self.stored, notify),
                                         lambda on: self.pause.set_pad(on))
        self.load_config()

    def pause_changed(self, paused):
        """Frametop paused or resumed (game_pause.py): the relay's own part."""
        if paused:
            self.gamepad.stop("Frametop paused")
            self.lease.pause(True)
            self.desktop_until = 0.0  # typing goes to Steam
        else:
            self.lease.pause(False)
        self.apply_roles()
        self.vr_bind(time.monotonic())

    def pad_send(self, to, message):
        try:
            self.screens_sock.sendto(message.encode(), SCREENS)
        except OSError:
            pass  # not running

    def gamepad_blocked(self):
        """Why gamepad mode can't be on now, or None."""
        return "Frametop is paused" if self.pause.paused else "a VR game runs" if self.pause.game else None

    def load_config(self):
        """frametop.conf and the rules, applied all or nothing: read and checked first (the pause's
        settings from the rules too), then put in place. A reload whose rules fail keeps the last
        ones (it used to leave the rules read and the pause unconfigured); at start, the
        default rules go with the conf's settings. A bad conf value warns and keeps its default."""
        conf = read_config()
        pointer_on, sensitivity, idle, wake_counts = pointer_settings(conf)
        try:
            rules = read_rules()
            self.pause.configure(rules)   # first: it works the rules out before it changes anything
        except Exception as e:  # a reload must not take the relay down
            if self.rules:
                log(f"config not applied, the last one kept: {e!r}")
                return
            log(f"rules not applied, the defaults used: {e!r}")
            rules = read_rules("")   # the defaults (no file there)
            self.pause.configure(rules)
        self.rules = rules
        self.pc_game.configure(rules)
        self.lease.configure(pointer_on, sensitivity, idle, wake_counts, self.pause.paused)
        self.gamepad.sound_on = self.pause.settings["sound"]
        # Hand interaction: the pointer helper's handoff arbiter needs to know who holds a controller.
        # HANDS_MODE=always keeps hand tracking running through games.
        # Direct touch alone (HANDS_NEAR=on) needs the same: the helper's handoff runs for it.
        self.pause.set_hands(conf.get("POINTER_HANDS", "0") == "1" or conf.get("HANDS_NEAR") == "on",
                        conf.get("HANDS_MODE") == "always", conf.get("HANDS_MODE") == "frametop")

    def vr_bind(self, now):
        """Tell the pointer helper which controller buttons to take from games."""
        if now < self.vr_capture_until:
            buttons = "*"
        else:
            self.vr_capture_until = 0.0
            buttons = " ".join(b for b, a in self.rules["controller_buttons"].items()
                               if b in VR_BUTTONS and known_action(a) and a not in ("key", "none")
                               and a not in GAZE_ACTIONS and (not self.pause.paused or works_paused(a))) or "-"
            if self.rules.get("controller_in_games"):
                buttons = "+games " + buttons
        self.vr_bind_retry = False
        try:
            self.screens_sock.sendto(f"vrbind {buttons}".encode(), HELPER)
        except BlockingIOError:
            self.vr_bind_retry = True  # the helper is behind: again on the next loop
        except OSError:
            pass  # helper not running; it says vrhello when it starts

    def vr_button(self, button, value, now):
        """A Frame controller button from the pointer helper."""
        for addr, until in list(self.watchers.items()):
            if now > until:
                del self.watchers[addr]
            else:
                self.reply(addr, {"t": "event", "id": VR_DEVICE, "path": "", "name": "Steam Frame controllers",
                             "type": "vr", "code": button, "value": value})
        action = self.rules["controller_buttons"].get(button)
        if (self.lease.current or (action and not needs_pointer(action))) and known_action(action) \
                and action not in ("key", "none") and action not in GAZE_ACTIONS:
            self.do_action(action, value, now, button.split("/")[0])

    def vr_keyboard_mode(self):
        mode = self.rules.get("vr_keyboard")
        return mode if mode in VR_KEYBOARD_MODES else "no_keyboard"

    def vr_keyboard(self, command):
        """Open or close Frametop's keyboard (ft-screens)."""
        try:
            self.screens_sock.sendto(f"vrkeyboard {command}".encode(), SCREENS)
        except OSError:
            pass  # ft-screens not running

    def wheel_to(self, notches):
        """The 3D mouse's wheel to ft-screens, while its laser is on a panel (Pointer.scroll):
        every notch, at any speed, where SteamVR's joystick scrolls 20 steps a second at most."""
        if time.monotonic() >= self.wheel_until:
            return False
        try:
            self.screens_sock.sendto(f"wheel {notches}".encode(), SCREENS)
        except OSError:
            return False  # ft-screens not running, or behind: the joystick then
        return True

    def text_field(self, focused):
        """A text field on the desktop gained or lost keyboard focus (the input method)."""
        mode = self.vr_keyboard_mode()
        if not focused:
            if not self.rules.get("vr_keyboard_persist", True):
                self.vr_keyboard("hide")  # ft-screens closes it only if it opened it for a text field
            return
        keyboards = sorted({n.name for n in self.nodes.values()
                            if n.candidate and n.is_keyboard and n.role == "passthrough" and not n.uinput})
        if mode == "always" or (mode == "no_keyboard" and not keyboards):
            self.vr_keyboard("show")
            why = "asking ft-screens to open Frametop's keyboard"
        elif mode == "no_keyboard":
            why = (f"not opening Frametop's keyboard: a keyboard is connected ({', '.join(keyboards)}), "
                   "and the Keyboard setting opens it only without one")
        else:
            why = f"not opening Frametop's keyboard (Keyboard setting: {mode})"
        # For bug reports (scripts/report.sh): once per decision, again after 30 s.
        now = time.monotonic()
        if why != self.text_field_said or now - self.text_field_said_at > 30:
            log(f"text field focused: {why}")
            self.text_field_said, self.text_field_said_at = why, now

    def do_action(self, action, value, now, source="mouse"):
        """A mapped mouse or controller button, or key combination (pointer mode only, but
        for the actions needs_pointer() says don't)."""
        if value and not ft_pinlock.allowed() and action not in ("steam_menu", "release_keys", "none"):
            return
        if action == "pause_toggle":
            if value == 1:
                self.pause.toggle(PAUSE_SOURCES.get(source, source), now)
            return
        if self.pause.paused and not works_paused(action):
            return
        if action == "keyboard_toggle":
            if value == 1 and self.vr_keyboard_mode() != "never":
                self.vr_keyboard("toggle")
        elif action.startswith(PROFILE):
            if value == 1:
                # Runs a few seconds and borrows the pointer, like layout_reset.
                subprocess.Popen([FT_LAYOUT, "use", action[len(PROFILE):]], stdin=subprocess.DEVNULL,
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
                log(action)
        elif action == "steam_menu":
            if value == 1:
                # Talks to Steam's UI for a moment; its errors go to this service's log.
                subprocess.Popen([FT_STEAM, "menu"], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                 start_new_session=True)
                log(action)
        elif action == "release_keys":
            if value == 1:
                self.release_all_keys()
        elif action == "desktopless_toggle":
            if value == 1:
                # Takes a few seconds (every window moves); its outcome goes to this service's log.
                subprocess.Popen([FT_LAYOUT, "desktopless", "toggle"], stdin=subprocess.DEVNULL,
                                 stdout=subprocess.DEVNULL, start_new_session=True)
                log(action)
        elif action == "hands_toggle":
            if value == 1:
                # systemctl --no-block, and the pause's sounds if it plays them; the outcome goes
                # to this service's log.
                subprocess.Popen(["python3", HANDSMODE, "toggle", *(["--sound"] if self.pause.settings["sound"] else [])],
                                 stdin=subprocess.DEVNULL, start_new_session=True)
                log(action)
        elif action.startswith(KEYS):
            if value == 1:
                self.tap_keys(keys_codes(action), now)
        elif action.startswith(COMMAND):
            if value == 1:
                subprocess.Popen(["sh", "-c", action[len(COMMAND):]], stdin=subprocess.DEVNULL,
                                 env=dict(os.environ, PATH=COMMAND_PATH), start_new_session=True)
                log(action)
        elif action.startswith(RUN):
            if value == 1:
                # A transient user unit: it never holds up the relay, and its output goes to the
                # journal (ft-cmd log NAME).
                _, message = ft_commands.run_named(action[len(RUN):])
                log(message)
        elif action in FLOAT_ACTIONS:
            if value == 1:
                try:
                    self.screens_sock.sendto(FLOAT_ACTIONS[action], FLOAT)
                except OSError:
                    pass  # the Frametop desktop isn't running
                log(action)
        elif action in SCREENS_ACTIONS:
            if value == 1:
                try:
                    self.screens_sock.sendto(SCREENS_ACTIONS[action], SCREENS)
                except OSError:
                    pass  # the Frametop desktop isn't running
                log(action)
        elif action in ("frunner", "os_menu"):
            if value == 1:
                try:
                    self.screens_sock.sendto(f"toggle {source}{' actions' if action == 'os_menu' else ''}".encode(),
                                             FRUNNER)
                except OSError:
                    log("frunner: not running (the Frametop desktop starts it)")
                log(action)
        elif action in VOICE_ACTIONS:
            # dictate starts or stops on a press; dictate_hold starts on the press and stops on
            # the release. The voice service logs it.
            command = {1: "toggle"}.get(value) if action == "dictate" else {1: "start", 0: "stop"}.get(value)
            if command:
                try:
                    self.screens_sock.sendto(f"{command} {source}".encode(), VOICE)
                except OSError:
                    log(f"{action}: Frametop Voice isn't running (frametop-voice.service)")
        elif self.lease.current:
            self.lease.current.action(action, value, now, source)

    def tap_keys(self, codes, now):
        """A "keys:" action: its keys go down in order and come up in reverse, at once, where typing
        goes (the desktop's screens, else the virtual keyboard for Steam). Down and up together,
        so a release that never comes (a controller gone) can't leave a key held. Modifiers the
        desktop holds (the key combination that did this) come up there first and go down again
        after, and one the action has too is left as it is, so the desktop gets the action's keys
        alone. Steam reads the keyboards itself, so there it sees the held ones too."""
        if not codes:
            return
        desktop = now < self.desktop_until
        held = sorted(c for c in self.held.desktop if c in MODIFIERS) if desktop else []
        lift = [c for c in held if c not in codes]
        for code in lift:
            self.to_screens(code, 0)
        for value, order in ((1, codes), (0, codes[::-1])):
            for code in order:
                if code not in held:
                    self.type_key(code, value, desktop)
        for code in lift:
            self.to_screens(code, 1)
        log(f"keys {'+'.join(map(str, codes))} to {'the desktop' if desktop else 'Steam'}")

    def cal_enter(self, code, value, now):
        """Enter while the gaze calibration panel is up (see the top): its press goes to the helper
        as "calkey 1", and the press, its repeats and its release aren't typed. True if taken."""
        if value == 1 and code in (KEY_ENTER, KEY_KPENTER) and now < self.calpanel_until \
                and not self.held.modifiers and code not in self.held.combos:
            self.held.cal.add(code)
            try:
                self.screens_sock.sendto(b"calkey 1", HELPER)
            except OSError:
                pass  # helper not running (SteamVR not running)
            log("Enter: the gaze calibration panel's")
            return True
        if code in self.held.cal:
            if value == 0:
                self.held.cal.discard(code)
            return True
        return False

    def key_binding(self, code, value, now):
        """A key from a keyboard: does it complete a key combination ("key_bindings")? True if
        it was taken for one (then it isn't typed)."""
        if code in MODIFIERS:
            mod = MODIFIERS[code]
            hide = value == 2 and code == self.held.talk_code and self.held.talk_hidden  # its repeats, as its press
            if value == 1:
                if self.held.talk_code is not None and code != self.held.talk_code:
                    self.cancel_talk()  # another modifier with it: a shortcut (Ctrl+Shift+T)
                self.held.tap = mod if not self.held.modifiers and not self.held.combos else None
                self.held.tap_code = code
                if self.held.tap is not None and self.held.talk_code is None and self.tap_action(mod, code)[1] == "dictate_hold":
                    # Hold to talk: dictation starts on the press, not the release. The desktop
                    # doesn't get the press (cancel_talk sends it after all for a shortcut), so
                    # live text is typed there while it's held. Steam and games read the keyboard
                    # themselves: there the text waits for the release (typing_tick).
                    self.held.talk_code, self.held.talk_hidden, hide = code, True, True
                    self.do_action("dictate_hold", 1, now, "keyboard")
                    log(f"key combination {self.tap_action(mod, code)[0]}: dictate_hold (held)")
                elif self.held.tap is not None and any(c not in MODIFIERS for c in self.physically_down()):
                    # A key or button already down (a drag, a game's W) and held right through
                    # sends nothing meanwhile (mice don't repeat): it isn't a tap either.
                    self.held.tap = None
            (self.held.modifiers.add if value else self.held.modifiers.discard)(mod)
            if code in (KEY_LEFTMETA, KEY_RIGHTMETA):
                (self.held.meta.add if value else self.held.meta.discard)(code)
                if value == 0 and code in self.held.meta_hidden:
                    self.held.meta_hidden.discard(code)
                    return True  # the desktop already had it come up (below)
            if value == 0 and code == self.held.talk_code:
                self.held.talk_code, self.held.tap = None, None
                hide, self.held.talk_hidden = self.held.talk_hidden, False
                if not hide:
                    # As with a tap (modifier_tap): a key in between, so a Meta release isn't one.
                    self.to_screens(KEY_F24, 1)
                    self.to_screens(KEY_F24, 0)
                self.do_action("dictate_hold", 0, now, "keyboard")
                return hide
            if value == 0 and self.held.tap == mod:
                self.held.tap = None
                self.modifier_tap(mod, self.held.tap_code, now)
            return hide
        if value == 0 and code in self.held.combos:
            action = self.held.combos.pop(code)
            if self.lease.current or not needs_pointer(action):
                self.do_action(action, 0, now, "keyboard")
            return True
        combo, action = self.combo_for(code) if value == 1 else (None, None)
        if value == 1 and action is None and self.held.meta_hidden:
            # Another key while Meta is still held after a combination: the desktop gets Meta
            # back first, so Meta+that key still works there. Not for one that's a combination
            # too (H again with Meta+Shift held): Meta stays up there, with nothing sent.
            for c in sorted(self.held.meta_hidden):
                self.to_screens(c, 1)
            self.held.meta_hidden.clear()
        if action is None:
            return value == 2 and code in self.held.combos
        self.held.combos[code] = action
        if self.held.meta - self.held.meta_hidden:
            # The desktop saw Meta go down. Its release mustn't open Plasma's launcher, and Meta
            # comes up there now: KWin takes Meta with a mouse button for moving or resizing
            # windows, which would swallow a gaze click. Its real release is dropped (above).
            # KWin opens the launcher only for Meta pressed and released with no other key down
            # in between. With another key the desktop holds (Shift of Meta+Shift+H), that's
            # already so; Meta alone gets an F24 press in between. F24 goes to the focused app,
            # which may type it, and with Shift held a terminal did (DEE-326).
            if not any(c < BTN_MISC and c not in self.held.meta for c in self.held.desktop):
                self.to_screens(KEY_F24, 1)
                self.to_screens(KEY_F24, 0)
            for c in sorted(self.held.meta - self.held.meta_hidden):
                self.to_screens(c, 0)
            self.held.meta_hidden.update(self.held.meta)
        if self.lease.current or not needs_pointer(action):
            self.do_action(action, 1, now, "keyboard")
        log(f"key combination {combo}: {action}")
        return True

    def combo_for(self, code):
        """The key combination a key's press completes with the modifiers held, and its action:
        (combo, action), the action None when it does nothing now."""
        if not self.rules["key_bindings"]:
            return None, None
        combo = "+".join(str(c) for c in sorted(self.held.modifiers) + [code])
        action = self.rules["key_bindings"].get(combo)
        if not known_action(action):
            combo, action = self.release_combo(code) or (combo, action)
        if not known_action(action) or action in ("key", "none") or (self.pause.paused and not works_paused(action)):
            return combo, None
        return combo, action

    def release_combo(self, code):
        """A release_keys combination this key completes with the modifiers held, more of them
        too (one may be stuck here as well): (combo, "release_keys"), or None."""
        for combo, action in self.rules["key_bindings"].items():
            if action != "release_keys":
                continue
            *mods, last = combo.split("+")
            if mods and last == str(code) and all(m.isdecimal() for m in mods) \
                    and {int(m) for m in mods} <= self.held.modifiers:
                return combo, action
        return None

    def tap_action(self, mod, code):
        """A lone modifier's combination and action, (combo, None) when it does nothing now. A
        right-hand key's own binding ("97": Right Ctrl) comes before the folded one."""
        bindings = self.rules["key_bindings"]
        combo = str(code) if code != mod and str(code) in bindings else str(mod)
        action = bindings.get(combo)
        if mod == KEY_LEFTMETA and not self.rules.get("meta_tap", True):
            return combo, None  # Meta taps turned off (Input Settings): Meta as an unbound one
        if not known_action(action) or action in ("key", "none") or (self.pause.paused and not works_paused(action)):
            return combo, None
        return combo, action

    def cancel_talk(self):
        """A key, mouse button or scroll while a hold-to-talk modifier is held: it was a shortcut
        (Ctrl+C), so the dictation it started is dropped, without its cancel tone."""
        if self.held.talk_code is not None:
            if self.held.talk_hidden:
                self.to_screens(self.held.talk_code, 1)  # the desktop gets the modifier before the key it's for
            self.held.talk_code, self.held.talk_hidden = None, False
            try:
                self.screens_sock.sendto(b"cancel keyboard quiet", VOICE)
            except OSError:
                pass
            log("dictate_hold: cancelled (another key while held: a shortcut)")

    def modifier_tap(self, mod, code, now):
        """A modifier pressed and released alone: its binding, if it has one ("125": Meta tap)."""
        combo, action = self.tap_action(mod, code)
        if action is None or action == "dictate_hold":
            return  # nothing, or hold to talk (key_binding), which started on the press
        # The desktop gets a key in between before the release goes there, so the tap isn't one
        # there too: a Meta tap would open Plasma's launcher.
        self.to_screens(KEY_F24, 1)
        self.to_screens(KEY_F24, 0)
        if self.lease.current or not needs_pointer(action):
            self.do_action(action, 1, now, "keyboard")
            self.do_action(action, 0, now, "keyboard")
        log(f"key combination {combo}: {action}")

    def to_screens(self, code, value, button=False):
        """A key for the desktop screens (ft-screens decides whether it types): one below
        BTN_MISC, or with button, a mouse button (BTN_MOUSE..BTN_TASK), which ft-screens gives
        the screen the pointer is on. Nothing else (gamepad, joystick, digitizer buttons, keys
        from KEY_OK up), but the release of anything the desktop has down."""
        if value not in (0, 1):
            return
        if code < BTN_MISC or (button and BTN_MOUSE <= code <= BTN_TASK) or (not value and code in self.held.desktop):
            try:
                self.screens_sock.sendto(f"key {code} {value}".encode(), SCREENS)
            except OSError:
                return  # ft-screens not running
            (self.held.desktop.add if value else self.held.desktop.discard)(code)

    def type_text(self, text, quiet=False):
        """Queue dictation's text (see the top), or with quiet, Full Keyboard's. Which keys type
        it is worked out as it goes, where typing goes then."""
        self.desktop_layout.refresh()
        chars = [c for c in text if c.isprintable()]
        room = max(0, TYPE_MAX - len(self.typing.queue))
        self.typing.queue.extend((("quiet", c) for c in chars[:room]) if quiet else chars[:room])
        if not quiet or len(chars) > room:
            log(f"typing {min(len(chars), room)} characters for {'Full Keyboard' if quiet else 'Frametop Voice'}"
                + (f" ({len(chars) - room} dropped: too many waiting)" if len(chars) > room else ""))

    def commit_text(self, text):
        """Send text to ft-textinput, which commits it to the focused text field. False if it
        didn't go. If ft-textinput is gone, there's no text field to send to until it says again;
        if it's only behind (its queue full), the next character tries again."""
        try:
            self.control.sendto(f"commit {text}".encode(), self.text_input)
            return True
        except OSError as e:
            if e.errno in (errno.ECONNREFUSED, errno.ENOENT):
                self.text_input = None
            return False

    def report_untyped(self, desktop):
        if self.typing.untyped and not self.typing.queue:
            where = (f"the desktop's layout ({self.desktop_layout.name}), with no text field taking text from "
                     "the input method," if desktop else "a US layout (Steam)")
            said = "".join(dict.fromkeys(c for c in self.typing.untyped if c is not None))
            log(f"typing: {len(self.typing.untyped)} characters {where} has no key for" + (f": {said}" if said else ""))
            self.typing.untyped.clear()

    def type_key(self, code, value, desktop):
        if desktop:
            self.to_screens(code, value)
        else:
            self.keyboard.emit(EV_KEY, code, value)
            self.keyboard.sync()

    def typing_blocked(self, now):
        """A modifier is held (the text would be shortcuts), except a hold-to-talk key the
        desktop never got, when typing goes there."""
        held = self.held.modifiers
        if self.held.talk_hidden and now < self.desktop_until:
            held = held - {MODIFIERS[self.held.talk_code]}
        return bool(held)

    def typing_tick(self, now):
        """Release the character pressed last, or press the next, when it's time. A new one
        waits while a modifier is held."""
        if now < self.typing.at:
            return
        if self.typing.down:
            code, mods, desktop = self.typing.down.pop()
            self.type_key(code, 0, desktop)
            for m in reversed(mods):
                self.type_key(m, 0, desktop)
        elif self.typing.queue and self.typing.queue[0][0] == "key":
            # Full Keyboard's key, with its modifiers its own: a held one doesn't stop it. It
            # comes up where it went down, and goes as keys, so after a commit it waits too.
            if self.typing.committing:
                self.typing.committing = False
                self.typing.at = now + TYPE_SWITCH
                return
            _, code, value = self.typing.queue.popleft()
            if value:
                if code in self.typing.kb_held:
                    return  # down already
                desktop = self.typing.kb_held[code] = now < self.desktop_until
            elif code in self.typing.kb_held:
                desktop = self.typing.kb_held.pop(code)
            else:
                return  # not down
            self.type_key(code, value, desktop)
        elif self.typing.queue and not self.typing_blocked(now):
            desktop = now < self.desktop_until
            keys = self.desktop_layout.keys if desktop else US_KEYS
            item = self.typing.queue[0]
            ch = item if isinstance(item, str) else item[1]
            # A character the desktop's layout has no key for goes to the focused text field
            # through the input method (ft-textinput), if there is one.
            commit = ch not in keys and desktop and self.text_input is not None and now < self.text_input_until
            if commit != self.typing.committing:
                self.typing.committing = commit
                self.typing.at = now + TYPE_SWITCH
                return
            self.typing.queue.popleft()
            if commit and self.commit_text(ch):
                self.typing.at = now + TYPE_STEP
                return
            if ch not in keys:
                self.typing.untyped.append(ch if isinstance(item, str) else None)
                self.report_untyped(desktop)
                return  # the next one right away
            code, mods = keys[ch]
            for m in mods:
                self.type_key(m, 1, desktop)
            self.type_key(code, 1, desktop)
            self.typing.down.append((code, mods, desktop))
            self.report_untyped(desktop)
        else:
            return
        self.typing.at = now + TYPE_STEP

    def typing_timeout(self, now, default):
        if self.typing.down or (self.typing.queue and (self.typing.queue[0][0] == "key" or not self.typing_blocked(now))):
            return max(0.0, min(default, self.typing.at - now))
        return default

    def restore_keymaps(self):
        """Give remapped devices their volume keys back, so they work without the relay."""
        for node in self.nodes.values():
            if node.remapped:
                try:
                    remap_volume(node.fd, restore=True)
                except OSError:
                    pass  # device already gone

    def take_volume(self, node):
        """Keep the node's volume keys from gamescope and SteamVR, which read every device.

        Returns False for a non-candidate node there's nothing to do with.
        """
        if not self.can_grab:
            return node.candidate
        try:
            found = remap_volume(node.fd)
        except OSError as e:
            log(f"remapping volume keys failed for {node.name}: {e}")
            # Some entries may have changed already: handle their stand-ins and restore them.
            node.remapped = True
            found = None
        if found:
            node.remapped = True
            log(f"{node.name} ({node.path}): volume keys taken over (remapped)")
            return True
        if found is None and node.only_volume:
            try:
                fcntl.ioctl(node.fd, EVIOCGRAB, 1)
                node.grabbed = True
                log(f"{node.name} ({node.path}): volume keys taken over (grabbed)")
                return True
            except OSError as e:
                log(f"grab failed for {node.name}: {e}")
        # Grabbed pointer devices still have their volume keys handled here.
        log(f"{node.name} ({node.path}): can't take over its volume keys")
        return node.candidate

    def role_of(self, node):
        return node_role(self.rules["devices"].get(node.id, {}), node, self.nodes.values())

    def release_held(self, node):
        for code in node.held:
            (self.mouse if code >= BTN_MISC else self.keyboard).emit(EV_KEY, code, 0)
        node.held.clear()
        self.mouse.sync()
        self.keyboard.sync()

    def physically_down(self):
        """The keys down on every device read here, as the kernel has them (EVIOCGKEY)."""
        down = set()
        for node in self.nodes.values():
            if node.network:
                down |= node.down
                continue
            buf = bytearray((KEY_MAX + 8) // 8)
            try:
                fcntl.ioctl(node.fd, EVIOCGKEY, buf)
            except OSError:
                continue
            down.update(i * 8 + bit for i, b in enumerate(buf) if b for bit in range(8) if b >> bit & 1)
        return down

    def reconcile_desktop_keys(self):
        """A key the desktop has down that no device holds comes up there, and the key
        combinations forget a Meta or modifier no device holds. A key can be left down when
        its device vanishes with it held (release_held only lets go of it here) or a release
        goes astray: on 2026-10-01, after a calibration, Meta stayed down in KWin, so typing
        opened the overview and clicks on the desktop did other things."""
        if not self.held.desktop and not self.held.meta and not self.held.modifiers:
            return
        down = self.physically_down()
        typed = {k for code, mods, desktop in self.typing.down if desktop for k in (code, *mods)}  # dictation's, coming up next
        typed |= {code for code, desktop in self.typing.kb_held.items() if desktop}  # Full Keyboard's
        for code in sorted(self.held.desktop - down - typed):
            self.to_screens(code, 0)
            log(f"key {code} released on the desktop: no keyboard holds it")
        self.held.forget_up(down)

    def release_all_keys(self):
        """release_keys: what this relay holds that no device does comes up, on the desktop and
        on its own virtual keyboard and mouse, combinations left down are forgotten, and
        ft-screens lets go of every key, on the desktop screens (every modifier in KWin too) and a
        remote display (its "keys release")."""
        self.reconcile_desktop_keys()
        down = self.physically_down()
        for code in [c for c in self.held.combos if c not in down]:
            del self.held.combos[code]  # its device went with it held: it kept modifier taps from working
        for node in self.nodes.values():
            for code in sorted(node.held - down):
                (self.mouse if code >= BTN_MISC else self.keyboard).emit(EV_KEY, code, 0)
                node.held.discard(code)
        self.mouse.sync()
        self.keyboard.sync()
        try:
            self.screens_sock.sendto(b"keys release", SCREENS)
        except OSError:
            pass  # the Frametop desktop isn't running
        log("release_keys: every key let go")

    def keys_down(self, node):
        if node.network:
            return bool(node.down)
        buf = bytearray((KEY_MAX + 8) // 8)
        try:
            fcntl.ioctl(node.fd, EVIOCGKEY, buf)
        except OSError:
            return False
        return any(buf)

    def apply_roles(self):
        """Grab pointer devices, and pass-through keyboards while typing goes to the desktop.

        A keyboard with a key down keeps its grab state until it's released, or the key
        would stay held on one side. Returns True if one is still waiting.
        """
        desktop = time.monotonic() < self.desktop_until
        waiting = False
        for node in self.nodes.values():
            if not node.candidate:
                continue  # volume keys only, taken over when found
            role = self.role_of(node)
            want_grab = self.can_grab and (role == "pointer"
                                      or (role == "passthrough" and node.is_keyboard and desktop))
            if node.network:
                want_grab = node.grabbed = False  # nothing else reads it
            if want_grab != node.grabbed and role == "passthrough" and self.keys_down(node):
                waiting = True
            elif want_grab != node.grabbed:
                try:
                    fcntl.ioctl(node.fd, EVIOCGRAB, 1 if want_grab else 0)
                    node.grabbed = want_grab
                except OSError as e:
                    log(f"{'grab' if want_grab else 'ungrab'} failed for {node.name}: {e}")
                if not node.grabbed:
                    self.release_held(node)
            if role != node.role:
                log(f"{node.name} ({node.path}, {node.id}): {role}{', grabbed' if node.grabbed else ''}")
                node.role = role
        if not waiting and desktop != self.typing_applied:
            self.typing_applied = desktop
            log(f"typing goes to {'the desktop (keyboards grabbed)' if desktop else 'Steam'}")
        return waiting

    def drop(self, node, reason):
        self.release_held(node)
        # A pointer-mode button the device held (a drag, a testbench killed mid-drag, or a release
        # read after the host's "hostbye") comes up in the helper too, unless another device holds
        # the same action.
        others = {action for n in self.nodes.values() if n is not node for action in n.actions.values()}
        for action in sorted(set(node.actions.values()) - others):
            self.do_action(action, 0, time.monotonic())
        node.actions.clear()
        if self.volume.held and self.volume.held[0] == node.fd:
            self.volume.release()
        os.close(node.fd)
        if node.wfd is not None:
            os.close(node.wfd)
        del self.nodes[node.fd]
        self.seen.pop(node.path, None)
        log(f"released {node.name} ({node.path}): {reason}")

    def host_message(self, cmd, words, now):
        path = f"host:{words[1]}:{words[2]}"
        node = next((n for n in self.nodes.values() if n.path == path), None)
        if cmd == "hostbye":
            if node:
                self.drop(node, "its host let go")
            return
        if node is None:
            if cmd != "hostdev":
                return  # events before a hello
            label = "".join(c for c in " ".join(words[3:]) if c.isprintable())[:60] or words[1]
            r, w = os.pipe2(os.O_NONBLOCK | os.O_CLOEXEC)
            mouse_kind = words[2] == "mouse"
            node = Node(path, r, f"{label} {'Mouse' if mouse_kind else 'Keyboard'}", BUS_NETWORK, 0, 0, path,
                        mouse_kind, not mouse_kind, network=True)
            node.wfd = w
            self.nodes[r] = node
            self.apply_roles()
        node.until = now + HOST_TIMEOUT
        if cmd != "hostev":
            return
        nums = words[3:]
        out = bytearray()
        keys = []
        for i in range(0, len(nums) - 2, 3):
            try:
                etype, code, value = int(nums[i]), int(nums[i + 1]), int(nums[i + 2])
            except ValueError:
                return
            if not ((etype == EV_SYN and code == SYN_REPORT) or (etype == EV_KEY and 0 < code <= KEY_MAX)
                    or (etype == EV_REL and 0 <= code <= REL_MAX)) or not -2**31 <= value < 2**31:
                continue
            if etype == EV_KEY:
                keys.append((code, value))
            out += EVENT.pack(0, 0, etype, code, value)
        try:
            os.write(node.wfd, out)
        except BlockingIOError:
            # A key-up may be among them: let go of every key it holds rather than keep one down.
            # Its next hello brings it back.
            self.drop(node, "events dropped, the relay is behind")
            return
        for code, value in keys:
            (node.down.add if value else node.down.discard)(code)

    def reply(self, addr, obj):
        try:
            self.control.sendto(json.dumps(obj).encode(), addr)
        except OSError:
            self.watchers.pop(addr, None)

    def kb_tell(self, now, message=None):
        """Tell Full Keyboard message, or with none, "kb DESKTOP SURFACE" if that changed: typing
        goes to the desktop 0|1, and the screens show with Frametop not paused 0|1 (it may show
        over them with the dashboard closed). From the control socket: it takes word from
        @frametop_relay only."""
        addr = self.kb.addr
        if addr is None:
            return
        if message is None:
            message = (f"kb {int(now < self.desktop_until)}"
                       f" {int(now < self.shown_until and not self.pause.paused)}")
            if message == self.kb.said:
                return
            self.kb.said = message
        try:
            self.control.sendto(message.encode(), addr)
        except OSError:
            pass  # gone: its hellos stop, and kb_gone follows

    def kb_gone(self, why):
        """Full Keyboard stopped or restarted: its keys still to come are dropped, and the keys
        it holds come up where they went down."""
        self.kb.addr, self.kb.said = None, None
        for code, desktop in self.typing.drop_full_keyboard():
            self.type_key(code, 0, desktop)
        log(f"Full Keyboard {why}")

    def handle_control(self, now):
        while True:
            try:
                data, anc, _, addr = self.control.recvmsg(65536, socket.CMSG_SPACE(CREDS.size))
            except BlockingIOError:
                self.kb_tell(now)  # what changed reaches Full Keyboard at once
                return
            pid, uid = peer_creds(anc)
            words = data.decode(errors="replace").split()
            cmd = words[0] if words else ""
            # One malformed datagram must not end the relay: it would drop every grab,
            # including the volume keys that keep gamescope from aborting. The control
            # socket is an abstract socket, so any local process can send to it.
            try:
                self.control_command(cmd, words, data, addr, pid, uid, now)
            except Exception as e:
                log(f"bad control datagram {data!r}: {e!r}")

    def control_command(self, cmd, words, data, addr, pid, uid, now):
        """One datagram on the control socket: cmd, its first word, of words; data as it came; addr,
        the sender's address (empty for an unbound one); pid and uid, the sender's, from the kernel."""
        if cmd == "keyboard":
            # From ft-screens (unbound, no reply): where typing goes, repeated every second.
            # "keyboard desktop|steam 1|0 1|0 1|0": the third word, whether the screens show;
            # the fourth, whether it's paused, the fifth whether the 3D mouse's laser is on a panel
            # so ft-screens takes its wheel (an older ft-screens leaves them out).
            desktop = len(words) > 1 and words[1] == "desktop" and not self.pause.paused
            self.desktop_until = now + 3.0 if desktop else 0.0
            self.shown_until = now + 3.0 if len(words) > 2 and words[2] == "1" else 0.0
            self.wheel_until = now + 3.0 if len(words) > 4 and words[4] == "1" else 0.0
            if uid == os.getuid():
                self.pause.screens_seen(pid)  # a desktop started while paused hides too
                if len(words) > 3 and words[3] in ("0", "1"):
                    self.pause.screens_report(words[3] == "1")
            return
        if cmd == "notap" and len(words) == 1:
            # From ft-screens (no reply): a laser clicked with a modifier held (Super+drag, say).
            # The relay never sees laser clicks, so the modifier's release isn't a tap. Nor is it
            # on the desktop, where KWin may not have had the click either (ft-screens carried the
            # window): with Meta held there, it gets an F24 press in between, as for a
            # combination, so letting go of Meta doesn't open Plasma's launcher. Not with another
            # key it holds (Shift): that already stops it, and a terminal would type F24.
            self.held.tap = None
            if self.held.meta - self.held.meta_hidden and not any(
                    c < BTN_MISC and c not in self.held.meta for c in self.held.desktop):
                self.to_screens(KEY_F24, 1)
                self.to_screens(KEY_F24, 0)
            return
        if cmd == "vrbtn" and len(words) == 3 and words[1] in VR_BUTTONS and words[2] in ("0", "1"):
            self.vr_button(words[1], int(words[2]), now)
            return
        if cmd == "vrhello":
            self.vr_bind(now)
            self.pause.helper_started()
            return
        if cmd == "vrgame" and len(words) == 2:
            if words[1] == "1":
                self.gamepad.stop("a VR game started")
            self.pause.game_state(words[1] == "1", now)
            return
        if cmd == "vrlink" and len(words) == 2:
            self.pc_game.link_state(words[1] == "1", now)
            return
        if cmd == "vrdash" and len(words) == 2:
            self.pause.dashboard_state(words[1] == "1")
            self.gamepad.dashboard_state(words[1] == "1")
            return
        if cmd == "gamepad" and len(words) == 2 and words[1] in ("desktop", "remote", "off"):
            # From ft-screens (no reply): SteamVR's Gamepad mode is Frametop's or ended. It
            # presses buttons, so only from this user.
            if uid != os.getuid():
                log(f"gamepad from another user ({uid}): ignored")
            else:
                self.gamepad.screens_word(None if words[1] == "off" else words[1], now, self.gamepad_blocked())
            return
        if cmd == "vrpad" and len(words) == 8:
            # From the controller reader in gamepad mode. It presses buttons, so only from
            # this user.
            if uid != os.getuid():
                log(f"vrpad from another user ({uid}): ignored")
            elif (pad_state := vrpad.parse(words[1:])) is not None:
                self.gamepad.reader_state(pad_state, now)
            return
        if cmd == "pause" and len(words) >= 2 and words[1] in ("on", "off", "toggle", "?"):
            # From input/ft-pause, the settings app, or the gesture reader (unbound, no reply).
            reason = words[2] if len(words) > 2 else "command"
            if words[1] == "toggle":
                self.pause.toggle(reason, now)
            elif words[1] != "?":
                self.pause.set(words[1] == "on", reason, now)
            if addr:
                self.reply(addr, self.pause.status())
            return
        if cmd == "action" and len(words) == 2:
            # From frunner (bound): one of Frametop's own actions, picked in its search or OS menu.
            # Only those (MENU_ACTION_NAMES), and only from this user: never run:, command: or keys:.
            name = words[1]
            if uid != os.getuid():
                self.reply(addr, {"t": "error", "error": "another user's"})
            elif name not in MENU_ACTION_NAMES:
                self.reply(addr, {"t": "error", "error": f"not a menu action {name!r}"})
            elif self.pause.paused and not works_paused(name):
                self.reply(addr, {"t": "error", "error": "paused"})
            else:
                self.do_action(name, 1, now, "frunner")
                self.do_action(name, 0, now, "frunner")
                self.reply(addr, {"t": "action", "action": name})
            return
        if cmd == "pause" and words[1:] in (["enforce"], ["defer", "remote"]):
            # From frametop-session.sh (input/ft-pause enforce|defer-remote) as a desktop
            # starts: while paused, its hand tracking stops again, and remote access waits
            # for the resume. Both stop things, so only from this user.
            if uid != os.getuid():
                log(f"pause {words[1]} from another user ({uid}): ignored")
            elif words[1] == "enforce":
                self.pause.desktop_started()
            else:
                self.pause.defer_remote()
            if addr:
                self.reply(addr, self.pause.status())
            return
        if cmd == "textfield" and words[1:] in (["1"], ["0"], ["1", "renew"]):
            # Typed characters go back to the address that said it, so only this user's. A
            # renewal keeps (or, after a restart here, finds) that address, and nothing else.
            if uid == os.getuid():
                self.text_input = addr if words[1] == "1" and addr else None
                self.text_input_until = now + TEXT_INPUT_LAPSE
            if words[2:] != ["renew"]:
                self.text_field(words[1] == "1")
            return
        if cmd in ("hostdev", "hostev", "hostbye"):
            # A host's keyboard or mouse, from its ft-stream (see "Host devices"). Keys and
            # clicks, so only from this user.
            if uid != os.getuid():
                log(f"{cmd} from another user ({uid}): ignored")
            elif len(words) >= 3 and HOST_ID.fullmatch(words[1]) and words[2] in ("mouse", "keyboard"):
                self.host_message(cmd, words, now)
            return
        if cmd == "type":
            # From Frametop Voice (unbound, no reply). It types keys, so only from this user.
            if uid == os.getuid():
                self.type_text(data[len(b"type "):].decode(errors="replace"))
            else:
                log(f"type from another user ({uid}): ignored")
            return
        if cmd == "kbhello":
            # Full Keyboard (keyboard/, --frametop), every second and when it opens or
            # closes: "kbhello OPEN". Only from this user and a bound address: what it
            # types goes to the desktop. It gets the word on every hello, and ft-screens
            # hears it's there ("fullkb OPEN"), so it opens Full Keyboard, not its own.
            if uid != os.getuid() or not addr:
                return
            if addr != self.kb.addr:
                if self.kb.addr is not None:
                    self.kb_gone("restarted")
                log("Full Keyboard connected")
            self.kb.addr, self.kb.until, self.kb.said = addr, now + 3.0, None
            self.kb_tell(now)
            opened = "1" if len(words) > 1 and words[1] == "1" else "0"
            try:
                self.screens_sock.sendto(f"fullkb {opened}".encode(), SCREENS)
            except OSError:
                pass  # ft-screens not running
            return
        if cmd in ("kbkey", "kbtext"):
            # Full Keyboard's typing, while it was told it goes to the desktop: "kbkey CODE
            # 0|1" (a key's press or release) and "kbtext TEXT" (what its input methods
            # composed). Only from the Full Keyboard that said hello. In order with what's
            # still to type, where typing goes then.
            if uid != os.getuid() or addr is None or addr != self.kb.addr:
                return
            if cmd == "kbtext":
                self.type_text(data[len(b"kbtext "):].decode(errors="replace"), quiet=True)
            elif (len(words) == 3 and words[1].isdigit() and 0 < int(words[1]) < BTN_MISC
                  and words[2] in ("0", "1") and len(self.typing.queue) < TYPE_MAX):
                self.typing.queue.append(("key", int(words[1]), int(words[2])))
            return
        if cmd == "kbpanel" and len(words) == 2 and words[1] in ("show", "hide"):
            # From ft-screens (unbound): open or close Full Keyboard, which it does for the
            # "vrkeyboard" commands while Full Keyboard is there.
            if uid == os.getuid():
                self.kb_tell(now, f"kb {words[1]}")
            return
        if cmd == "kbpoke":
            # From ft-screens (unbound): a fingertip on Full Keyboard's panel, "kbpoke
            # <left|right> <hover|press|move|up|end> [x y z]" (its point in the standing
            # universe), passed on as "kb poke ...". It presses keys, so only from this user.
            if (uid == os.getuid() and len(words) in (3, 6) and words[1] in ("left", "right")
                    and words[2] in KB_POKES and all(KB_COORD.fullmatch(w) for w in words[3:])):
                self.kb_tell(now, "kb poke " + " ".join(words[1:]))
            return
        if cmd == "calpanel" and len(words) == 2:
            # From the pointer helper (see the top). Only from this user: it takes Enter.
            if uid == os.getuid():
                self.calpanel_until = now + 3.0 if words[1] == "1" else 0.0
            return
        if cmd == "gazeawake" and len(words) == 2:
            if self.lease.current:
                self.lease.current.gaze_awake_until = now + 12.0 if words[1] == "1" else 0.0
            return
        if cmd == "gazewoke":
            # The gaze took the pointer back (gaze mode owns it): it's on, as a wake leaves it.
            p = self.lease.current
            if p and not p.active:
                p.active, p.last_used = True, now
                log("pointer on (gaze)")
            return
        if not addr:
            return  # unbound sender, nowhere to reply
        if cmd == "devices":
            self.reply(addr, {"t": "devices", "pointer_mode": self.lease.conf is not None,
                         "paused": self.pause.paused,
                         "actions": ACTIONS,
                         "nodes": [n.describe() for n in self.nodes.values() if n.candidate]})
        elif cmd == "watch":
            seconds = float(words[1]) if len(words) > 1 else 30
            self.watchers[addr] = now + min(seconds, 600)
            self.reply(addr, {"t": "watching", "seconds": seconds})
        elif cmd == "reload":
            self.load_config()
            self.apply_roles()
            if self.lease.current:
                self.lease.current.send("reload")
            self.vr_bind(now)
            self.reply(addr, {"t": "reloaded"})
        elif cmd == "vrcapture":
            seconds = float(words[1]) if len(words) > 1 else 30
            self.vr_capture_until = now + min(seconds, 120) if seconds > 0 else 0.0
            self.vr_bind(now)
            self.reply(addr, {"t": "vrcapture", "seconds": seconds})
        else:
            self.reply(addr, {"t": "error", "error": f"unknown command {cmd!r}"})

    def broadcast(self, node, etype, code, value, now):
        if not self.watchers or not node.candidate:
            return
        if etype == EV_REL and now - node.last_watch < 0.05:
            return  # motion: enough for an activity light
        node.last_watch = now
        msg = {"t": "event", "id": node.id, "path": node.path, "name": node.name, "role": node.role,
               "type": EV_NAMES.get(etype, str(etype)), "code": code, "value": value}
        for addr, until in list(self.watchers.items()):
            if now > until:
                del self.watchers[addr]
            else:
                self.reply(addr, msg)

    def recover_from_restart(self):
        """What a relay that starts does for what the one before it left behind, in one place: the
        restart fixes on the relay's side (docs/charts/README.md, G1 and G3). The pause's own
        (L5: the resume a restart cut short; L6: a pause that ends with its game)
        runs as GamePause reads its state file (game_pause.py, load), which has to come before the
        rules configure the pause."""
        if self.pause.paused and self.lease.conf:
            # L1: restarted while paused, the helper may still hold the pointer the last relay woke,
            # and this relay's new Pointer doesn't know, so it lets go now.
            self.lease.conf.stand_down()
        self.vr_bind(time.monotonic())  # a helper that's already running keeps its buttons in step
        # K3: a relay that went away with a key down left it down on the desktop, where this one
        # never sent it: modifiers and mouse buttons come up there now (a release of a key that
        # isn't down is nothing), and then ft-screens lets go of every key it holds, on the desktop
        # screens and a remote display.
        for code in sorted(MODIFIERS):
            self.to_screens(code, 0)
        for code in range(BTN_MOUSE, BTN_TASK + 1):
            self.to_screens(code, 0, button=True)
        try:
            self.screens_sock.sendto(b"keys release", SCREENS)
        except OSError:
            pass  # the Frametop desktop isn't running
        # K6: the same on its virtual keyboard and mouse, kept in the fd store.
        for dev in (self.mouse, self.keyboard):
            if dev.reused:
                dev.release_all()
        # K7: and on the pointer driver: a pointer-mode button held as it went never came up there.
        # The helper ignores the release of a button it doesn't have down.
        for driver in sorted(set(Pointer.DRIVER_BUTTONS.values())):
            try:
                self.screens_sock.sendto(f"btn {driver} 0".encode(), HELPER)
            except OSError:
                break  # helper not running (SteamVR not running)

    def run(self):
        """The loop: devices, the control socket and the timers."""
        self.pc_game.start()
        while True:
            now = time.monotonic()
            pointer = self.lease.current
            if now >= self.next_scan:
                self.scan(now)
            # The 3D mouse connecting or letting go moves a hand role, so the pause gesture's reader
            # looks the controllers up again (game_pause.py).
            if self.lease.holding_changed():
                self.pause.controllers_changed()
            ready, _, _ = select.select(list(self.nodes) + [self.control], [], [],
                                        self.typing_timeout(now, min(self.volume.timeout(now, pointer.timeout() if pointer else 0.5),
                                                                     self.pause.timeout(now), self.gamepad.timeout(now))))
            now = time.monotonic()
            self.tick(now, pointer)
            for fd in ready:
                if fd is self.control:
                    self.handle_control(now)
                elif fd in self.nodes:  # (not dropped earlier in this batch: a host's "hostbye")
                    self.read_node(self.nodes[fd], now)

    def scan(self, now):
        """Once a second: hosts gone quiet, keys no device holds, and event nodes come and gone."""
        self.next_scan = now + 1.0
        for node in [n for n in self.nodes.values() if n.network and now > n.until]:
            self.drop(node, "its host went quiet")
        self.reconcile_desktop_keys()
        current = {}
        for name in os.listdir("/dev/input"):
            if name.startswith("event"):
                try:
                    current[f"/dev/input/{name}"] = os.stat(f"/dev/input/{name}").st_ino
                except OSError:
                    pass
        for path in [p for p in self.seen if p not in current]:
            del self.seen[path]
        added = False
        for path, ino in sorted(current.items()):
            if self.seen.get(path) == ino:
                continue
            # New here: a new device, or one that came back in the same place.
            for old in [n for n in self.nodes.values() if n.path == path]:
                self.drop(old, "replaced by a new device node")
            # A new node is root's alone until udev gives it to the input group, a moment
            # after it appears. Opened in that gap, it would fail and never be tried
            # again: leave it for the next scan instead.
            if not os.access(path, os.R_OK):
                continue
            self.seen[path] = ino
            node = probe(path)
            if node and node.volume_keys and not self.take_volume(node):
                os.close(node.fd)
                node = None
            if node:
                self.nodes[node.fd] = node
                added = True
        if added:
            self.apply_roles()

    def tick(self, now, pointer):
        """The timers, after each wait: pointer is the 3D mouse in use as the wait began."""
        if pointer:
            pointer.tick(now)
        elif self.lease.conf:
            self.lease.conf.drain()  # paused: what waited still goes, in order
        if self.vr_bind_retry:
            self.vr_bind(now)
        self.volume.tick(now)
        self.typing_tick(now)
        if self.kb.addr is not None and now >= self.kb.until:
            self.kb_gone("stopped (no word for 3 seconds)")
        self.kb_tell(now)
        self.pause.tick(now)
        self.gamepad.tick(now)
        if self.vr_capture_until and now >= self.vr_capture_until:
            self.vr_bind(now)  # capture over: back to the mapped buttons
        if (now < self.desktop_until) != self.typing_applied or self.grab_waiting:
            self.grab_waiting = self.apply_roles()

    def read_node(self, node, now):
        """The events a device has ready."""
        pointer = self.lease.current  # (a pause or resume earlier in this batch changes it)
        fd = node.fd
        try:
            data = os.read(fd, EVENT.size * 64)
        except OSError as e:
            if e.errno == errno.EAGAIN:
                return
            self.drop(node, os.strerror(e.errno))
            return
        if not data:
            self.drop(node, "closed")
            return
        buttons = self.rules["buttons"].get(node.id, {})
        for off in range(0, len(data) - EVENT.size + 1, EVENT.size):
            _, _, etype, code, value = EVENT.unpack_from(data, off)
            if etype in (EV_KEY, EV_REL):
                self.broadcast(node, etype, VOLUME_ORIGINAL.get(code, code) if node.remapped else code,
                          value, now)
            if (etype == EV_KEY and code not in MODIFIERS) or (etype == EV_REL and code in SCROLLS):
                # A key or button going down or up (or repeating), or a scroll, in between: the
                # modifier's release isn't a tap. A key down before the modifier counts on its release.
                self.held.tap = None
            if (etype == EV_KEY and value == 1 and code not in MODIFIERS) or (etype == EV_REL and code in SCROLLS):
                self.cancel_talk()
            if etype == EV_KEY and ((node.remapped and code in VOLUME_ORIGINAL)
                                    or (node.grabbed and code in VOLUME_STANDIN)):
                self.volume.key(fd, code, value, now)
                continue
            if node.role == "volume":
                # A keyboard's media keys (its Consumer Control node) go to the desktop like a
                # pass-through keyboard's, and nowhere else: the node isn't grabbed, so gamescope
                # and SteamVR have them already. Not platform buttons: the headset's click button
                # is KEY_SELECT on gpio-keys (BUS_HOST). Nor a volume key a remap missed.
                if etype == EV_KEY and node.bus in (BUS_USB, BUS_BLUETOOTH) and code not in VOLUME_CODES:
                    self.to_screens(code, value)
                continue
            if node.role != "pointer":
                # Observed only, unless typing goes to the desktop. Key combinations work on
                # any pass-through keyboard.
                if node.role == "passthrough" and etype == EV_KEY and self.cal_enter(code, value, now):
                    continue
                if node.role == "passthrough" and etype == EV_KEY and code < BTN_MISC and self.key_binding(code, value, now):
                    continue
                if node.role == "passthrough" and etype == EV_KEY:
                    if value == 1 and code < BTN_MISC and now - self.last_typing >= 0.25:
                        self.last_typing = now
                        try:
                            self.screens_sock.sendto(b"typing", HELPER)
                        except OSError:
                            pass  # helper not running (SteamVR not running)
                    self.to_screens(code, value)
                    if node.network and code < BTN_MISC and (code in node.held or
                                                             (value == 1 and now >= self.desktop_until)):
                        # Nothing else reads a host's keyboard: Steam gets it from here.
                        self.keyboard.emit(etype, code, value)
                        self.keyboard.sync()
                        (node.held.add if value else node.held.discard)(code)
                continue
            if etype == EV_KEY and code < BTN_MISC and (str(code) not in buttons or code in self.held.combos) \
                    and self.rules["devices"].get(node.id, {}).get("role") == "combo" \
                    and self.key_binding(code, value, now):
                # A Keyboard + 3D pointer device that sends its keys on its mouse node (one node
                # for both): key combinations work on it as on a pass-through keyboard. A key held in
                # one still comes up here if the button map took it meanwhile.
                if value == 0 and code in node.held:
                    node.held.discard(code)  # it went down on the virtual keyboard too (Meta of Meta+J)
                    self.keyboard.emit(etype, code, value)
                continue
            if etype == EV_KEY:
                action = buttons.get(str(code), DEFAULT_BUTTONS.get(code, "key"))
                # A button that went down on the virtual devices (paused, POINTER=0, or as a
                # key) comes up there, even if pointer mode or its binding changed meanwhile.
                went_virtual = value != 1 and code in node.held
                if not went_virtual and (pointer or action == "pause_toggle") and action not in ("key", "none"):
                    if value == 1:
                        node.actions[code] = action
                    elif value == 0:
                        node.actions.pop(code, None)
                    self.do_action(action, value, now)
                    continue
                if action == "none" and not went_virtual:
                    continue
                target = self.mouse if code >= BTN_MISC else self.keyboard
                target.emit(etype, code, value)
                # A mouse button goes to the desktop only when pointer mode passes it through
                # as a key (a side button for Back there). The rest, like every click with
                # POINTER=0 or paused, reach the desktop through a laser, if at all. (A
                # key-mapped button still goes to the virtual mouse too.)
                self.to_screens(code, value, button=bool(pointer) and not went_virtual)
                if value:
                    node.held.add(code)
                else:
                    node.held.discard(code)
            elif etype == EV_REL:
                if pointer:
                    pointer.motion(code, value, now)
                else:
                    self.mouse.emit(etype, code, value)
            elif etype == EV_SYN and code == SYN_REPORT:
                if pointer:
                    pointer.flush(now)
                self.mouse.sync()
                self.keyboard.sync()

def main():
    can_grab = "--no-grab" not in sys.argv
    stored = stored_fds()
    mouse = Virtual(f"{VIRTUAL_PREFIX} mouse", 1,
                    keys=range(BTN_MISC, 0x118), rels=range(REL_MAX + 1), stored=stored)
    keyboard = Virtual(f"{VIRTUAL_PREFIX} keyboard", 2,
                       keys=range(1, BTN_MISC), rels=(), stored=stored)
    notify("READY=1")
    if can_grab:  # not under --no-grab: that's the tests (input/test), with the real home
        try:
            ft_commands.refresh()  # ft-cmd on the PATH, and the aliases, point at this install
        except OSError as e:
            log(f"commands: {e}")

    control = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    control.bind("\0frametop_relay")
    control.setblocking(False)
    # The kernel adds each sender's user ("type" is taken only from ours: an abstract socket
    # takes datagrams from anyone).
    control.setsockopt(socket.SOL_SOCKET, socket.SO_PASSCRED, 1)
    screens_sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM | socket.SOCK_NONBLOCK)

    relay = Relay(mouse, keyboard, control, screens_sock, can_grab, stored)
    atexit.register(relay.restore_keymaps)
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))  # so atexit runs on systemctl stop
    relay.recover_from_restart()
    relay.run()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
