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

Results with the fixes (`32456e1`): IdleStops, CheckKeepsAwake, ScreensComeBack and FullOnlyWhenNeeded hold in all 11 configs. CalibrationComes holds in Code, CodeAuto, Steam, NoBlip, FitSays, PreGuard and PreGuardAuto. It still breaks where it should: Crash (ft-gazed dies forever, which also breaks TrackerWakes), Free (the user ignores every calibration), NewLayout (the known limit) and PreBlind (the code before `d39fbb9`). The model now describes the fixed code: `FitSays` is no longer a switch (the fit's "ok" always counts), the eyes going counts as away, and a restarted ft-gazed shows the screens it finds marked hidden. To see the old breaks, check out `f85dff9` and run its copy of the model.

Z3 isn't in the model. It is covered by `gaze/test/restart-checks-test.py`, with Z1, Z2 and Z4.

The chart (`G4-gaze.mmd`, `.svg`) still draws Z1 to Z4 red. It is due a redraw.

Fixed bugs:

| Id | Ticket | Rule | What happened | Fix |
| --- | --- | --- | --- | --- |
| Z1 | DEE-270 | ScreensComeBack | ft-gazed died during a full calibration. Nothing called `close()`, ft-screens' hide has no lease, and the new ft-gazed never knew, so the screens stayed hidden | Fixed in `32456e1`. A calibration that hides the screens writes `screens-hidden` in the gaze state folder and `close()` removes it. A gaze service that starts and finds it asks ft-screens to show |
| Z2 | DEE-271 | FullOnlyWhenNeeded | Our tracker's first calibration saved while ft-gazed still held ft-eyes' old status. With `full_armed` still set, a second full calibration opened at once | Fixed in `32456e1`. calib-fit's "ok" updates the held status with the captured dots, so the tracker counts as calibrated at once |
| Z3 | DEE-272 | From the code (minor) | Check headset fit asked for while idle waited for a corrected gaze, which our tracker can't send before its first calibration, so it opened only after WAKE_SETTLE (15 s) | Fixed in `32456e1`. The pending fit check waits for the tracker to send anything, not for a gaze |
| Z4 | DEE-273 | CalibrationComes | No eyes for EYES_GONE (2 s) closed the full calibration, but away needs AWAY_MIN (3 s). Eyes back in that second were no don, so nothing re-armed it | Fixed in `32456e1`. A check closed because the eyes went counts the headset as away, so their coming back is a don |

Design choices in these fixes (Daenon can override each):

- Z1: the marker is a file in the gaze state folder (`~/.local/state/frametop/gaze/screens-hidden`), so it outlives a crash but not a wipe of that folder. Since G2's P2 fix (`45685cc`) it holds the mode in effect and ft-screens' switch count after the hide, and a restarted ft-gazed shows the screens only if nothing moved the switch since.
- Z2: the held status is updated from calib-fit's reply until ft-eyes' next status replaces it. The dots count is the one ft-gazed captured.
- Z3: the fit check waits only on the tracker sending, not on the gaze being good.
- Z4: the eyes gone counts as away (the headset taken off), the same as AWAY_MIN would.

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

Results with the fixes (`4e24d91`): every rule holds in all 11 configs, 33 runs. States per run: 64 (Steady, DashFix) to 1,524 (Code). The model now describes the fixed code, with G1's L2 fix (`93cbf29`): `FixPark`, `DashFix` and `L2` no longer switch anything, so the configs that showed one bug each now check the same code with different settings. To see the old breaks, check out `f85dff9` and run its copy of the model.

Undoing a fix in a copy of the model brings its break back: H5-1 breaks TrackerIdleInPause in FreshBoot and Code, H5-3 in Cutouts and Code, and H5-4 breaks both rules in most configs. H5-2 breaks SwitchOff only with H5-3 also undone, because the park now holds the kept tracker anyway. Its fix is a second line of defence: the units stop instead of sitting parked.

The chart (`G5-hands.mmd`, `.svg`) still draws H5-1 to H5-4 red. It is due a redraw.

Fixed bugs:

| Id | Ticket | Rule | What happened | Fix |
| --- | --- | --- | --- | --- |
| H5-1 | DEE-274 | TrackerIdleInPause | A pause before hand tracking ever ran since login. `/run/user/UID/frametop-hands` was missing, so `update_park` wrote no park file but still set `parked`. Tracking that started later in the pause ran unparked through the game | Fixed in `4e24d91`. `update_park` makes the folder (0700, as ft-camd and ft-hands do) and writes the park. It sets `parked` only once the file is written |
| H5-2 | DEE-275 | TrackerIdleInPause (minor) | Hand interaction switched off mid-pause. `set_hands` emptied `keep` and unparked, but nothing stopped the units the pause kept | Fixed in `4e24d91`. The worker runs `enforce`, which stops them as the pause would have |
| H5-3 | DEE-276 | TrackerIdleInPause (minor) | ft-cutouts' transient units aren't in the pause's `UNITS`, and without hand interaction nothing parked them, so the cutouts' tracker ran through VR games | Fixed in `4e24d91`. The park is written whenever Frametop is paused, with hand interaction or not. ft-cutouts runs the same ft-hands with the same run folder, so it parks |
| H5-4 | DEE-277 | TrackerIdleInPause, ParkLifts (minor) | The helper sent `vrdash` only on a change, or every 5 s in a game. A relay that restarted, or one that heard "1" from a helper that went, held a stale copy. Outside a game, a manual pause stayed unparked, or parked under an open dashboard | Fixed in `4e24d91`. The helper sends `vrdash` every 5 s, in a game or not |

Design choices in these fixes (Daenon can override each):

- H5-3: the cutouts' tracker is parked, not stopped. Its transient units can't be started again by the resume, so stopping them would leave the cutouts off after the game. The cutouts show through the screens, which a pause hides anyway.
- H5-2: switching hand interaction off mid-pause stops the hand units then, as a pause with it off would have. Switching it on mid-pause starts nothing until the resume.
- H5-4: the 5 s resend is the one the game case already used. A relay that holds "dashboard open" from a helper that died keeps it until a new helper speaks, at most 5 s after SteamVR is back.
- H5-1: if the folder can't be made, the relay logs it and tries again on its next update, which the helper's `vrdash` brings every 5 s.

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

Results with the fixes (`45685cc`): every rule holds in all 11 configs, 55 runs. States per run: 216 (HeadOff, HeadOffFix) to 2,260 (Code, Fixed). The model now describes the fixed code, with G1's L7 fix (`93cbf29`, the relay tells an out-of-step ft-screens again: `RelayRetells`) and G4's Z1 fix (`32456e1`, the next ft-gazed deals with the screens-hidden mark: `GazedBack`). `HeadFix`, `CalFix` and `StartFix` no longer switch anything, so the configs that showed one bug each now check the same code with different settings. `CalFix` was a different fix (a hide flag of its own in ft-screens); the code took the one below, and `calHide` stays false. To see the old breaks, check out `f85dff9` and run its copy of the model.

Undoing a fix in a copy of the model brings its break back: P1 breaks ShownOnlyWanted in HeadOff, Fixed and Code (and HiddenComesBack where a calibration runs), P2 breaks ShownOnlyWanted in CalInGame, FixedAlways and GazedCrash, the switch count alone breaks the same configs, and P3 breaks VncOffInPause in MidPause, Fixed, FixedAlways and Code.

The first P2 fix (read the mode in effect only) left one path that the model found: a game started, or ft-layout reset the switch, while the calibration ran, and the close's `show` turned the screens on over the game. The switch count closes that path.

The chart (`G2-desktop.mmd`, `.svg`) still draws P1 to P3 red. It is due a redraw.

Fixed bugs:

| Id | Ticket | Rule | What happened | Fix |
| --- | --- | --- | --- | --- |
| P1 | DEE-278 | ShownOnlyWanted | A desktop that started with no head pose. `apply_screens` asked ft-screens for `head` before it sent the visibility mode and conceal, and `start` and `apply --wait` only logged the error, so ft-screens kept its defaults (always, nothing concealed) until a reset, a settings change or a profile | Fixed in `45685cc`. With no head pose, `apply_screens` sends the visibility settings and conceal before it gives up placing |
| P2 | DEE-279 | ShownOnlyWanted (minor) | A full gaze calibration during a VR game, with the always mode and ingames hide. ft-screens' `state` gave `g_mode`, so `gazecheck.py` read the screens as shown, and the `show` at close turned the switch on under the dashboard mode, over the game | Fixed in `45685cc`. gazecheck works out the mode in effect from `state`'s game and ingames fields. ft-screens adds its switch's change count to `state`, and the close (or the next ft-gazed after a crash) shows the screens only if nothing moved the switch since the hide |
| P3 | DEE-280 | VncOffInPause (minor) | A desktop that started mid-pause started remote access with no pause check, so krdp and Xvnc ran through the game | Fixed with G1 in `93cbf29`. The session asks the relay (`ft-pause defer-remote`) before it starts remote access, and while paused the resume starts it instead |

Design choices in these fixes (Daenon can override each):

- P1: placing still needs a head pose. Only the settings go without one, so the screens keep their last place until a reset.
- P2: the calibration leaves the screens alone at its close when anything moved the switch during it, including the user's own hotkey. Whatever moved it last decides.
- P2: `state` gains a tenth field (the switch's change count). Only gazecheck reads `state` by position. `report.sh` and `keys-report.py` print it as is. A new ft-screens starts the count at 0, so a mark left before an ft-screens restart can in rare cases match a new count and show the screens once.

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

Results with the fixes (`be6480e`): every rule holds in all 11 configs, 56 runs. States per run: 26 (NoDeath) to 98 for Verify, 96 (LiveClean) to 644 (LiveAll) for Guard. The Verify model now describes the fixed code: `TtlEnds` (the proxy's override TTL) replaces `LeaseEnds`, and `StatusPoll` restarts nothing. `FixLease` and `FixPoll` are no longer used, so Code, Overrides, OverridesFix, Polls and Fixed check the same code. `FixLease` was a different fix (overrides bound to the verify run's lease); the code took a TTL inside the proxy, which needs no lease and also covers runs that never took one. The Guard wrapper adds `TtlEnds` to the skill's `Next` with strong fairness (the TTL's clock runs through the proxy's stalls, so a proxy that is up again and again acts on it), and its configs now also check the skill's `NoHostTakeover` and `FakeIsNoticed`. `FakeWornGuard.tla` is unchanged. To see the old breaks, check out `f85dff9` and run its copy of the models.

Undoing a fix in a copy of the models brings its break back: without `TtlEnds` (and its fairness), OverridesEnd and WearerGetsReal break in every Verify config but NoDeath. With a status poll restarting the hour again, FakesEnd and WearerGetsReal break in Code, Polls and Fixed. Without `TtlEnds` in the Guard wrapper, GuardRestoresReal breaks in LiveNoF and LiveAll, and it breaks there too with weak fairness in place of strong: a proxy that stalls just before each repeated `worn off` never stays up long enough for weak fairness to apply.

V4 and V5 aren't modelled. `input/test/hostin-test.py` checks V4 (a `hostbye` mid-drag sends `btn trigger 0`), and `hands/rec/tests/test_validate.py` checks V5. Both fail on the old code.

The chart (`G8-verify.mmd`, `.svg`) still draws V1 to V5 red. It is due a redraw.

Fixed bugs:

| Id | Ticket | Rule | What happened | Fix |
| --- | --- | --- | --- | --- |
| V1 | DEE-281 | OverridesEnd, WearerGetsReal | The input proxy's overrides (worn on or off, head pose, HMD buttons, synthetic controllers) had no time limit, and a real wearer didn't end them (`testbench/native/input_state.cpp`). Only `frame-testbench release`, `verify-frametop cleanup` or a SteamVR restart did. A run whose agent died kept them, and someone who put the headset on later got a frozen head pose, or standby under worn off | Fixed in `be6480e`. The proxy ends every override after 300 s with no command other than `status` (`FRAME_TESTBENCH_OVERRIDE_TTL` in vrserver's environment changes it). A release the proxy refuses is tried again every second. `status` reports `override_ttl` and `override_idle` |
| V2 | DEE-282 | FakesEnd, WearerGetsReal | Any command restarted the fake trackers' idle hour, `status` too (`fakes.py`), so a later agent's doctor and status checks kept a dead run's fake eyes and parked hands alive | Fixed in `be6480e`. A `status` question no longer counts as a command for the idle hour |
| V3 | DEE-283 | GuardRestoresReal (minor) | A guard that died while the input proxy stalled. `after_guard` tried the release 3 times 0.2 s apart, then gave up (`hostinput.py`), and the faked wearer stayed until the next `frame-testbench` command | Fixed in `be6480e` through V1: the proxy's TTL ends the faked wearer once it runs again. `after_guard`'s retries and the flag are unchanged, so NoHostTakeover still holds |
| V4 | DEE-284 | not modelled (minor, G3 side) | In pointer mode the relay's drop of a host mouse let go of its buttons only on the virtual mouse (`release_held`, `input/input-relay.py`). A testbench killed mid-drag, or a release read after `hostbye`, left the 3D mouse's button down until the next click | Fixed in `be6480e`. Each device remembers the pointer-mode actions its buttons hold, and its drop releases them through the same path, unless another device holds the same action. `testbench/docs/desktop.md` Limits now says so |
| V5 | DEE-285 | not modelled (minor) | The hand upload's draft gate opened on `FT_HANDREC_ALLOW_UPLOAD=1` alone (`hands/rec/hub.py`), while the eye upload's opened only for another dataset (`gaze/rec/hub.py`), and a missing text counted as final (`recorder/hub.py` `is_draft`). All five consent texts are final today, so nothing could leak | Fixed in `be6480e`. The hand gate opens a rehearsal only with the kind's dataset set to a test one (`FT_HANDREC_DATASET`, or `FT_HANDREC_TEST_DATASET` for testing), and a missing consent or upload text counts as a draft. `rehearse.sh` passes its `--repo` to the check, and the rehearsal banner shows only when uploads are allowed |

Design choices in these fixes (Daenon can override each):

- V1: the TTL is 300 s. It covers an agent's normal gaps between proxy commands, a build or long thinking included, and a wearer who puts the headset on after a dead run gets their head and proximity back within 5 minutes instead of never. Status polls don't renew it, so a doctor or the MCP's status tool can't keep a dead run's overrides alive. An agent that stops for longer than 5 minutes loses its overrides and sets them again with its next command. A looped replay renews it through its pose commands.
- V1: the TTL lives in the proxy, not in `verify-frametop`'s lease, so it also covers `frame-testbench` and MCP use outside a verify run.
- V2: the fakes keep their idle hour; only `status` stops restarting it.
- V3: no new retry loop in `after_guard`. The TTL bounds the stalled case at 300 s after the proxy runs again.
- V4: a dropped device's held actions go up only when no other device holds the same action, so a real mouse's drag survives the testbench mouse leaving.
- V5: only the hand gate changed. `recorder/hub.py` `is_draft` still reads a missing file as not a draft, since the shared window uses it for its banners; the Eye Recorder already checks that its texts exist.

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

Results with the fixes (`140aced`): 72 runs. Every rule holds in CIOnly, Local, Skips, Rollback, Reinstall and Fixed. Code, Interrupt and FixedInterrupt still break NoMixedUnits (7 steps), CurrentIsLive (3 steps) and PatchLands (8 steps), all on W5's path; UnitsHaveFolder, ReleaseMatchesCommit and ReinstallGivesRelease now hold there. States per run: 116 (CIOnly, Local) to 15,049 (Code, FixedInterrupt). The model now describes the fixed code: a tree's `install.sh` points `current` at itself when it finishes (W1), a part an install skips has its units removed when they point into another release (`drops`, W2), a local pack can't make a release from a changed tree, so `LocalBuild` adds no states (W3), and a folder with fast-installed files can't be installed over (W4), so a re-run after an interruption comes after a `fast-install undo`. `FixCurrent`, `FixSkip`, `FixReinstall` and `FixLocal` are no longer used, so Code and FixedInterrupt check the same code. `FixSkip` and `FixReinstall` were different fixes (renew the skipped part's units, copy the folder again); the code took the ones that change less (below). `framedrop/build.sh --allow-changes`, for local test releases, isn't modelled. To see the old breaks, check out `f85dff9` and run its copy of the model.

Undoing a fix in a copy of the model brings its break back: with a tree's `install.sh` leaving `current` alone, CurrentIsLive (12 steps) and PatchLands (13 steps) break in Rollback. Keeping a skipped part's units in another release, UnitsHaveFolder (12 steps) and NoMixedUnits (8 steps) break in Skips. With a changed tree packed, ReleaseMatchesCommit breaks in Local (1 step). Without the refusal, ReinstallGivesRelease breaks in Reinstall (9 steps), and with a re-run that skips the undo it breaks in Interrupt and FixedInterrupt (11 steps).

`pack/test/install-release-test.py` checks W1 and W4 in `install-release.sh` offline, and that the release `current` named before is still kept now that `install.sh` moves the link first. It fails on the old `install-release.sh`. W2 needs a Frame, so it has no offline test. W3 was checked with a stub podman: a clean tree at HEAD packs, a changed tree, another commit or an image with no labels is refused, and `--allow-changes` warns and packs. CI builds with `./ft dev build` from a clean checkout and passes `--commit "$GITHUB_SHA"`, so its images pass.

`G7-release-traces/` still holds TLC's output for every break at `78e93b1`, the old code. These results were run from a copy of the model, so the folder stays that record. Running the check here rewrites its W5 traces with the fixed model's. The chart's W2 and W4 legend boxes say 9 and 8 steps, one short.

The chart (`G7-release.mmd`, `.svg`) still draws W1 to W4 red. It is due a redraw.

Fixed bugs:

| Id | Ticket | Rule | What happened | Fix |
| --- | --- | --- | --- | --- |
| W1 | DEE-286 | CurrentIsLive, PatchLands | Only `install-release.sh` moved `releases/current`, after `install.sh`. Going back by running the older release's `install.sh`, as `pack/README.md` and `install-release.sh` said, moved the units and left `current` on the newer release. `fast-install` then patched `current`'s tree, found no unit in it or restarted units that ran the other tree, and reported success. The next install kept the wrong release as the one before, and `get.sh` read the channel from it | Fixed in `140aced`. A release's `install.sh` points `current` at its own folder when it finishes (only a release folder named for its `VERSION`). `install-release.sh` reads the release `current` named before it runs `install.sh`, so the release before is still kept |
| W2 | DEE-287 | UnitsHaveFolder, NoMixedUnits | A part a later install skipped (voice or hands unticked, `--no-voice`, `--no-hands`, or `--yes` with no quiet sudo) kept its units in the old release's folder and container. Two releases on, `install-release.sh` removed that folder and container, and those services failed | Fixed in `140aced`. When an install skips voice or hand tracking, or its install fails, that part's units that point into another Frametop folder are stopped and removed (with the `ft-handsctl` link), and the message says how to install it for this folder. Units already in this folder stay. The Hand Recorder has its own ft-camd unit, so it is unaffected |
| W3 | DEE-288 | ReleaseMatchesCommit (local builds only) | `ft dev build` built the image from the working tree, uncommitted and untracked files in, and `framedrop/build.sh` named HEAD at pack time as its commit. `fast-install` diffs from that commit and trusts it for its rebuilds | Fixed in `140aced`. `ft dev build` labels the image with its commit and whether the tree had changes (`org.frametop.commit`, `org.frametop.tree`). `framedrop/build.sh` refuses an image built from a changed tree, from a commit other than `--commit`, or with no labels. `--allow-changes` packs it anyway, with a warning |
| W4 | DEE-289 | ReinstallGivesRelease | Installing the same release again skipped the copy when its folder was there, so fast-installed files and `.fast-install/` stayed in a tree that named the release's commit | Fixed in `140aced`. `install-release.sh` refuses to install over a folder with `.fast-install/state.json`, and says to run `scripts/fast-install undo` first (after that release's `install.sh`, if another one is current) |

Open suspected bugs:

| Id | Ticket | Rule | What happens | Why it's still open |
| --- | --- | --- | --- | --- |
| W5 | no ticket yet | NoMixedUnits, CurrentIsLive, PatchLands (minor) | `install.sh` runs 12 steps under `set -e`. A failing step or a power-off leaves the early steps' units in the new release and its container, the later ones in the old release, and `current` on the old one. `fast-install` then patches the old tree. Running the same command again finishes it | Documented, not fixed. A safe fix needs a transactional install (every unit written for the new release first, then one switch), which reworks all 12 steps and can't be checked without running an install. `install.sh` now says "Run the same command again to finish the install" when it stops early, and `pack/README.md` (step 6), `README.md` (Update) and `install-release.sh`'s header say the same |

Way around it until it is fixed: run the same command again.

Design choices in these fixes (Daenon can override each):

- W1: only a release's `install.sh` moves `current`, and only when its folder's name is its `VERSION`. A clone's `install.sh` leaves the link alone, since `current` must name a release folder.
- W2: a skipped part's units from another folder are stopped and removed, not renewed for this folder (that would install a part the person said no to, or one that needs sudo) and not kept with their old folder (that would leave units across releases and a folder that has to outlive its release). The part stays off until it is installed again, and the message says how.
- W3: refused by default, with `--allow-changes` as the way out for local test releases. An image with no labels (built before this change, or by another tool) is refused too: rebuild it with `ft dev build`, or pass `--allow-changes`.
- W4: a reinstall over fast-installed files is refused rather than copied again. Copying again would quietly drop the fast-installed commits, and a fresh copy loses ft-camd's file capabilities and the tree's own models until `install.sh` sets them again.
- W5: documentation and an exit message only (see above).

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
