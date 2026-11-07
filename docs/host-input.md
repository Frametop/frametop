# Host input: a computer's keyboard and mouse in Frametop (design notes)

Status: Linux hosts built (2026-10-27), with Frametop's Vibepollo build; see "Linux hosts". Windows and the Mac are planned, and the spikes below come first for them. Pausing the streams when the headset comes off isn't built yet: only the keyboard and mouse go back.

The idea: while you wear the headset and a computer's displays stream into Frametop ([remote displays](remote-displays.md)), that computer's own keyboard and mouse work as if they were paired to the Frame. The mouse drives the 3D mouse, and the keys type wherever typing goes in Frametop. Take the headset off and the computer gets its keyboard and mouse back, and its streams stop. Put it back on and the streams come back, and the keyboard and mouse go back to Frametop. It's what a Mac's keyboard and trackpad do for an Apple Vision Pro with Mac Virtual Display.

Nothing new is needed to control the computer itself: the 3D mouse on one of its panels already sends the pointer, clicks and keys back to it through the stream. So the host's mouse moves the 3D mouse, the 3D mouse is on the host's panel, and the host's cursor follows. Gaze mode, key bindings, drags across panels and Full Keyboard all work with the host's keyboard and mouse as they do with a Bluetooth one. With gaze mode on it's close to the Vision Pro: look at a panel, touch the trackpad, and the pointer is there.

## Decisions (2026-10-20)

| Question | Decision |
|---|---|
| The Mac | A small helper, Frametop Bridge, next to the stock Sunshine instances. Not a macOS build of the Vibepollo fork: its macOS code isn't built by Vibepollo's CI, our display role, microphone and display mode code is Windows and Linux only, and replacing LizardByte's notarized Sunshine with our unsigned build would break Screen Recording and Accessibility on every update |
| Headset off | The host gets its keyboard and mouse back and its streams stop at the same moment: when the pointer helper decides the headset is off (idle for `HEADSET_OFF_AFTER`, 1.5 s, or standby). No longer grace. Putting it back on waits for the streams, about 3-8 s |
| Volume and media keys | Never taken: they stay on the computer |
| A Mac keyboard's Cmd | Always Meta, wherever typing goes. No Cmd/Ctrl swap for the Frametop desktop |
| Signing the Mac Bridge | Ad hoc, built by CI. The macOS kit pins its SHA-256, and the setup clears its quarantine after checking it. Sunshine starts it, and it works under Sunshine's Accessibility grant (S3). No Apple Developer ID |

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
  - `events`: a batch ending in a sync, each `type` (key, motion, wheel), `code` (Linux evdev: `KEY_*`, `BTN_*`, `REL_*`), `value`. Keys are positional codes (a scan code on Windows, a `kVK_*` on the Mac, mapped to evdev on the host), so the Frametop desktop's keyboard layout decides which character a key types, as it does for a Bluetooth keyboard. Motion is the mouse's raw counts, before the host's pointer acceleration (a Linux host's touchpad is the exception: libinput's accelerated motion, as a touchpad needs); the relay's sensitivity applies as for any mouse. The wheel is in 120ths of a notch (`REL_WHEEL_HI_RES`, `REL_HWHEEL_HI_RES`), which carries a trackpad's smooth scrolling.
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
- The host hasn't released it with its escape hotkey since the headset last went on (the Frame keeps leasing; the host answers `released escape`).
- No test is faking the wearer or holding a verification run, unless it was allowed to take a host's input. The pointer helper can't tell the testbench's `worn on` from a real head, so the Frame drops the lease while `$XDG_RUNTIME_DIR/frametop-fake-worn.json` exists, or `$XDG_RUNTIME_DIR/verify-frametop.json` (a `verify-frametop launch` run, held while its mtime is at most 30 minutes old) does, without `"allow_host_input": true`. A garbled one counts as not allowed (below).

## Tests and a faked wearer

The pointer helper can't tell the testbench's `worn on` from a real head, so a test that fakes the wearer while a host that lends its input streams takes that computer's keyboard and mouse from whoever sits at it (2026-10-30: oma7, mid-sentence, from a run that never meant to touch it). Leases need the user's say-so for tests:

- `frame-testbench worn on` refuses while such a stream is live, unless given `--allow-host-input` (the CLI only; the MCP `worn` tool has no override). Otherwise it first starts a guard, `python3 -m frame_testbench.hostinput guard` in its own user unit (`frametop-fake-worn-guard`), which holds the input lock (`$XDG_RUNTIME_DIR/frametop-input-HOST.lock`, below) of every host with `"input": true`, so no stream, live or started later, can lease: ft-stream leases only from the stream holding that lock. A host given `"input": true` meanwhile has its lock taken too; if a stream holds it already, the guard releases the faked wearer, and ft-stream, reading the flag below, sends that stream no lease meanwhile. `worn off|physical` and `release` end the guard. If the guard ends any other way (stopped, killed), it or its unit's `ExecStopPost` releases the wearer but leaves the flag, and the next testbench command releases the wearer again and clears it. Until then no stream leases, even with nothing faked: the guard can be stopped after `worn on` saw it live and before it fakes the wearer, which the flag still covers. `frame-testbench host-input` (MCP `host_input`) lists the live streams from such hosts, the flag below and whether a guard holds it, which process holds each input lock, and ft-stream's last `hostinput:` log lines.
- `verify-frametop launch` refuses the same way unless `--allow-host-input`, and `verify-frametop guard` stops the run (exit 3) when such a stream starts mid-run or a faked wearer has lost its guard. `verify-frametop stream` runs ft-stream with a viewer that holds the host's input lock, passed down to ft-stream, which doesn't know it has it, so the lock lasts while either runs.
- While the testbench fakes the wearer it keeps `$XDG_RUNTIME_DIR/frametop-fake-worn.json`, `{"since": UNIX TIME, "pid": PID, "allow_host_input": BOOL}`, PID the guard's (the CLI's when allowed), written before the wearer is faked and removed by `worn off|physical`, `release`, or the first testbench command after its guard has gone. A verification run is `$XDG_RUNTIME_DIR/verify-frametop.json`, `{"run": FOLDER, "started": UNIX TIME, "allow_host_input": BOOL}`, held while its mtime is under 30 minutes old. `$XDG_RUNTIME_DIR` is the session's, `/run/user/UID`, never a nested desktop's. ft-stream reads both files on every tick of its lease loop, in the session's runtime dir and, under a nested desktop, `/run/user/UID` too. While either exists without `"allow_host_input": true` (one that is garbled, too big or not a plain file counts as not allowed) it sends no lease, and releases one it holds at once. The locks and the flag back each other up: a guard killed outright frees its locks before anything releases the wearer, but its flag stays, so no stream leases in that gap.

## Headset off and on

The ft-stream holding a host's input lock asks the pointer helper's headset state itself, every second (see "On the Frame"). When it goes to away:

1. The Frame stops the leases (`release` at once). The host gives its keyboard and mouse back within one lease of the change. Keys and buttons held at that moment come up on the side they went down on: a Ctrl held on the Frame comes up on the Frame. That release lifts an escape too. When no stream sees the headset come off (none was running, or it was reconnecting), the next one sends the release before its first lease, from the pointer helper's word on when the headset last went back on.
2. Planned, not built: ft-screens stops every remote display's stream. With our fork, that hands the monitor back its own resolution and HDR, and frees the host's encoder and the Frame's decoder. The panels would stay where they are, with their last picture and a "Paused" label, not destroyed: destroying overlays while SteamVR goes to standby has crashed vrcompositor before. Until then the streams keep running with the headset off.

When it's worn again, the host's input goes back to Frametop at the helper's next "worn" answer, once the stream holding the lock is live. Until then the keyboard and mouse stay on the computer.

## Safety

- **A dead man's lease.** The host releases its input 1.5 s after the last lease: Frametop stopping or crashing, the Wi-Fi dropping, the Frame sleeping, a stream ending.
- **An escape hotkey on the host** (to pick, for example both Shifts with Escape) releases at once, and keeps it released until the headset comes off and goes on again (off for 1.5 s or more, as the pointer helper counts it, whether or not a stream was running), or the switch in Remote Displays goes off and on. A Wi-Fi drop, a stream starting over or a pause keeps it. On Windows, Ctrl+Alt+Del always reaches Windows: nothing can hook it.
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
- Sunshine starts it (a global prep command), and it then works under Sunshine's Accessibility grant (S3), so it needs none of its own.

