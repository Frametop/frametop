---
name: verify-frametop
description: Verify Frametop (the VR desktop for Steam Frame) on the live headset the way a user does, with the testbench faking the user (worn state, head pose, controllers, headset buttons, gaze, hands, keyboard and mouse) and reading back overlays, pointer state, display power, the gaze service and compositor captures. Use to prove a Frametop change works in VR before calling it done, or when asked to drive, check or screenshot Frametop on the Frame. Refuses while someone wears the headset.
---

# Verify Frametop

Frametop runs on one Steam Frame: the user's own headset, as their live VR desktop. There is no
second instance to start, and the user may put the headset on at any time. Verification drives
that one instance through the testbench (`testbench/`, see its README), which fakes the person
wearing it, and reads the result back through Frametop's own sockets and SteamVR captures.

All commands below run from the repo root, on the Frame. `V` is
`.claude/skills/verify-frametop/scripts/verify-frametop`, `T` is `testbench/frame-testbench`
(every `T` command prints one JSON object; the MCP server `frame-testbench` exposes the same
operations as tools), and `G` is `~/.local/share/frametop/releases/current/gaze/ft-gazectl`
(gaze mode on or off, as the Input Settings' Gaze page does).

## Hard rules

- Never drive anything while the headset is on someone's head. `V doctor` exits 2 and `V launch`
  refuses when the proximity sensor says worn (`physical_worn` in `T status`, which reads the
  real sensor even under a `worn on` override). During a run, chain every drive step after
  `V guard` (`V guard && T pose set ...`): it exits 0 while nobody wears the headset, and
  otherwise cleans up at once and exits 2 (worn) or 1 (sensor unread): stop there. `V snapshot`
  guards too. With the displays lit, the real sensor reads worn for under a second every few
  seconds while the headset lies still; on a worn reading `V guard` and `V doctor` read again
  for a few seconds, and count it as not worn only if two reads in a row say so and the headset
  hasn't moved (1 cm, 2 degrees). Never work around them: if they stop the run, check the
  headset is still before launching again.
- Never take a streamed host's keyboard and mouse. ft-stream leases a host's input while the
  headset is worn (docs/host-input.md), and a faked wearer counts: on 2026-10-30 a run took
  Daenon's laptop mid-sentence. `V launch` refuses while a remote display streams from a host with
  `"input": true` (doctor's "no host-input streams"; `T host-input` lists them), and so does
  `T worn on`. Otherwise `T worn on` starts a guard (user unit `frametop-fake-worn-guard`) that
  holds every such host's input lock while the wearer is faked, so a stream that starts later
  can't lease either; every `T` command releases a faked wearer whose guard has gone, and
  `V guard` stops the run with exit 3 when such a stream starts or the guard is gone. Don't work
  around it: no layout edits, no turning hosts' displays off, no stopping the guard to get a run
  through. `V launch NAME --allow-host-input` and `T worn on --allow-host-input` are only for a
  test the user allowed in this session, in their words; quote them in the report. A step that
  needs the wearer goes to HEADSET-TESTS.md instead.
- Never connect to a Vibepollo host unless the user allowed that host in this session: a stream
  can change the host's displays and plays its sound here. `V stream HOST` is the only way a run
  streams, and its viewer holds the host's input lock for the whole stream, so that stream can't
  lease the host's keyboard and mouse; `--allow-host-input` drops that, under the rule above.
- Every run ends with `V cleanup RUN` (the folder `V launch` printed), even after a failure: it
  turns the displays back on, runs `T release` (worn, pose, HMD buttons, controllers, fake eyes
  and hands) and `T compositor auto`, puts gaze mode back as the run found it, and frees the
  run.
- Don't restart SteamVR, gamescope, KWin or the Frametop desktop to make a check pass without
  telling the user. Restarting SteamVR with the testbench goes only through
  `testbench/install.sh install --restart` (it guards against the crash loop that makes SteamOS
  repair Steam or switch OS slots), and it takes the Frametop desktop down with it: run
  `~/.local/share/frametop/releases/current/desktops.sh start` after.
- Evidence can show the user's desktop, remote computers' screens and, through passthrough, the
  room. It stays in `testbench/artifacts/verify/` on this machine. Attach a capture to a PR only
  after the user has seen it and said yes.
- Never write to `/dev/shm/eye-server.mmap`, and never put camera images in fixtures or evidence.
- What the testbench can't fake goes in `~/Desktop/Projects/HEADSET-TESTS.md` as a short guided
  test for the user; keep working.

## Launch

1. Commit the change under test, then install the commit into the live release:
   `scripts/fast-install` (AGENTS.md, "Trying a change on the headset"). The run proves what
   runs, not what's in the checkout. It rebuilds changed components in the release's container,
   puts the changed files and programs in place, restarts the services using them (the desktop
   too, unless the headset is worn: `--worn-ok` once the wearer says yes), and records it all in
   the release's `.fast-install/` (`scripts/fast-install status`; `undo` restores the release).
   Exit 3 lists steps left: what an installer has to do (the `ft_pointer` driver:
   `pointer/driver/install.sh install`, then a SteamVR restart, ask first; units, menu entries,
   `/etc` files), restarts it left (the desktop while worn), and hand tracking held back by a
   changed ft-camd (that takes the full build); `scripts/fast-install done PATH` once one is
   done. Exit 1 on a change the image carries (the full build) or a failed build or restart.
   Don't swap live files by hand. Say in the report which commit is installed.
2. `V launch NAME`: runs the doctor, refuses on a worn headset, an unhealthy Frame or another
   run under way (a run is held for 30 minutes from its last launch, guard or snapshot), then
   makes `testbench/artifacts/verify/<date>-<time>-NAME/` with `doctor.json` and `baseline.json`
   (the readbacks below, before anything is driven). It prints the folder. If it refuses on
   overrides left by an earlier run that died, run `V cleanup` (`--force` if its lease still
   runs) and launch again.
3. Fake the wearer, for anything that needs the headset awake: `T worn on`, `T compositor awake`,
   and a head pose facing the panels, e.g. `T pose set --position 0 1.1 0 --rotation 0 0 0`
   (standing space, metres; rotation yaw/pitch/roll degrees, yaw 0 faces -z). Use
   `T pointer measure KEY` to find where a panel is, then aim at it. A test of the unworn
   behaviour skips this step.

Ready means `V launch` printed the run folder and, after step 3, `T pointer state` shows
`worn: true` and a `head` position. During a run, `V doctor` lists the run's own overrides as
left over and exits 1; that is expected until `V cleanup`.

## Doctor

`V doctor` changes nothing (its `T host-input` only releases a faked wearer whose guard has gone)
and prints JSON checks; exit 0 drive, 1 not ready, 2 worn (refuse):

- the headset isn't worn (proximity sensor, through the testbench proxy)
- `vrserver` and `vrcompositor` run
- the testbench proxy is loaded (`T status` `input.available` and `proximity_ready`); if not,
  the drop-in is missing or SteamVR started before it: `testbench/install.sh install --restart`
  with the headset off
- no testbench override is left over from an earlier run (worn, pose, HMD buttons,
  controllers, fake trackers)
- no remote display streams from a host that lends Frametop its keyboard and mouse (a faked
  wearer would take them); launch refuses on this one unless `--allow-host-input`
- Frametop's units (`frametop-desktop`, `-input-relay`, `-pointer`, `-gaze`, `-power`) are
  active and their sockets (`@frametop_relay`, `@ft_pointer_helper`, `@ft_powerd`, `@ft_gazed`)
  listen
- for information only: whether hand tracking (`frametop-hands`, opt-in) runs, which the hands
  recipes need; which release is live against this checkout's commit (a hint: it can't see
  binaries swapped by hand, and a published release never names a commit), with the commit
  `scripts/fast-install` put on top; and the run under way

Run it first whenever anything looks off.

`V ask NAME MESSAGE [--listen S]` sends one datagram to a Frametop socket and prints the reply
(with `--listen`, every reply for S seconds), for the services' own queries:
`V ask ft_gazed 'wake 90'`, `V ask ft_gazed status`, `V ask frametop_relay devices`.

## Drive

Drive the way the user would: through their inputs, never by writing Frametop's state files or
calling its internals. The feature map (`features/README.md`) has a recipe per feature. The
inputs:

| User does | Testbench |
| --- | --- |
| Puts the headset on or takes it off | `T worn on`, `T worn off`, `T worn physical` |
| Looks around, walks | `T pose set ...`, `T pose move --translation X Y Z --rotation YAW PITCH ROLL` |
| Holds and moves a controller | `T controller left\|right set --position X Y Z [--rotation ...]`, `move` |
| Presses a controller button | `T controller right button a on`, then `off` (a held button is one long press) |
| Pulls a trigger or grip, moves a stick | `T controller right trigger 1.0 --click on`, `grip`, `thumbstick X Y` |
| Presses a headset button | `T hmd-button /input/volume_up/click on`, then `off` |
| Looks with their eyes | `T gaze look YAW PITCH`, `T gaze eyes open\|closed\|lost` (x2) |
| Moves and pinches a hand | `T hand right set --position X Y Z --shape open\|point\|pinch\|fist` |
| Types or presses keys | `T keyboard keys ctrl+l enter`, `T keyboard type -- TEXT` |
| Moves, clicks, scrolls a mouse | `T mouse move DX DY [--drag left]`, `T mouse click [right] [--count 2]`, `T mouse scroll N` |
| Turns the displays off or on | `T power off`, `T power on` |

Details: `testbench/README.md`, `testbench/docs/controller-inputs.md`,
`testbench/docs/fake-trackers.md`, `testbench/docs/desktop.md`.

## Evidence

`V snapshot LABEL [--capture]` writes `RUN/LABEL/readbacks.json`: `T status` (overrides and the
real sensors), `T pointer state` (head pose, gaze mode, worn), the visible overlays
(`T pointer overlays`), `T power status`, the gaze service's status (`@ft_gazed`), and
`T host-input`: streams from hosts that lend their keyboard and mouse, the fake-worn flag, which
process holds each host's input lock (`$XDG_RUNTIME_DIR/frametop-input-HOST.lock`, read from
`/proc/locks`), and the last `hostinput:` lines of each stream's log
(`$XDG_RUNTIME_DIR/frametop-remote-N.log`, no timestamps; its `mtime` is the last write). The
host's own log (Vibepollo) isn't read: that needs the host. With `--capture`, also `RUN/LABEL/capture/` (`preview.png`, `stereo.png`, `result.json`): the
compositor's view, which needs `worn on` and a head pose facing the panels.

