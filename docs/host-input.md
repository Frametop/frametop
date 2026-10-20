# Host input: a computer's keyboard and mouse in Frametop (design notes)

Status: planned (2026-10-20), nothing built yet. The spikes below come first.

The idea: while you wear the headset and a computer's displays stream into Frametop ([remote displays](remote-displays.md)), that computer's own keyboard and mouse work as if they were paired to the Frame. The mouse drives the 3D mouse, and the keys type wherever typing goes in Frametop. Take the headset off and the computer gets its keyboard and mouse back, and its streams stop. Put it back on and the streams come back, and the keyboard and mouse go back to Frametop. It's what a Mac's keyboard and trackpad do for an Apple Vision Pro with Mac Virtual Display.

Nothing new is needed to control the computer itself: the 3D mouse on one of its panels already sends the pointer, clicks and keys back to it through the stream. So the host's mouse moves the 3D mouse, the 3D mouse is on the host's panel, and the host's cursor follows. Gaze mode, key bindings, drags across panels and Full Keyboard all work with the host's keyboard and mouse as they do with a Bluetooth one. With gaze mode on it's close to the Vision Pro: look at a panel, touch the trackpad, and the pointer is there.

## Decisions (2026-10-20)

| Question | Decision |
|---|---|
| The Mac | A small helper, Frametop Bridge, next to the stock Sunshine instances. Not a macOS build of the Vibepollo fork: its macOS code isn't built by Vibepollo's CI, our display role, microphone and display mode code is Windows and Linux only, and replacing LizardByte's notarized Sunshine with our unsigned build would break Screen Recording and Accessibility on every update |
| Headset off | The host gets its keyboard and mouse back and its streams stop at the same moment: when the pointer helper decides the headset is off (idle for `HEADSET_OFF_AFTER`, 1.5 s, or standby). No longer grace. Putting it back on waits for the streams, about 3-8 s |
| Volume and media keys | Never taken: they stay on the computer |
| A Mac keyboard's Cmd | Always Meta, wherever typing goes. No Cmd/Ctrl swap for the Frametop desktop |
| Signing the Mac Bridge | Ad hoc, built by CI. The macOS kit pins its SHA-256, and the setup clears its quarantine after checking it. Sunshine starts it (see S3 for whether it then works under Sunshine's Accessibility grant). No Apple Developer ID |

## How it fits

```
the host's keyboard and mouse
  Windows: our Vibepollo fork (low-level hooks take them, Raw Input gives the mouse's counts)
  macOS:   Frametop Bridge (a CGEventTap), one per Mac
      │  events: evdev key codes, relative motion, buttons, high-resolution wheel
      │  ◄── an engage lease from the Frame every 500 ms
      ▼
ft-stream: the host's stream that holds its input lock (one per host, like the audio lock)
      │  datagrams to the input relay
      ▼
input relay: a "network device" per host and kind (mouse: pointer, keyboard: pass-through)
      → the 3D mouse, mapped buttons, key bindings, typing to the desktop, a remote panel, or Steam
```

## The protocol

Both transports carry the same messages. On the fork they're control stream packets, which on a Sunshine host are encrypted (the fork refuses them otherwise, like the microphone's `0x3003`). On the Bridge they're length-prefixed records on its TLS connection.

- **Host → Frame, `0x3004` (Frametop host input).** A version byte, then records:
  - `events`: a batch ending in a sync, each `type` (key, motion, wheel), `code` (Linux evdev: `KEY_*`, `BTN_*`, `REL_*`), `value`. Keys are positional codes (a scan code on Windows, a `kVK_*` on the Mac, mapped to evdev on the host), so the Frametop desktop's keyboard layout decides which character a key types, as it does for a Bluetooth keyboard. Motion is the mouse's raw counts, before the host's pointer acceleration; the relay's sensitivity applies as for any mouse. The wheel is in 120ths of a notch (`REL_WHEEL_HI_RES`, `REL_HWHEEL_HI_RES`), which carries a trackpad's smooth scrolling.
  - `state engaged|released REASON`: whenever it changes, and once a second. Reasons: `lease` (none, or it ran out), `escape` (the hotkey on the host), `secure` (macOS Secure Input, the Windows secure desktop or lock screen), `permission` (no Accessibility on the Mac), `off` (switched off on the host).
  - `hello`: the host's kinds (keyboard, mouse, trackpad) and its name, at the start.
- **Frame → host, `0x3005` (engage lease).** `engage SEQ MS` every 500 ms while every condition below holds, with MS = 1500. `release` when one stops holding. The host takes input only while the last lease is fresh.

The host's own input is never sent back: Sunshine's injected events (the Frame's clicks and keys on the host's panels) have the `INJECTED` flags on Windows, and on the Mac they're told apart by the posting process (Vibepollo's macOS input posts mouse events from a `HIDSystemState` source, so the source state can't tell them from the hardware's).

## When the host's input goes to Frametop

The Frame sends leases for a host while all of these hold:

- The host's "Use this computer's keyboard and mouse" switch is on in Remote Displays (`"input": true` on its layout entry, `ft-layout remote host NAME input=on|off`).
- One of its displays streams (live), and that ft-stream holds the host's input lock.
- The headset is worn (the pointer helper's headset state).
- Frametop isn't paused for a VR game. A Steam Link VR session from the same PC needs that PC's own keyboard and mouse; a game on the Frame has the Frame's.
- The host hasn't released it with its escape hotkey since the headset last went on.

## Headset off and on

ft-screens asks the pointer helper's headset state, as gaze idling does. When it goes to away:

1. The Frame stops the leases (`release` at once). The host gives its keyboard and mouse back within one lease of the change. Keys and buttons held at that moment come up on the side they went down on: a Ctrl held on the Frame comes up on the Frame.
2. ft-screens stops every remote display's stream. With our fork, that hands the monitor back its own resolution and HDR, and frees the host's encoder and the Frame's decoder. The panels stay where they are, with their last picture and a "Paused" label. ft-screens doesn't destroy them: destroying overlays while SteamVR goes to standby has crashed vrcompositor before.

When it's worn again, the streams start, and the host's input goes to Frametop once the first of its streams is live. Until then the keyboard and mouse stay on the computer.

## Safety

- **A dead man's lease.** The host releases its input 1.5 s after the last lease: Frametop stopping or crashing, the Wi-Fi dropping, the Frame sleeping, a stream ending.
- **An escape hotkey on the host** (to pick, for example both Shifts with Escape) releases at once, and keeps it released until the headset comes off and goes on again, or the switch in Remote Displays goes off and on. On Windows, Ctrl+Alt+Del always reaches Windows: nothing can hook it.
- **Held keys** come up where they went down, on engage and release both.
- **Never more than one Frame** takes a host's input at a time; a second gets `released busy`.
- **The host says why** its input stays: Secure Input, the lock screen or UAC's secure desktop, a missing permission. Remote Displays shows it under the host.

## Windows: in the fork

A capture thread in `sunshine.exe` (the service starts it in the console session as SYSTEM, `tools/sunshinesvc.cpp`), alive only while a Frametop client with mouse and keyboard permission holds an engage lease:

- `WH_KEYBOARD_LL` and `WH_MOUSE_LL` hooks take the hardware's events (returning 1) and skip injected ones. Volume and media keys pass.
- Raw Input (`RIDEV_INPUTSINK`) gives the mouse's counts, since a low-level hook only sees the pointer's accelerated, clamped position.
- The hooks belong to one desktop, so the thread follows the input desktop as `syncThreadDesktop` does for injection. On the secure desktop and the lock screen input stays on the PC (`released secure`).
- A setting in `sunshine.conf` turns it off on that PC.

## macOS: Frametop Bridge

One small helper per Mac (Swift, built by the Frametop CI as a universal binary, ad hoc signed, pinned in the macOS kit):

- A `CGEventTap` at the HID level takes key, mouse and scroll events while engaged. Motion comes from the events' delta fields; the cursor is held still (`CGAssociateMouseAndMouseCursorPosition`) so only Sunshine moves it. Volume and media keys (system-defined events) pass.
- It listens with TLS on a port of its own, serving the Sunshine instances' shared certificate, which Frametop already pins per Mac. It takes only client certificates the instances have paired (it reads their state files; it runs as the same user). No new pairing.
- Frametop Streams in the menu bar shows "Keyboard and mouse: in Frametop" or "here", with Take Back Now and an off switch.
- Sunshine starts it (a global prep command), so with luck it works under Sunshine's Accessibility grant. If not, it needs its own, granted again after each update of the Bridge (its ad hoc signature changes).

On the Frame, the ft-stream holding a Mac's input lock connects to its Bridge with the display's client key.

## Linux hosts

Later, with the testers who run Linux hosts. Vibepollo's packaged service runs as its own user, and Wayland gives no global input capture; the fork's root session broker could grab the evdev devices (whose codes need no mapping).

## On the Frame

- **ft-stream:** the per-host input lock (`flock` on `$XDG_RUNTIME_DIR/frametop-input-HOST.lock`, like audio). The holder sends leases while ft-screens says `input engage`, and passes the events on to the relay as datagrams. For a Mac it also keeps the Bridge connection.
- **Input relay:** a network device per host and kind, made from those datagrams (`probe()` takes only USB and Bluetooth nodes, and a uinput keyboard would also be read by gamescope). Each is a node fed through a pipe, so the event loop doesn't change. Roles, mapped buttons and key bindings work as for any device, and Input Settings lists them. Differences from a real node: no grab (nothing else sees its events), so while typing goes to Steam its keys go out on the relay's virtual keyboard; its held keys are tracked here (there's no `EVIOCGKEY`); and when its hellos stop for 3 s it goes away like a device unplugged, its held keys coming up.
- **ft-screens:** the engage conditions and the headset pause above; `input engage|release` to the lock holder; the paused panels.
- **Remote Displays:** the switch and the host's state under each computer.

## Spikes

| | What | Learns |
|---|---|---|
| S1 (Frame) | The relay's network devices, fed by a fake host in an offline test like `input/test/keys-test.py` | The 3D mouse, typing to the desktop and to Steam, held keys at engage and release, a device that stops saying hello |
| S2 (the test PC) | A standalone capture test, run the way `sunshine.exe` runs (SYSTEM, console session), and as the signed-in user | Hooks from that context; whether Raw Input still arrives for events a hook takes; that injected events are told apart; the cursor staying still while taken |
| S3 (the Mac) | A standalone event tap | Taking moves without the cursor drifting; trackpad scrolling and momentum; telling Sunshine's posted events apart; Secure Input; whether a helper Sunshine starts works under Sunshine's Accessibility grant |