On the Frame, the ft-stream holding a Mac's input lock connects to its Bridge with the display's client key.

## Linux hosts

Built first (2026-10-27), for a Hyprland host (oma7). Wayland gives no global input capture, and Vibepollo's packaged service runs as its own user without access to `/dev/input`, so the fork adds a capture service of its own, `vibepollo-host-input` (`packaging/linux/vibepollo-host-input.c` in frametop-vibepollo):

- Off until the computer's owner turns it on: `sudo systemctl enable --now vibepollo-host-input.socket`. Until then the Frame is told `released off`.
- Socket-activated (`/run/vibepollo/host-input.sock`, group `vibepollo`), serving only the machine host's uid. A dynamic user in the `input` and `vibepollo-uinput` groups, with no capabilities, no network, and only input devices and `/dev/uinput` allowed.
- While a lease holds it grabs (`EVIOCGRAB`) each keyboard (it has letter keys) and mouse (relative X/Y and a left button), Bluetooth ones too, so the desktop sees nothing from them. A touchpad (multitouch with a finger tool, not a touchscreen) is taken through libinput, which makes it a mouse here: its accelerated motion, its buttons with tap to click, and two-finger scrolling as `REL_WHEEL_HI_RES` plus whole `REL_WHEEL` notches (pointer mode scrolls by notches). Its settings: `VIBEPOLLO_HOST_INPUT_TOUCHPAD` (`tap`, `natural`, `click`, `scroll`), Hyprland's defaults unless set. Never a uinput device (under `/devices/virtual/input`, which includes Vibepollo's own passthrough devices) or a touchscreen. It looks for new devices every second, from sysfs: opening and closing an event node can block for tens of milliseconds, which once made a 1000 Hz mouse overflow its kernel buffer every second or so.
- A device is grabbed only once no key is held on it, so a key that went down on the computer comes up there. Keys and buttons that went to the Frame come up there when the grab ends.
- Volume, media and brightness keys stay on the computer: it types them again on a uinput device of its own.
- Escape: both Shifts and Escape on the computer (the Shifts may be on different keyboards). It refuses leases (`released escape`) until the Frame sends `release`, which it does when the headset comes off or the switch goes off, even if it had stopped leasing. Anything else that stops the leases (a pause, the helper or relay going quiet, the stream ending) sends `drop`, which keeps the escape. The host keeps one escape for every client, and a session's end or a lease running out keep it too, so the Frame keeps it across streams as well: the stream that hears it writes `$XDG_RUNTIME_DIR/frametop-escape-ID`, and a release that goes removes it. The next stream to hold the host's input lock reads it and asks the pointer helper when the headset last went back on (`gaze ? headset on-at`, CLOCK_BOOTTIME): an off and on since the escape owes a release, which goes before its first engage. A helper that doesn't say, or restarted since, says nothing, and the escape stands.
- The escape is kept by Vibepollo too, so a capture service that restarts doesn't hand the input straight back; restarting Vibepollo itself forgets it. It guards against a client that plays by the rules: a client allowed keyboard and mouse can send `release` itself, and such a client already drives this computer's desktop.
- States and `done` reach Vibepollo in order even when it reads slowly; events may be lost then, but the lease ends (`stalled`) and its released state follows them.
- When the kernel drops a device's events (`SYN_DROPPED`), it catches up from the device's key state (`EVIOCGKEY`): keys that came up meanwhile come up on the Frame, ones that went down stay on the computer, and the device stays taken. (`dropped` is still a reason in the protocol, from older hosts.)
- It lets go 1.5 s after the last lease, when the stream ends, when the client loses its mouse or keyboard permission, when the client's control stream falls behind (`overflow`), when Vibepollo stops reading from it (`stalled`), or when Vibepollo stops (`closed`).

The fork's host side (`src/frametop_hostin*.cpp`) gives the lease to one client at a time (another gets `released busy`), only on an encrypted control stream and only to a client allowed both mouse and keyboard. Windows and macOS hosts answer `released unsupported`.

