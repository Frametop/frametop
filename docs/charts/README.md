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
| ft-screens' screens and panels and when they show, visibility modes, conceal, ft-layout and profiles, floating windows, the dashboard, remote access over VNC | G2 Desktop and panels | `G2-desktop.mmd`, `G2-desktop.tla` |
| The testbench, its input proxy and fake trackers, the verify skills and their lease, the fake-worn guard, the Hand and Eye Recorders and their upload, report.sh, doctor.sh, the logs, the tour | G8 Verify, data and diagnostics | `G8-verify.mmd`, `G8-verify.tla`, `G8-verify-Guard.tla` |
| The dev box and builds, `ft dev build`, `pack/Containerfile`, CI, `framedrop/build.sh` and the zip, FrameDrop, `get.sh`, `install-release.sh`, `install.sh`, `fast-install`, the models, `uninstall.sh` | G7 Build and release | `G7-release.mmd`, `G7-release.tla` |

`feature-map.md` lists every feature (F1 to F22), its processes, sockets, files and units, and proposes the eight chart groups. It was written at `403c1eb`. Its bug table predates the H1, R1 and H2 fixes (`76121c7`, `e15b65f`, `2c783dd`), and it mentions an overview chart and a `snapshot/` folder that are kept outside the repo.

All eight groups are charted.

## Files

Each group has:

- `GROUP.mmd`: the chart's source, a Mermaid state diagram. Every state and arrow names its code.
- `GROUP.svg`: the chart, full size.
- `GROUP-small.png`: a 1200 px wide preview.
- `GROUP.tla` (G3 and G8 have two models): the model.
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

Results with the fixes (`93cbf29`): every rule holds in every config, 66 runs. States per run: 1,856 (PauseLossy) to 32,770 (Joint); Presence is unchanged at 162. The model now describes the fixed code. To see the old breaks, check out `f85dff9` and run its copy of the model.

With L7's fix, ft-screens out of step with the relay is a transient, like a restarting unit: its next report (`ScreensReport`) corrects it, and `Quiet` waits for that report. So in G1, DesktopMatchesPause no longer catches a lost datagram on its own. G2's liveness rule HiddenComesBack carries that check.

The chart (`G1-lifecycle.mmd`, `.svg`) still draws L1 to L7 red. It is due a redraw.

Fixed bugs:

| Id | Ticket | Rule | What happened | Fix |
| --- | --- | --- | --- | --- |
| L1 | DEE-252 | PointerFreeWhilePaused | A relay restart forgot the 3D pointer the helper still held, so the pause couldn't let it go | Fixed in `93cbf29`. `Pointer.stand_down` always sends `hide pause`, and a relay that starts paused stands its pointer down |
| L2 | DEE-253 | HandsOffWhilePaused | A desktop that started during a pause started camd and hands | Fixed in `93cbf29`. The session runs `ft-pause enforce` after `handsmode up`, and a paused relay stops them again |
| L3 | DEE-254 | HandsFollowDesktop | A resume job started camd and hands with SteamVR off | Fixed in `93cbf29`. The resume starts units only with SteamVR running, and with HANDS_MODE=frametop, hand tracking only with the desktop running |
| L4 | DEE-255 | GazeOffWhilePaused | `is-active` missed a restarting ft-gazed, and systemd brought it back mid-pause | Fixed in `93cbf29`. `stop_units` counts `activating` and `reloading` as running |
| L5 | DEE-256 | GazeBackAfterResume, ClosedDesktopComesBack | A relay restart before the resume job lost the list of what the pause stopped | Fixed in `93cbf29`. A relay that starts unpaused with a leftover list runs the resume |
| L6 | DEE-257 | NoStuckGamePause | A relay restart while the game ended left Frametop paused | Fixed in `93cbf29`. A relay that starts in a pause that ends with its game takes the game as running, so `vrgame 0` or GAME_STALE ends the pause |
| L7 | DEE-258 | DesktopMatchesPause (PauseLossy) | One lost `pause off` datagram left the panels hidden, because ft-screens' pause is sticky | Fixed in `93cbf29`. ft-screens adds its pause flag to its `keyboard` report each second, and the relay tells one that is out of step again |

## G6 Remote displays and host input

