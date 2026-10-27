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

1. Deploy the change under test into the live release, if the change is in Frametop's own
   components (the run proves what runs, not what's in the checkout). `V doctor` shows the live
   release (`~/.local/share/frametop/releases/current`) next to this checkout's commit.
   - A component binary or script: build it (in the `dev` container, see AGENTS.md), copy it
     next to the live one as `NAME.new`, keep the old one as `NAME.pre-<topic>`, `mv` the new one
     into place, and restart only its unit (`systemctl --user restart frametop-<unit>.service`).
   - The `ft_pointer` SteamVR driver: never `cp` over the loaded `.so` (vrserver crashes); stop
     SteamVR first or `mv` the new file into place, and ask before restarting SteamVR.
   - Say in the report which live files were swapped and where their backups are.
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

`V doctor` is read-only and prints JSON checks; exit 0 drive, 1 not ready, 2 worn (refuse):

- the headset isn't worn (proximity sensor, through the testbench proxy)
- `vrserver` and `vrcompositor` run
- the testbench proxy is loaded (`T status` `input.available` and `proximity_ready`); if not,
  the drop-in is missing or SteamVR started before it: `testbench/install.sh install --restart`
  with the headset off
- no testbench override is left over from an earlier run (worn, pose, HMD buttons,
  controllers, fake trackers)
- Frametop's units (`frametop-desktop`, `-input-relay`, `-pointer`, `-gaze`, `-power`) are
  active and their sockets (`@frametop_relay`, `@ft_pointer_helper`, `@ft_powerd`, `@ft_gazed`)
  listen
- for information only: whether hand tracking (`frametop-hands`, opt-in) runs, which the hands
  recipes need; which release is live against this checkout's commit (a hint: it can't see
  swapped binaries, and a published release never names a commit); and the run under way

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
(`T pointer overlays`), `T power status`, and the gaze service's status (`@ft_gazed`). With
`--capture`, also `RUN/LABEL/capture/` (`preview.png`, `stereo.png`, `result.json`): the
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
deletes evidence, and it doesn't undo deployed files: put swapped binaries back from their
`.pre-<topic>` backups when the user shouldn't keep the change. Nor does it undo settings a
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

- `scripts/verify-frametop`: `doctor`, `launch [NAME]`, `guard`, `snapshot LABEL [--capture]`,
  `cleanup [RUN] [--force]`, `ask NAME MESSAGE [--listen S]`. `--help` prints the usage.
  Python 3 standard library only; runs on the SteamOS host.