Proof standards:

- Snapshot before the action and after it, with labels in order (`1-before`, `2-pinched`...),
  so the evidence shows the change, not only an end state.
- Prove the effect where the user would see it (an overlay became visible, the capture shows
  it) and a side effect where there is one (the pointer helper's log line for a click,
  `journalctl --user -u frametop-pointer`, the gaze service's sample, the displays' state).
- Read a capture yourself (it's a PNG) before calling it proof: an empty or black frame means
  the compositor was idle or the pose faced away.
- `T inputs` (SteamVR action readback) is not a reliable readback here: with no scene app
  focused it reports `input_available: false` or inactive actions whatever the controllers do.
  Use the readbacks above.

## Cleanup

`V cleanup RUN` turns the displays back on (if the baseline had them on or away), releases
every override, sets the compositor back to auto, puts gaze mode back to how the baseline found
it, writes `RUN/cleanup.json`, frees the run, and reports any override still left (exit 1 if
so). It refuses a run under way that isn't the one named, unless `--force` (for a run whose
agent died). A run whose lease ran out needs no RUN and is restored from its own baseline.
With no run held, `V cleanup` only releases (no baseline: displays and gaze mode stay as they
are). `T compositor auto` sets SteamVR's persistent `pauseCompositorOnStandby` to true even if
the user had it false (the baseline's `testbench.runtime.settings` shows what it was). It never
deletes evidence, and it doesn't undo installed commits: `scripts/fast-install undo` does,
when the user shouldn't keep the change. Nor does it undo settings a
recipe changed through a service's socket (`V ask ft_screens 'cutouts on'`): the recipe says
how to put them back. `V doctor` afterwards should exit 0 with "no testbench overrides left".

## Risky paths

A diff touching these gets the extra reviewer (Codex) in the review panel:

- the installers and uninstaller: `get.sh`, `install.sh`, every `*/install.sh`, the `install`
  commands of `*/run.sh`, `uninstall.sh`, `testbench/install.sh`
- host privilege: anything using `sudo`, `setcap` or file capabilities (`hands/run.sh caps`,
  `gaze/tracker/install.sh`, `setup/bluetooth/`)
- files under `/etc` and system services
- the SteamVR driver and code loaded into vrserver: `pointer/driver/`, `testbench/native/`
  (the input proxy and loader)
- the release and image pipeline: `pack/`, `.github/workflows/`

## Helpers

- `scripts/verify-frametop`: `doctor`, `launch [NAME] [--allow-host-input]`, `guard`, `snapshot LABEL [--capture]`,
  `stream HOST [--display CLIENT] [--seconds N] [--allow-host-input] [--mic-bursts]`, `cleanup [RUN] [--force]`,
  `ask NAME MESSAGE [--listen S]`. `--help` prints the usage. Python 3 standard library only;
  runs on the SteamOS host.
- `scripts/fake-viewer`: ft-screens' side of one remote display, headless. `V stream` runs it in
  the Frametop container through the release's `scripts/in-box` (ft-stream's converter needs the
  container's Mesa: on the host it fails with EGL 0x3009) with the layout display's client, app
  and size, 30 fps and 20 Mbit/s. `RUN/stream-CLIENT-TIME/` gets `ft-stream.log`, `result.json`
  (states, frames, frames per second) and `summary.json`; `ok` means it went live and frames
  came. It refuses a display that's already streaming (Vibepollo ties a display to its client,
  so it would take the panel's) and a host whose input lock another stream holds.
- `scripts/mic-bursts` and `scripts/mic-latency`: the microphone's latency to a host.
  `V stream HOST --seconds 60 --mic-bursts` (the host needs `"mic": true`: for the test,
  `ft-layout remote host NAME mic=on`, back after) loads a pipe source, `frametop-verify-mic`, has
  ft-stream record it (`PULSE_SOURCE`), and once it records feeds it a 1 kHz burst each second,
  logging when each was due in `bursts.txt`. Not a null sink: SteamOS's policy sends a player
  through its spatializer to the speakers whatever sink it names, so a null sink's monitor stays
  silent (and the bursts play out loud). Meanwhile record the host's virtual microphone with
  wall-clock packet times, starting a few seconds before the stream:
  - Windows: `ffmpeg -f dshow -audio_buffer_size 20 -use_wallclock_as_timestamps 1 -t 85 -i
    "audio=Microphone (Steam Streaming Microphone)" -copyts -map 0:a -c:a copy mic.mkv`. The
    `-t` goes before `-i`: an output `-t` with `-copyts` ends the recording at once.
  - Linux (Vibepollo's `frametop_mic` source, untested): the same with `-f pulse -i frametop_mic`.

  Then `mic-latency bursts.txt mic.mkv --offset-ms MS`, MS the host's clock minus the Frame's: the
  lowest round trip of a few dozen `echo %time%` (Windows, 10 ms steps) or `date +%s.%N` over
  ssh. It prints the bursts matched and missed and the latency's median, spread and drift; the
  absolute value is only as good as MS (half the round trip). 2026-10-30, dody on the network route: 55 of
  55 bursts, 234 ms, no spread or drift over a minute (both ends run on sample clocks).