Covers the Remote Displays app and ft-layout, remote.c's stream slots in ft-screens, one ft-stream per display, the host's audio and input locks, the host-input lease, the relay's network devices, the Frame mic, and the testbench's fake-worn guard.

Model: `G6-remote.tla` (module `G6RemoteExit`): how an ft-stream ends and whether something always reaps it. Configs: `Exit` (the code), `Live` (liveness of asked and stuck ends), `PreFix` (before `e15b65f` added the SIGKILL), `PreR23` (the code at `78e93b1`, before the R2 and R3 fixes). Since those fixes the model tracks `clock` (`quit_at` is set: a SIGKILL is coming) apart from `quit` (the socket is closed), and HungGetsKilled and HungLostGetsKilled read `clock`.

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

Results with the fixes (`144bd31`): Exit holds every rule (332 states) and Live holds AskedEnds and StuckEnds (1,312). PreR23 still breaks HungGetsKilled in 5 steps and HungLostGetsKilled in 6 (312 states), as Exit did at `78e93b1`. PreFix breaks as before. The chart still draws R2 to R5 red and is due a redraw.

Fixed bugs:

| Id | Ticket | Rule | What happened | Fix |
| --- | --- | --- | --- | --- |
| R2 | DEE-259 | HungGetsKilled | An ft-stream the host ended that hung in LiStopConnection was never killed. Its panel froze and still said live | Fixed in `144bd31`. ft-stream says `state lost` before LiStopConnection, and ft-screens starts the SIGKILL clock on it |
| R3 | DEE-260 | HungLostGetsKilled | One that hung after it said lost (decoder close, Disconnect Monitor) was never killed, and held every display of its host | Fixed in `144bd31`. ft-screens sets `quit_at` on `lost` or a closed socket, so FT_REMOTE_QUIT_MS (20 s) later it's killed |
| R4 | DEE-261 | From the code | A desktop stop SIGTERMed its ft-streams and exited. One that hung was an orphan nobody killed | Fixed in `144bd31`. ft-stream ends itself 25 s after its way out starts (`alarm`). Not modelled. An orphan can still overlap the next desktop's stream of its display for up to 25 s |
| R5 | DEE-262 | From the logs (minor) | A 404 that never clears retried every 30 s for good | Fixed in `144bd31`. A stream that ended with a 404 backs off up to 5 minutes. It still retries, and connecting it again tries at once |

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

Results with the fixes (`4606ad0`): every rule holds in every config, 34 runs. States per run: Keys 52 (Buttons) to 252 (Host); Laser 82 (Code) and 70 (Waits). The models now describe the fixed code. To see the old breaks, check out `f85dff9` and run its copy of the models. Undoing K4 and K5 in the Laser model, or K6 and K7 in the Keys model, brings the old counts and breaks back, so the rules aren't vacuous.

The Laser model's `Waits` config (let-go paths wait for a touch) was the other way to fix K5. The fix took the code's way instead: the touch goes, with its trigger up. Both configs hold.

The chart (`G3-input.mmd`, `.svg`) still draws K1 to K7 red. It is due a redraw.

Fixed bugs:

