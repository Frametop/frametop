# Keyboard, mouse, pointer and display power

The testbench drives Frametop's desktop through Frametop's own control sockets, the way its
devices and tools do. Nothing here touches SteamVR's inputs: for controllers and the HMD, see
[controller inputs](controller-inputs.md).

These act on the live desktop and the headset's displays. Don't run them while someone else wears
the headset, and turn the displays back on (`power on`) if you turned them off.

```sh
./frame-testbench keyboard keys ctrl+l enter    # each combination pressed and let go in turn
./frame-testbench keyboard type -- 'hello world'   # typed where typing goes, as Frametop Voice types
./frame-testbench mouse move -200 40            # mouse counts, +x right, +y down
./frame-testbench mouse move 300 0 --drag left  # hold a button through the move
./frame-testbench mouse click                   # left; or right, middle, back, forward; --count 2
./frame-testbench mouse scroll -3               # wheel notches, + up; --horizontal for + right

./frame-testbench pointer state                 # head, gaze mode, worn or not
./frame-testbench pointer overlays              # SteamVR's overlays: key, name, visible
./frame-testbench pointer measure KEY           # a visible overlay's centre, size and axes
./frame-testbench pointer show|hide|recenter    # the 3D mouse's cursor

./frame-testbench power status                  # displays on, off or away; idle and timeout seconds
./frame-testbench power off|on
```

The MCP tools are `keyboard_keys`, `keyboard_type`, `mouse_move`, `mouse_click`, `mouse_scroll`,
`pointer_state`, `pointer_control`, `overlay_list`, `overlay_measure` and `display_power` (see
[MCP setup and tools](mcp.md)).

## How it works

- **Keyboard and mouse.** The testbench plugs a keyboard or a mouse into the input relay
  (`@frametop_relay`) for the length of a command, as a host device named `testbench` ("Host
  devices" in `input/input-relay.py`): `hostdev`, then evdev events as `hostev`, then `hostbye`.
  The relay treats it as one more device, with the same roles, mapped buttons and key bindings.
  In pointer mode the mouse moves the 3D mouse's cursor and clicks where it is; keys go where
  typing goes (the desktop panel clicked last, or Steam), and the relay's bindings apply, so
  `meta+space` opens frunner. If the testbench stops mid-command, the relay lets the device go
  3 s after its last word, its keys and buttons coming up.
- **Text.** `keyboard type` sends the relay's `type`, as Frametop Voice does: the desktop's
  keyboard layout, or a US one when typing goes to Steam. Printable characters only, up to
  2000 (the testbench's limit; the relay takes more); press `enter` with `keyboard keys`.
- **Pointer helper** (`@ft_pointer_helper`): `gaze ? headset`, `head`, `overlays` and
  `measure KEY` are read-only questions; `show`, `hide` and `recenter` change only the cursor.
  `pointer state` reports a missing head pose (SteamVR not tracking) as `head.error`.
  `measure` needs a head pose too, and finds only an overlay in view of the head: a hidden one,
  one behind, or the pointer's own cursor says "panel not visible".
- **ft-powerd** (`@ft_powerd`): `status` reads; `on` and `off` turn the headset displays on or off,
  as the Power tab does. `away` is SteamVR's standby.

All are abstract datagram sockets: nothing listens on the network. The relay takes keys, clicks
and text only from this user.

## Key names

Letters, digits (`0`-`9` are the digit keys), `enter`, `esc`, `tab`, `space`, `backspace`,
`delete`, `insert`, `up`, `down`, `left`, `right`, `home`, `end`, `pageup`, `pagedown`, `f1`-`f12`,
punctuation by evdev name (`minus`, `comma`, `dot`, `slash`...), the modifiers `ctrl`, `shift`,
`alt`, `altgr`, `meta` (or `super`, `win`) and their `left`/`right` forms, and `mute`,
`volumeup`, `volumedown`, `menu`. `KEY_` prefixes are fine. Any other key by its evdev code (10 up
to 767). A combination joins up to six keys with `+`: held in order, let go in reverse.

## Limits

- The relay doesn't answer keys, clicks or text, and the helper doesn't answer `show`, `hide`
  or `recenter`: their `ok` means sent. Check the result another way (`pointer state`, a
  capture).

- Typing goes wherever typing goes, which may not be what's in front of you: click the panel
  first.
- `mouse move` is in mouse counts, not pixels or degrees: in pointer mode the 3D mouse's speed
  settings apply.
- These need Frametop's services running: a command whose service isn't listening fails with
  "@NAME isn't listening".
