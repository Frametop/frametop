# Charts and models

These charts map how Frametop's parts start, stop, hand things to each other and fail. Each finished group has a state chart and a TLA+ model that TLC checks. The charts were drawn from the code at `78e93b1` (branch `experimental`, 2026-10-30). Where the code has moved on since, the code wins, and the chart is due a redraw.

Read the matching group's `.mmd` and model before you answer a question or change code in its area. The `.mmd` names the files and line ranges each state comes from, so it is also an index into the code. The model's rules say what must stay true, and its configs say which bugs it reproduces.

| Area of the question | Group | Read first |
| --- | --- | --- |
| SteamVR start and stop, the units, the desktop session, pausing for VR games, worn or away, display power | G1 Lifecycle and presence | `G1-lifecycle.mmd`, `G1-lifecycle.tla` |
| Remote displays, ft-stream, remote.c slots, the host-input lease, the fake-worn guard, the relay's network devices | G6 Remote displays and host input | `G6-remote.mmd`, `G6-remote.tla`, then the verify skills' models (below) |
| The input relay, held keys, bindings, the pointer helper's laser and trigger, keyboards, voice typing, gamepad mode | G3 Input and pointer | `G3-input.mmd`, `G3-input-Keys.tla`, `G3-input-Laser.tla` |
| Gaze sources, ft-gaze and ft-gazed, calibration and its panel, idle and wake, our own eye tracker | G4 Gaze | `G4-gaze.mmd`, `G4-gaze.tla` |
| ft-camd, ft-hands, when hand tracking runs and parks, hand or controller per side, near input on panels, the hand ray and pinches | G5 Hands | `G5-hands.mmd`, `G5-hands.tla` |

`feature-map.md` lists every feature (F1 to F22), its processes, sockets, files and units, and proposes the eight chart groups. It was written at `403c1eb`. Its bug table predates the H1, R1 and H2 fixes (`76121c7`, `e15b65f`, `2c783dd`), and it mentions an overview chart and a `snapshot/` folder that are kept outside the repo.

G2 (desktop and panels), G8 (verification, data and diagnostics) and G7 (build, install and release) are not charted yet.

## Files

Each group has:

- `GROUP.mmd`: the chart's source, a Mermaid state diagram. Every state and arrow names its code.
- `GROUP.svg`: the chart, full size.
- `GROUP-small.png`: a 1200 px wide preview.
- `GROUP.tla` (G3 has two models): the model.
- `GROUP-*.cfg`: one TLC config per scenario: the code as it is, the code before a fix, or the code with one fault switched off.
- `GROUP-check.py`: runs TLC on every config and rule and prints what holds and what breaks, with each break's shortest path.

Badges on the charts work the same way in every group. Red states and the labels `(L2.3)` or `(K4.1)` mark the steps of a suspected open bug, in the order TLC's shortest path takes them. Green marks a fixed bug, with the commit that fixed it. Dashed grey boxes are other groups: an arrow into one shows where the two groups touch, and the detail is drawn in that group's chart.

## G1 Lifecycle and presence

Covers SteamVR and the units that start and stop with it, the desktop session, the input relay's pause for VR games (auto, gesture, key, resume when the game ends, the state file across relay restarts), the pointer helper's worn answer, and ft-powerd's display power.

Model: `G1-lifecycle.tla` (module `G1Lifecycle`). `check.py` copies it under the module's name for TLC. `G1-lifecycle.cfg` is the same as `PauseHide`, for running TLC by hand.

Configs: `PauseHide` and `PauseClose` (the two pause_desktop settings, with crashes and auto pause), `PauseCloseNoAuto`, `PauseNoCrash` (no process crashes), `PauseLossy` (datagrams can be lost), `Presence` (worn and display power alone), `Joint` (pause and presence together, no crashes, hands not tied to the desktop).

Results at `78e93b1`:

| Rule | What it says | Result |
| --- | --- | --- |
| MouseFreeWhilePaused | The 3D mouse is let go while paused | Holds in every config |
| PointerFreeWhilePaused | The pointer helper holds no pointer while paused | Breaks in PauseHide, PauseClose, PauseCloseNoAuto (L1). Holds without crashes |
| GamepadOutsideGames | Gamepad mode only during a game | Holds in every config |
| GazeOffWhilePaused | frametop-gaze is stopped while paused | Breaks with crashes (L4). Holds in PauseNoCrash, PauseLossy, Joint |
| HandsOffWhilePaused | camd and hands are stopped while paused | Breaks in every pause config with HandsWithDesk (L2), crashes or not |
| HandsFollowDesktop | camd and hands run only with SteamVR and the desktop | Breaks in every pause config with HandsWithDesk (L3) |
| DesktopMatchesPause | The panels are hidden exactly while paused | Holds, except PauseLossy, where one lost "pause off" leaves them hidden (L7) |
| GazeBackAfterResume | A resume starts gaze again | Breaks with crashes (L5) |
| ClosedDesktopComesBack | A desktop the pause closed comes back on resume | Breaks in PauseClose and PauseCloseNoAuto (L5) |
| NoStuckGamePause | A pause that waits for the game's end ends | Breaks with crashes (L6) |
| WornAgreesSoon, DarkHasOwner, AwayReleasesPointer | Worn readers agree within 1.5 s, dark displays have an owner, away lets go of the pointer | Hold (Presence 162 states, Joint 34,942) |

States per run: 27,172 (PauseHide) to 34,942 (Joint).

Open suspected bugs:

| Id | Ticket | Rule | What happens |
| --- | --- | --- | --- |
| L1 | DEE-252 | PointerFreeWhilePaused | A relay restart forgets the 3D pointer the helper still holds, so the pause can't let it go |
| L2 | DEE-253 | HandsOffWhilePaused | A desktop that starts during a pause starts camd and hands |
| L3 | DEE-254 | HandsFollowDesktop | A resume job starts camd and hands with SteamVR off |
| L4 | DEE-255 | GazeOffWhilePaused | `is-active` misses a restarting ft-gazed, and systemd brings it back mid-pause |
| L5 | DEE-256 | GazeBackAfterResume, ClosedDesktopComesBack | A relay restart before the resume job loses the list of what the pause stopped |
| L6 | DEE-257 | NoStuckGamePause | A relay restart while the game ends leaves Frametop paused |
| L7 | DEE-258 | DesktopMatchesPause (PauseLossy) | One lost "pause off" datagram leaves the panels hidden, because ft-screens' pause is sticky |

## G6 Remote displays and host input

Covers the Remote Displays app and ft-layout, remote.c's stream slots in ft-screens, one ft-stream per display, the host's audio and input locks, the host-input lease, the relay's network devices, the Frame mic, and the testbench's fake-worn guard.

Model: `G6-remote.tla` (module `G6RemoteExit`): how an ft-stream ends and whether something always reaps it. Configs: `Exit` (the code), `Live` (liveness of asked and stuck ends), `PreFix` (before `e15b65f` added the SIGKILL).

The rest of G6 is checked by the models in the verify skills, which matched their baselines at `78e93b1`:

- `.claude/skills/verify-remote-streams/`: `HostInputLease.tla` (3,561 states), `HostChart.tla` (345), `RemoteStreams.tla` (164).
- `.claude/skills/verify-fake-worn-guard/`: `FakeWornGuard.tla` (264 to 632 states).

Results at `78e93b1`:

| Config | Rule | Result |
| --- | --- | --- |
| Exit | TypeOK, OneStreamPerDisplay, OneHostOperation, StoppedStaysStopped, NeverStranded | Hold (312 states) |
| Exit | HungGetsKilled | Breaks, 5 steps (R2) |
| Exit | HungLostGetsKilled | Breaks, 6 steps (R3) |
| Live | AskedEnds | Holds (1,232 states) |
| Live | StuckEnds | Breaks (R2, R3 as liveness) |
| PreFix | AskedEnds, StuckEnds | Break: the slot leak `e15b65f` fixed |
| PreFix | OneStreamPerDisplay | Holds |

Fixed and drawn green: H1 (`76121c7`), H2 (`2c783dd`), R1 and the slot leak (`e15b65f`), and the fake-worn bugs A and C (`37d0fbf`), D (`0c7eb9e`), B (`bd606ce`), E (`38d6257`) and F (`1f485fe`).