| Id | Ticket | Rule | What happened | Fix |
| --- | --- | --- | --- | --- |
| K1 | DEE-263 | VirtualComesUp | A mouse button pressed while paused or with POINTER=0 went to the virtual mouse. A resume or POINTER=1 while it was held sent its release to the helper, and the virtual mouse kept it down | Fixed in `4606ad0`. The release of a button in `node.held` goes to the virtual device it went down on, whatever pointer mode or the binding says now |
| K2 | DEE-264 | DriverComesUp (minor) | POINTER=0 while a pointer-mode button was held dropped the Pointer with no stand_down, and the laser's trigger stayed down | Fixed in `4606ad0`. `load_config` stands the old pointer down (its driver buttons come up, then `hide pause`) |
| K3 | DEE-265 | DesktopComesUp | A relay restart while a key was down on the desktop. The new relay let go of modifiers and mouse buttons only, so KWin kept the key down and repeated it | Fixed in `4606ad0`. A relay that starts sends ft-screens `keys release`. ft-screens passes it on to Frunner, which lets go of its keys and stops a repeat |
| K4 | DEE-266 | OneTriggerHolder, HolderHasTrigger | The mouse's button asked nobody. Pressed during a touch or a ray pinch, it shared the one trigger and leftHeld, and the first let-go ended both drags | Fixed in `4606ad0`. The helper's mouse press ends a touch (trigger up first if down) and a ray pinch (released, no click), then presses |
| K5 | DEE-267 | NoOrphanTrigger | The pointer was let go while a touch held the trigger. The touch was forgotten with no trigger up, and the next wake connected the controller pressed | Fixed in `4606ad0`. The touch goes with `btn trigger 0` when it held the trigger |
| K6 | DEE-268 | VirtualComesUp | The virtual devices came back from the fd store with keys down after a relay restart, and the new relay's `node.held` was empty | Fixed in `4606ad0`. A reused virtual keyboard and mouse release every key at start (the kernel drops releases of keys that aren't down). A reused gamepad releases its buttons and centres its sticks |
| K7 | DEE-269 | DriverComesUp | A relay restart while a pointer-mode button was down. The release never reached the helper, and leftHeld and the driver's trigger stayed down until the next click | Fixed in `4606ad0`. A relay that starts sends the helper a release for every driver button. The helper ignores a release for a mouse button it doesn't have down, so a touch or ray hold keeps the trigger |

Design choices in these fixes (Daenon can override each):

- K3: a relay that starts releases every key (a full `keys release`), not only modifiers and mouse buttons. A key held through a relay restart comes up and has to be pressed again.
- K4: a mouse press ends any touch on a SteamVR panel, hovering or pressed, and any ray pinch, then takes the trigger. The touch comes back with ft-screens' next word after the mouse lets go.
- K2: POINTER=0 also hides the pointer (`hide pause`), so the virtual controller gives the laser back at once.

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

## G2 Desktop and panels

Covers when one of ft-screens' screens or panels shows (`UpdateVisibility`, `vr.cpp` 1236-1257): the visibility mode and the hide and show switch (`ModeVisible` 1177-1197, `EffectiveMode` 1162-1164), conceal, the pause, and a full gaze calibration's hide. It also covers the desktop's start and stop with `ft-layout start`, ft-layout's apply, reset, profiles and hidden screens, and remote access over VNC (`remote-ctl.sh`, `remote-desktop.sh`, `vnc-bridge.sh`). It shows how often ft-screens draws a screen (`compositor.c` `frame_interval`). Floating windows (ft-floatd and its KWin script) and the dashboard with Steam's panels are drawn by hand.

Model: `G2-desktop.tla` (module `G2Desktop`), one screen. `Visible` is what ft-screens shows. `Wanted` is what the settings and the user ask for: not paused, not concealed, the configured mode with the user's switch, and no calibration running. Configs: `HeadOff` (a desktop that starts with no head pose), `HeadOffFix` (ft-layout sends the settings anyway), `CalInGame` (a full calibration during a VR game), `CalInGameFix` (the calibration hides with a flag of its own), `MidPause` (a desktop that starts mid-pause with remote access on), `MidPauseFix` (the resume starts remote access instead), `Lossy` (the relay's pause datagrams can be lost), `GazedCrash` (ft-gazed dies mid-calibration), `Code` (every fault on), `Fixed` and `FixedAlways` (all three fixes, dashboard and always modes). Each fix config switches off one bug, to show that bug alone breaks the rule.

Results at `78e93b1`:

| Rule | What it says | Result |
| --- | --- | --- |
| TypeOK | | Holds in all 11 configs |
| HiddenComesBack | A screen that is wanted but hidden shows again, or stops being wanted | Breaks in Lossy (G1's L7) and GazedCrash, 4 steps (G4's Z1). Holds in every other config |
| ShownOnlyWanted | A screen shown against the settings doesn't stay shown | Breaks in HeadOff, 3 steps (P1), CalInGame, 5 steps (P2), Lossy (a lost `pause on`) and Code. Holds in HeadOffFix, CalInGameFix, MidPause, MidPauseFix, GazedCrash, Fixed and FixedAlways |
| VncOffInPause | Remote access doesn't keep running through a pause | Breaks in MidPause, 4 steps (P3) and Code. Holds in every other config |
| VncComesBack | Remote access the pause stopped comes back with the resume | Holds in all 11 configs |

