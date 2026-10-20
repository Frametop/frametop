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

- A `CGEventTap` at the HID level takes key, mouse and scroll events while engaged. Motion comes from the events' delta fields. The cursor is held still so only Sunshine moves it: the tap sees a move after the cursor has moved, so the Bridge warps it back (`CGWarpMouseCursorPosition`, then `CGAssociateMouseAndMouseCursorPosition(true)` to end the warp's event suppression). Disassociating the cursor doesn't work from a background process. Volume, brightness and media keys (system-defined events, subtype 8) pass; the system-defined event every click also sends (subtype 7) is taken with it. Sunshine's own events are the ones with a nonzero source pid.
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

### Results so far (2026-10-20)

**S1 passed (offline).** Branch `host-input` (8d2de5e): host devices in the input relay, as under "On the Frame", with `input/test/hostin-test.py`. A fake host's hello makes a mouse (pointer) and a keyboard (pass-through), bus `network`. Its motion wakes and moves the 3D mouse, and its left button is the trigger. Its keys go to ft-screens while typing goes to the desktop, and out on the virtual keyboard while typing goes to Steam. A Meta tap on it opens the Steam menu (the default binding). With no word for 3 s both devices go away; a key it held on Steam comes up there, and one it held on the desktop comes up at the next scan (a held key isn't released while the host is still there). `hostbye` drops one device, and a bad id or kind does nothing. The other input tests still pass. Not done: ft-stream's side, and anything live.

**S2 (the test PC), without anyone there.** `spike.exe` (sources in `~/.cache/host-input-spike/win`, not in the repo) ran as SYSTEM in the console session, started the way sunshinesvc starts sunshine.exe (`consolerun.exe` from a SYSTEM scheduled task), with injected input standing in for the hardware:
- Hooks and Raw Input to a message-only window (`RIDEV_INPUTSINK`) both work from that context. The input desktop was `Default`.
- Injected events have `LLKHF_INJECTED` / `LLMHF_INJECTED`, and their Raw Input has no device handle, so either tells Sunshine's own input apart.
- A mouse event the hook takes doesn't move the cursor, but Raw Input still gets it: 40 of 40 moves, and the right button, wheel and horizontal wheel too. So the mouse can come wholly from Raw Input. A key the hook takes never reaches Raw Input; the hook has the key itself (vk, scan code, extended).
- Still to do, with someone at the PC: the same with the real mouse and keyboard (`--take-hw`), the lock screen and UAC's secure desktop, and a precision touchpad (the test PC has none).

**S3 (the Mac).** `spike` (C/Objective-C, `~/.cache/host-input-spike/mac`, built on the Mac with Xcode's clang). Over SSH (`sshd-session`) there is no Accessibility, so an active tap is refused, as expected. Run from Terminal with Accessibility granted:
- Hardware events have source pid 0. Events the spike posted from a `HIDSystemState` source carry its own pid, so the source pid tells Sunshine's input apart, not the source state.
- Dropping a move in the tap doesn't stop the cursor: macOS moves it first. `CGAssociateMouseAndMouseCursorPosition(false)` returned success but held nothing for a background process. Warping back on each taken move held it: every move afterwards was at the hold point, while the deltas kept coming (about 7,500 taken events in 30 s, clicks and keys taken, Secure Input off).
- Every click comes with a system-defined event (type 14, subtype 7). Media keys are subtype 8 (`data1` = key << 16 | 0xa00 down / 0xb00 up); they passed and worked.
- A mouse wheel sends line steps (`continuous=0`); the trackpad sends point deltas with a scroll phase, then momentum events (`momentum` 1, 2, 3). Trackpad gestures arrive as type 29 events.
- Still to do: whether a helper Sunshine starts works under Sunshine's Accessibility grant, which needs a Sunshine instance on and a stream.