Open suspected bugs:

| Id | Ticket | Rule | What happens |
| --- | --- | --- | --- |
| R2 | DEE-259 | HungGetsKilled | An ft-stream the host ended that hangs in LiStopConnection is never killed. Its panel freezes and still says live |
| R3 | DEE-260 | HungLostGetsKilled | One that hangs after it said lost (decoder close, Disconnect Monitor) is never killed, and holds every display of its host |
| R4 | DEE-261 | From the code | A desktop stop SIGTERMs its ft-streams and exits. One that hangs is an orphan nobody kills, and the next desktop starts its display beside it |
| R5 | DEE-262 | From the logs (minor) | A 404 that never clears retries every 30 s for good |

A stuck ft-stream keeps `frametop-input-HOST.lock` and `frametop-audio-HOST.lock` until it dies, so the host's other streams get no keyboard, mouse or sound.

## G3 Input and pointer

Covers the input relay (device roles, grabs, bindings, the typing target, the fd store), ft-screens' keys, the pointer helper's laser and its one trigger, the driver, Full Keyboard and ft-textinput, voice typing, and gamepad mode.

Models:

- `G3-input-Keys.tla` (module `G3Keys`): every key and button that goes down comes up on the target it went down on. Configs: `Buttons`, `ButtonsRestart`, `Host`, `Keys`, `Mods`, `Stay`.
- `G3-input-Laser.tla` (module `G3Laser`): one owner for each hand's laser, and one holder of the trigger. Configs: `Code`, and `Waits` (let-go waits for a touch to end).

Results at `78e93b1`:

| Model and config | Holds | Breaks |
| --- | --- | --- |
| Keys Buttons | TypeOK, DesktopComesUp (144 states) | VirtualComesUp (K1, 3 steps), DriverComesUp (K2, 3 steps) |
| Keys ButtonsRestart | TypeOK, DesktopComesUp (72) | VirtualComesUp (K6), DriverComesUp (K7) |
| Keys Host | TypeOK, DriverComesUp (550) | DesktopComesUp (K3), VirtualComesUp (K6) |
| Keys Keys | TypeOK, VirtualComesUp, DriverComesUp (300) | DesktopComesUp (K3, 4 steps) |
| Keys Mods | Every rule (180) | None |
| Keys Stay | Every rule (96) | None |
| Laser Code | TypeOK, OneOwnerPerHand (302) | OneTriggerHolder (K4, 3 steps), HolderHasTrigger (K4, 4 steps), NoOrphanTrigger (K5, 5 steps) |
| Laser Waits | TypeOK, OneOwnerPerHand, NoOrphanTrigger (178) | OneTriggerHolder, HolderHasTrigger (K4) |

The DEE-222 stuck-keys fix (`956ed81`) is drawn green.

Open suspected bugs:

| Id | Ticket | Rule | What happens |
| --- | --- | --- | --- |
| K1 | DEE-263 | VirtualComesUp | A mouse button pressed while paused or with POINTER=0 goes to the virtual mouse. A resume or POINTER=1 while it is held sends its release to the helper, and the virtual mouse keeps it down |
| K2 | DEE-264 | DriverComesUp (minor) | POINTER=0 while a pointer-mode button is held drops the Pointer with no stand_down, and the laser's trigger stays down |
| K3 | DEE-265 | DesktopComesUp | A relay restart while a key is down on the desktop. The new relay lets go of modifiers and mouse buttons only, so KWin keeps the key down and repeats it |
| K4 | DEE-266 | OneTriggerHolder, HolderHasTrigger | The mouse's button asks nobody. Pressed during a touch or a ray pinch, it shares the one trigger and leftHeld, and the first let-go ends both drags |
| K5 | DEE-267 | NoOrphanTrigger | The pointer is let go while a touch holds the trigger. The touch is forgotten with no trigger up, and the next wake connects the controller pressed |
| K6 | DEE-268 | VirtualComesUp | The virtual devices come back from the fd store with keys down after a relay restart, and the new relay's `node.held` is empty |
| K7 | DEE-269 | DriverComesUp | A relay restart while a pointer-mode button is down. The release never reaches the helper, and leftHeld and the driver's trigger stay down until the next click |