States per run: 208 (HeadOffFix) to 3,344 (Code).

Open suspected bugs:

| Id | Ticket | Rule | What happens |
| --- | --- | --- | --- |
| P1 | DEE-278 | ShownOnlyWanted | A desktop that starts with no head pose (the headset off or asleep). `apply_screens` asks ft-screens for `head` (`ft_layout.py` 756) before `send_visibility` (758) and `send_hidden` (774). The error is only logged as "not arranged now" by `start` (1955-1958) and `apply --wait` (2031-2038), so ft-screens keeps its defaults (`vr.cpp` 528-548): always, nothing concealed. Concealed screens show, and users of the toggle, gesture or dashboard mode see the screens all the time, until a reset, a settings change or a profile. `use` has a `send_hidden` fallback (2076-2079); `start` and `apply` don't |
| P2 | DEE-279 | ShownOnlyWanted (minor) | A full gaze calibration during a VR game, with the always mode and ingames hide (the defaults). ft-screens' `state` reports `g_mode` (`vr.cpp` 2606, 6254-6257), not the dashboard mode in effect, so `gazecheck.py` 623 reads the screens as shown and its `hide` (624) changes nothing. The `show` at close (1264-1265) turns the switch on under the dashboard mode (`vr.cpp` 5992-5998), and the screens stay over the game with the dashboard closed until the game ends or the hotkey |
| P3 | DEE-280 | VncOffInPause (minor) | A desktop that starts mid-pause (`autostart.sh` on a SteamVR restart, or the dashboard's Desktop entry) runs `remote-ctl.sh start` with REMOTE=1 (`frametop-session.sh` 246-249) without a pause check. The relay sends only `pause on` again (`game_pause.py` 675-688), so krdp and Xvnc run through the game, and the resume doesn't know they ran. Same class as G1's L2 |

The Lossy and GazedCrash breaks are G1's L7 and G4's Z1, drawn in those groups. A lost `pause on` also shows the screens through the pause, because the relay sends it again only to a new ft-screens.

## G8 Verify, data and diagnostics

Covers where each testbench fake enters the real pipeline, and how long it lasts. Fake 1 is the input proxy inside vrserver: the `90-frame-testbench.conf` drop-in preloads `interpose.cpp`, which wraps `driver_cv.so` and takes worn, head pose, HMD button and synthetic controller overrides on `RUN/frame-testbench.sock`. Fake 2 is the fake trackers (`frame-testbench-fakes`, `fakes.py`): stand-in eye files for ft-gaze, and a marker that parks ft-hands while fake hands are published. Fake 3 is Frametop's own sockets (the relay's testbench devices, the pointer helper, ft-powerd). Fake 4 is the fake-worn guard (`frametop-fake-worn-guard`, `hostinput.py`). The chart also draws the verify run's lease and cleanup (`verify-frametop` launch, guard, snapshot, cleanup), the MCP server, the Hand and Eye Recorders with their export and Hugging Face upload, `report.sh`, `doctor.sh`, the logs and the tour. Arrows into G1, G3, G4, G5 and G6 show where each fake feeds those groups.

Models:

- `G8-verify.tla` (module `G8Verify`): one verify run, its lease, the proxy's overrides, the fake trackers, an agent that can die mid-run, status polls from later agents, and a wearer. Configs: `Code` (the code as it is), `NoDeath` (the agent always lives to clean up), `Overrides` (only the overrides' bug), `OverridesFix` (overrides bound to the lease), `Polls` (only the idle hour's bug) and `Fixed` (both fixes).
- `G8-verify-Guard.tla` (module `G8Guard`): extends verify-fake-worn-guard's `FakeWornGuard.tla`, read from `.claude/skills/verify-fake-worn-guard/model/` without changes. It adds fairness for the system's own steps and the rule `GuardRestoresReal`. Configs: `LiveClean` (no faults), `LiveHangs` (the proxy can stall), `LiveKills` (the guard can die), `LiveNoF` (stalls and deaths, but not mid-fake) and `LiveAll` (every fault). The skill's own configs and rules still hold at `78e93b1`: Clean 88, Hangs 188, Kills 264, NoF 480 and All 632 states.

Results at `78e93b1`:

| Rule | What it says | Result |
| --- | --- | --- |
| TypeOK | | Holds in all 11 configs |
| LaunchClean | A run starts on a clean headset (launch's doctor) | Holds in all 6 Verify configs |
| NoDriveOnWearer | Drive steps happen only with nobody in the headset (`verify-frametop guard`) | Holds in all 6 Verify configs |
| OverridesEnd | The proxy's overrides end once nobody drives the run | Breaks in Code and Overrides, 5 to 6 steps then a loop (V1). Holds in NoDeath, OverridesFix, Polls and Fixed |
| FakesEnd | The fake trackers end once nobody drives the run | Breaks in Code and Polls, 6 to 9 steps then a loop (V2). Holds in NoDeath, Overrides, OverridesFix and Fixed |
| WearerGetsReal | Someone who puts the headset on gets their own head, proximity and trackers back | Breaks in Code, Overrides and Polls (V1, V2). Holds in NoDeath, OverridesFix and Fixed |
| GuardRestoresReal | Once the wearer is faked with no live guard and no command running, the fake ends with no command needed | Breaks in LiveNoF and LiveAll (V3). Holds in LiveClean, LiveHangs and LiveKills |

States per run: 26 (NoDeath) to 98 for Verify, 88 (LiveClean) to 632 (LiveAll) for Guard.

Open suspected bugs:

| Id | Ticket | Rule | What happens |
| --- | --- | --- | --- |
| V1 | DEE-281 | OverridesEnd, WearerGetsReal | The input proxy's overrides (worn on or off, head pose, HMD buttons, synthetic controllers) have no time limit, and a real wearer doesn't end them (`testbench/native/input_state.cpp` 112-116). Only `frame-testbench release`, `verify-frametop cleanup` or a SteamVR restart do (`testbench/frame_testbench/cli.py` 351-363). A run whose agent dies (a killed shell, a stalled approval prompt, the context running out) keeps them, and its guard keeps every input host's lock. The lease only marks the run stale (`verify-frametop` 91, 169). Someone who puts the headset on later gets a frozen head pose, or standby under worn off |
| V2 | DEE-282 | FakesEnd, WearerGetsReal | The fake trackers end after an hour with no command (`fakes.py` 47, 815), but any command restarts the hour, status too (836-837). `frame-testbench status` sends one, so every `verify-frametop doctor`, `verify-frametop guard` and MCP status call does. A later agent's doctor checks keep a dead run's fake eyes and parked hands alive, and without them the fakes still last up to an hour |
| V3 | DEE-283 | GuardRestoresReal (minor) | A guard that dies while the input proxy stalls. `after_guard` tries the release 3 times 0.2 s apart, then gives up (`hostinput.py` 209-217, 230-237). The faked wearer stays until the next `frame-testbench` command. NoHostTakeover still holds, because the flag keeps ft-stream from taking a lease |
| V4 | DEE-284 | not modelled (minor, G3 side) | In pointer mode the relay's drop of the testbench mouse lets go of its buttons only on the virtual mouse (`release_held`, `input/input-relay.py` 1654-1659). A testbench killed mid-drag, or a release read after `hostbye`, leaves the 3D mouse's button down until the next click. Written up in `testbench/docs/desktop.md` under Limits. Same class as G3's K7 |
| V5 | DEE-285 | not modelled (minor, moot at `78e93b1`) | The hand upload's draft gate opens on `FT_HANDREC_ALLOW_UPLOAD=1` alone (`hands/rec/hub.py` 91-94), while the eye upload's opens only for another dataset (`gaze/rec/hub.py` 60-62). `is_draft` counts a missing text as final (`recorder/hub.py` 47-52). All five consent texts are final today |

Fixed before `78e93b1`, drawn green: the fake-worn guard's A to F (`37d0fbf`, ft-stream `bd606ce` in merge `81a5c82`, `0c7eb9e`, `38d6257`, `1f485fe`), which the skill's model checks.

## G7 Build and release

Covers how a build is made and how a release reaches the Frame. On the build side: the dev box (`setup/dev-container.sh`, a Fedora 44 toolbox in distrobox), a source install's `install.sh` in a clone, `ft dev build` and `ft dev test` (the image from `pack/Containerfile`, with its pinned base, `uv.lock`, OpenVR and `.dockerignore`), CI (`.github/workflows/release.yml` on a `v*` tag or a manual run, with its version rule and smoke test), and `framedrop/build.sh` with `pack/release-info.py` (the OCI archive, `frametop-release.json`, `Frametop.zip` and its manifest). On the install side: the ways in (`get.sh` and its channels, FrameDrop's installer and askpass socket, the zip by hand, or from source), `pack/install-release.sh` (the SteamOS table, the image load and sha256 check, the atomic copy into `releases/VERSION`, `current` and the clean-up), `install.sh`'s 12 steps under `set -e`, `scripts/fast-install` and its undo, the models (the pinned hand student, `ft-models` with the hands and eyes catalogs, `voice/models.sh`), and upkeep (`update-check.py`, `conf-migrate.sh`, `uninstall.sh`). Arrows into G1, G3, G4, G5 and G8 show where an install's units and models reach those groups. The flow is drawn by hand. The `.mmd` names the files and lines.

Model: `G7-release.tla` (module `G7Release`). It follows what an install leaves behind, release after release (N = 3): each release's folder and container, the release each unit points into (core for the units always installed, hands for an optional part), the `current` link, a stopped `install.sh`, and `fast-install` patches. Configs: `Code` (every fault on), `CIOnly` (CI releases and full installs only, no `fast-install`), `Skips` (a later install skips the optional part), `Rollback` (an older release's `install.sh` run to go back, with `fast-install`), `Interrupt` (one `install.sh` stops mid-way, with `fast-install`), `Local` (local builds), `Reinstall` (the same release installed again after a `fast-install`), `Fixed` (all four fixes, no interruption) and `FixedInterrupt` (all four fixes with an interruption).