The protocol as built: `0x3005` is version 1, then engage (1) with a u32 sequence and a u16 lease in ms, release (2) or drop (3). `0x3004` is version 1, then one record: events (1: kind, 1 keyboard or 2 mouse, a u16 count, then evdev type u16, code u16, value s32 each, little-endian; a report ends with 0 0 0; a mouse record carries `BTN_MOUSE` to `BTN_TASK` and relative motion, a keyboard record the other keys), state (2: engaged 0 or 1, then the reason), or hello (3: kinds, then the host's name). State and hello come on every change and once a second. Reasons: `lease`, `host`, `escape`, `busy`, `permission`, `overflow`, `dropped`, `stalled`, `closed`, `off`, `service`, `unsupported`.

On the Frame, ft-stream does what "On the Frame" says without ft-screens: the stream holding the host's input lock asks the pointer helper (`gaze ? headset on-at`) and the relay (`pause ?`) every second itself, taking answers only from this user's processes, and a helper or relay that stops answering counts as a drop. An answer counts from when it was given, the kernel's send time on the datagram (`SO_TIMESTAMPNS`, `GivenAt`), not from when ft-stream read it: one left waiting while the switch was off is stale when it comes back on. It waits up to 100 ms for room to send the relay a host message; one it couldn't send says `hostbye`, so the relay lets go of the host's keys rather than miss a key-up, and the next hello brings the devices back. A `hostbye` that couldn't go either is owed, and that kind gets no `hostdev` until it has gone, so the relay's own 3 s timeout lets go meanwhile. These sends run on moonlight's control receive thread, so a stuck relay slows the control stream by up to 100 ms per message. A release goes out on each fresh switch-off or headset-off, leased or not, and for an escape in the marker that the headset came off and went on since (see the escape above), so a new ft-stream also lifts an escape it never saw. Before each decision it reads the tests' files in `$XDG_RUNTIME_DIR` (and in `/run/user/UID` when that is a nested desktop's), and one that forbids drops the lease at once and logs `hostinput: no lease for HOST's keyboard and mouse while FILE says a test may not take them`; the testbench's input-lock guard alone left a short gap when a host got the switch while its stream held the lock. `stream/hostinput.h` has the records, when to hold, drop or release (`Decide`), the kept escape (`EscapeMark`, `OnAt`, `OffOnSince`), the tests' files (`TestForbidding`) and the relay's messages (`Relay`), offline-tested by `stream/test/hostinput-test.cpp`. moonlight-common-c gets `LiSendFrametopHostInputLease` and `LiSetFrametopHostInputCallback` from `stream/moonlight-hostin.patch` (channel 0x08 for leases; the callback only sees packets that came encrypted).

## On the Frame

- **ft-stream:** the per-host input lock (`flock` on `$XDG_RUNTIME_DIR/frametop-input-HOST.lock`, like audio). The holder checks the engage conditions itself (the switch, the pointer helper, the relay, the tests' files), sends the leases, and passes the events on to the relay as datagrams. For a Mac it would also keep the Bridge connection.
- **Input relay:** a network device per host and kind, made from those datagrams (`probe()` takes only USB and Bluetooth nodes, and a uinput keyboard would also be read by gamescope). Each is a node fed through a pipe, so the event loop doesn't change. Roles, mapped buttons and key bindings work as for any device, and Frametop Settings lists them. Differences from a real node: no grab (nothing else sees its events), so while typing goes to Steam its keys go out on the relay's virtual keyboard; its held keys are tracked here (there's no `EVIOCGKEY`); and when its hellos stop for 3 s it goes away like a device unplugged, its held keys coming up.
- **ft-screens:** starts and stops the streams; planned, the headset pause and the paused panels above. It sends ft-stream no input commands.
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
- A helper Sunshine starts works under Sunshine's Accessibility grant. With a temporary `global_prep_cmd` on one instance and a stream from the Frame, the prep command's process (and a child it left running) had Sunshine as its responsible process, `AXIsProcessTrusted` 1, and an active tap was made. Sunshine.app is responsible for itself even though the LaunchAgent's bash starts it. So the Bridge needs no grant of its own, and an update of it (a new ad hoc signature) asks for nothing.
- Sunshine's own input, seen from there: moves with Sunshine's pid as the source pid, source state 1 (HIDSystemState).