Ctrl+Alt+Shift+Esc (`release_keys`) clears K1 and K3, and so does pressing the stuck key again. It doesn't clear K2, K5, K6 or K7.

## G4 Gaze

Covers the gaze sources (SteamVR's eye-server.mmap, its gaze action, our own tracker, fusion), ft-gaze to ft-gazed to the pointer helper, ft-gazed's idle and wake, the full calibration, the calibration panel and its lease, and Check headset fit.

Model: `G4-gaze.tla` (module `G4Gaze`). Configs: `Code` (the code, our tracker), `Steam` (SteamVR's tracker), `CodeAuto`, `Crash` (ft-gazed can die), `Free`, `NoBlip` (eyes never come back inside AWAY_MIN), `FitSays` (the fit's reply says calibrated at once), `NewLayout` (an eye-server.mmap layout ft-gaze doesn't know), `PreBlind` (before `d39fbb9`), `PreGuard` and `PreGuardAuto` (before `3a796b5`).

Results at `78e93b1`:

| Rule | What it says | Result |
| --- | --- | --- |
| IdleStops | ft-gazed stops ft-gaze and ft-eyes when nothing uses gaze | Holds |
| CheckKeepsAwake | An open check keeps the tracker awake | Holds in all 11 configs |
| ScreensComeBack | Frametop's screens come back after a calibration | Holds without crashes. Breaks in Crash, 7 steps (Z1) |
| FullOnlyWhenNeeded | A full calibration opens only on an uncalibrated tracker | Breaks in Code and CodeAuto, 10 steps (Z2), and in Crash, Free, NoBlip and PreGuard. Holds in FitSays, Steam, NewLayout and PreBlind. PreGuardAuto also breaks it through the don (fixed by `3a796b5`) |
| CalibrationComes | An uncalibrated tracker gets its calibration | Breaks in Code and Steam, 11 steps (Z4). Holds in NoBlip. Breaks in PreBlind (the 2026-10-05 deadlock, fixed by `d39fbb9`) and NewLayout (a known limit) |
| TrackerWakes | A woken tracker sends | Holds, except where ft-gazed crashes forever |

Open suspected bugs:

| Id | Ticket | Rule | What happens |
| --- | --- | --- | --- |
| Z1 | DEE-270 | ScreensComeBack | ft-gazed dies during a full calibration. Nothing calls `close()`, ft-screens' hide has no lease, and the new ft-gazed never knew, so the screens stay hidden |
| Z2 | DEE-271 | FullOnlyWhenNeeded | Our tracker's first calibration saves while ft-gazed still holds ft-eyes' old status. With `full_armed` still set, a second full calibration opens at once |
| Z3 | DEE-272 | From the code (minor) | Check headset fit asked for while idle waits for a corrected gaze, which our tracker can't send before its first calibration, so it opens only after WAKE_SETTLE (15 s) |
| Z4 | DEE-273 | CalibrationComes | No eyes for EYES_GONE (2 s) closes the full calibration, but away needs AWAY_MIN (3 s). Eyes back in that second are no don, so nothing re-arms it |

## G5 Hands

Covers ft-camd and ft-hands (how they start, restart and stop), when hand tracking runs and when the pause parks it, the input relay's park state and the helper's dashboard word, the hand or controller role on each side (`handoff.h`), near input on ft-screens' panels, and the hand ray and pinches. The pipeline from XRService's cameras to the readers of `/run/user/UID/frametop-hands` is drawn by hand.

Model: `G5-hands.tla` (module `G5Hands`). Configs: `Steady` (hand interaction on, the run folder there), `FreshBoot` (a pause before hand tracking ever ran), `FreshBootFix` (the same with the park remembered only once written), `SwitchOff` (hand interaction switched off mid-pause), `Cutouts` (ft-cutouts without hand interaction), `Restarts` (the helper and the relay can restart), `RestartsFix` and `DashFix` (the dashboard word resent after a restart), `NoHands` and `NoHandsL2` (hand interaction off, with G1's L2), `Code` (every fault on). Each fix config switches off one bug, to show that bug alone breaks the rule.

Results at `78e93b1`:

| Rule | What it says | Result |
| --- | --- | --- |
| TypeOK | | Holds in all 11 configs |
| TrackerIdleInPause | While paused with the dashboard closed, a running hand tracker soon stops or parks | Breaks in FreshBoot, 3 steps (H5-1), SwitchOff, 4 steps (H5-2), Cutouts, 4 steps (H5-3), Steady and Restarts (H5-4), NoHandsL2 (G1's L2) and Code. Holds in FreshBootFix, DashFix, RestartsFix and NoHands |
| ParkLifts | A park on a running tracker lifts once the pause ends or the dashboard opens | Breaks in Restarts, 6 steps (H5-4) and Code. Holds in every config without relay restarts |

States per run: 64 (Steady, DashFix) to 1,741 (Code).

Also checked in the code and drawn as holding: the helper ends a held pinch as lost when ft-hands is quiet for 2.5 s (`ft-pointer.cpp` 3649-3672), a park ends gestures as lost (`track/main.cpp` 890-917), and ft-screens never holds a near-input grab through a controller (`vr.cpp` 2724-2779).

Open suspected bugs (tickets for H5-1 to H5-4 are being created):

| Id | Rule | What happens |
| --- | --- | --- |
| H5-1 | TrackerIdleInPause | A pause before hand tracking ever ran since login. `/run/user/UID/frametop-hands` is missing, so `update_park` writes no park file (`game_pause.py` 722) but still sets `parked` (733). Tracking that starts later in the pause, from a desktop opened mid-pause, runs unparked through the game, and nothing calls `update_park` again until the dashboard opens or a resume |
| H5-2 | TrackerIdleInPause (minor) | Hand interaction switched off mid-pause. `set_hands` (701-707) empties `keep` and unparks, but nothing stops the units the pause kept. `pause-test.py` 262-263 expects the worker to stop them |
| H5-3 | TrackerIdleInPause (minor) | ft-cutouts' transient units aren't in the pause's `UNITS`, and without hand interaction nothing parks them, so the cutouts' tracker runs through VR games |
| H5-4 | TrackerIdleInPause, ParkLifts (minor) | The helper sends `vrdash` only on a change, or every 5 s in a game. A helper that goes away with the dashboard open (a SteamVR restart or a crash) leaves the relay's copy open, and a new relay thinks it is closed. Outside a game, a manual pause stays unparked, or parks under an open dashboard |

G1's L3 has its root here: `frametop-camd.service` and `frametop-hands.service` have no `Requisite=steamvr`, which `game_pause.py` 577 assumes. Only `frametop-handview.service` has it.

## Running a check

The checks need TLC 1.7.4 and Java. They look for TLC at `~/.cache/frametop-formal/tla2tools-1.7.4.jar` (the verify skills' `scripts/check` put it there), or at `TLA2TOOLS` for G3, G4 and G6. Java comes from `JAVA`, then `PATH`. G1 asks mise for `java@temurin-25`.

A check keeps a core busy for seconds to minutes, so run it through `frame-job` on the Frame, at the lowest priority:

```
cd docs/charts
J=$(mise which java --tool java@temurin-25)
frame-job --local -- nice -n 19 env JAVA=$J PATH=$(dirname $J):$PATH python3 G4-gaze-check.py           # every config and rule
frame-job --local -- nice -n 19 env JAVA=$J PATH=$(dirname $J):$PATH python3 G4-gaze-check.py Gaze:Code  # one model and config
frame-job --local -- nice -n 19 env JAVA=$J PATH=$(dirname $J):$PATH python3 G1-lifecycle-check.py PauseHide
```

G3 takes `Keys`, `Laser`, `Keys:Host` and so on. G5 takes `Hands:FreshBoot` and so on. G6 takes `Exit`, `Live` or `PreFix`. G1 takes config names.

Each script writes TLC's full output, with every counterexample trace, to `GROUP-traces/` next to the models. Those folders are ignored by git. A rule that breaks prints its shortest path, and the chart's red badges follow that path.

When code in a group's area changes, re-run its check. A rule that changes from breaks to holds means a bug was fixed: move its badge to green with the commit. A rule that changes from holds to breaks is a regression, or the model needs the new behaviour.