Results at `78e93b1`:

| Rule | What it says | Result |
| --- | --- | --- |
| TypeOK | | Holds in all 9 configs |
| UnitsHaveFolder | Every unit points into a release folder and container that are still there | Breaks in Skips, 10 steps (W2) and Code. Holds in every other config |
| NoMixedUnits | Outside a running install, every unit points into one release | Breaks in Skips, 7 steps (W2), Interrupt, 7 steps (W5), FixedInterrupt and Code |
| CurrentIsLive | Outside a running install, `current` names the release the units run | Breaks in Rollback, 12 steps (W1), Interrupt, 3 steps (W5), FixedInterrupt and Code |
| PatchLands | `fast-install` never reports success for a tree nothing runs | Breaks in Rollback, 13 steps (W1), Interrupt, 8 steps (W5), FixedInterrupt and Code |
| ReleaseMatchesCommit | Every release's image was built from the commit its release JSON names | Breaks in Local, 1 step (W3) and Code. Holds in CIOnly and Fixed |
| ReinstallGivesRelease | Installing a release again gives that release as it was built | Breaks in Reinstall, 9 steps (W4), Rollback, Interrupt and Code |
| InstallSettles | A stopped install is finished by running the same command again | Holds in all 9 configs |

CIOnly (116 states) and Fixed (1,844) hold every rule. States per run: 116 (CIOnly) to 155,405 (Code). FixedInterrupt shows that none of the W1 to W4 fixes helps W5: only running the command again settles it.

Also checked in the code and drawn as holding: the copy out of the image is atomic (`VERSION.new`, then `mv`), a damaged image file stops before anything is written (exit 3), another image under a version already installed is refused, and every model file is pinned to a commit and a sha256.

Open suspected bugs:

| Id | Ticket | Rule | What happens |
| --- | --- | --- | --- |
| W1 | DEE-286 | CurrentIsLive, PatchLands | Only `install-release.sh` moves `releases/current`, after `install.sh` (`pack/install-release.sh` 222-224). Going back by running the older release's `install.sh`, as `pack/README.md` 202 and `install-release.sh` 18-19 say, moves the units and leaves `current` on the newer release. `fast-install` then patches `current`'s tree (`scripts/fast-install` 135-142), finds no unit in it (`unit_dirs` 355) or restarts units that run the other tree, and reports "fast-installed X onto VERSION". The next install keeps the wrong release as the one before (224-236), and `get.sh` reads the channel from it (`get.sh` 169-172) |
| W2 | DEE-287 | UnitsHaveFolder, NoMixedUnits | A part that a later install skips (voice or hands unticked, `--no-voice`, `--no-hands`, or `--yes` with no quiet sudo, which is FrameDrop with no password, `install.sh` 153-154) keeps its units in the old release's folder and container. They point at `@REPO@` (`hands/frametop-hands.service` 16-19, `hands/frametop-camd.service` 19, `voice/frametop-voice.service` 12, `voice/frametop-voice-whisper.service` 14) and at that tree's BOX (`scripts/in-box`). Two releases on, `install-release.sh` 225-236 removes the folder and the container, and those services fail. Only the recorders' menu entries are renewed (`install.sh` 174-176, 234-237) |
| W3 | DEE-288 | ReleaseMatchesCommit (local builds only) | `ft dev build` builds the image from the working tree (`ft` 188), with uncommitted and untracked files in. `.dockerignore` leaves out a fixed list, not `work/` or other untracked files. `framedrop/build.sh` 42 takes `--commit` from HEAD at pack time, and `fast-install` diffs from that COMMIT (428-447) and trusts it for its rebuilds (490-491). A `0.3.0-local.SHA` version is passed by hand. CI is clean: a fresh checkout and `GITHUB_SHA` |
| W4 | DEE-289 | ReinstallGivesRelease | Installing the same release again (the same IMAGE_ID) skips the copy when `.frametop-release` is there (`pack/install-release.sh` 169-172, 193), so fast-installed files and `.fast-install/` stay. The tree names the release's COMMIT and runs other commits. `fast-install undo` is the only way back |
| W5 | no ticket yet | NoMixedUnits, CurrentIsLive (minor) | `install.sh` runs 12 steps under `set -e`. A failing step or a power-off leaves the early steps' units in the new release and its container, the later ones in the old release, and `current` on the old one. `fast-install` then patches the old tree. Running the same command again finishes it: no copy, every step again |

Ways around them until they are fixed: for W1, run the newest release's `install-release.sh` again. For W2, install the skipped part again after each update (`hands/run.sh install`). For W3, pack from a clean checkout, or use CI. For W4, run `fast-install undo` before installing again. For W5, run the same command again.

`G7-release-traces/` holds TLC's output for every break at `78e93b1`. It is committed, unlike the other groups' trace folders, so the W paths can be read without Java. The step counts above come from those traces. The chart's W2 and W4 legend boxes say 9 and 8 steps. They are one short, and the next redraw corrects them.

## Running a check

The checks need TLC 1.7.4 and Java. They look for TLC at `~/.cache/frametop-formal/tla2tools-1.7.4.jar` (the verify skills' `scripts/check` put it there), or at `TLA2TOOLS` for every group but G1. Java comes from `JAVA`, then `PATH`. G1 asks mise for `java@temurin-25`.

A check keeps a core busy for seconds to minutes, so run it through `frame-job` on the Frame, at the lowest priority:

```
cd docs/charts
J=$(mise which java --tool java@temurin-25)
frame-job --local -- nice -n 19 env JAVA=$J PATH=$(dirname $J):$PATH python3 G4-gaze-check.py           # every config and rule
frame-job --local -- nice -n 19 env JAVA=$J PATH=$(dirname $J):$PATH python3 G4-gaze-check.py Gaze:Code  # one model and config
frame-job --local -- nice -n 19 env JAVA=$J PATH=$(dirname $J):$PATH python3 G1-lifecycle-check.py PauseHide
```

G3 takes `Keys`, `Laser`, `Keys:Host` and so on. G5 takes `Hands:FreshBoot` and so on. G2 takes `Desktop:HeadOff` and so on. G8 takes `Verify`, `Guard`, `Verify:Overrides`, `Guard:LiveNoF` and so on, and reads the guard model from the verify-fake-worn-guard skill. G6 takes `Exit`, `Live` or `PreFix`. G1 and G7 take config names, such as `Rollback`.

Each script writes TLC's full output, with every counterexample trace, to `GROUP-traces/` next to the models. Those folders are ignored by git, except `G7-release-traces/`. A rule that breaks prints its shortest path, and the chart's red badges follow that path.

When code in a group's area changes, re-run its check. A rule that changes from breaks to holds means a bug was fixed: move its badge to green with the commit. A rule that changes from holds to breaks is a regression, or the model needs the new behaviour.
