# G1 Lifecycle and presence

The group's notes, from [the charts' README](README.md): what the chart and the model cover, their configs, and what's held by tests instead.


Covers SteamVR and the units that start and stop with it, the desktop session, the input relay's pause for VR games (auto, gesture, key, resume when the game ends, the state file across relay restarts), the pointer helper's worn answer, and ft-powerd's display power, with the standby it asks SteamVR for (`DISPLAY_OFF_STANDBY`).

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
| WornAgreesSoon, DarkHasOwner, AwayReleasesPointer | Worn readers agree within 1.5 s, dark displays have an owner, away lets go of the pointer | Hold (Presence 162 states then, Joint 34,942) |

States per run: 27,172 (PauseHide) to 34,942 (Joint).

Results with the fixes (`93cbf29`): every rule holds in every config, 66 runs. States per run: 1,856 (PauseLossy) to 32,770 (Joint); Presence was unchanged at 162 (more since the forced standby, below). The model now describes the fixed code. To see the old breaks, check out `f85dff9` and run its copy of the model.

With L7's fix, ft-screens out of step with the relay is a transient, like a restarting unit: its next report (`ScreensReport`) corrects it, and `Quiet` waits for that report. So in G1, DesktopMatchesPause no longer catches a lost datagram on its own. G2's liveness rule HiddenComesBack carries that check.

The chart (`G1-lifecycle.mmd`, `.svg`) still draws L1 to L7 red. It is due a redraw.

Forced standby (2026-11-24): the model has the proximity sensor (`near`) apart from the level SteamVR has (`lvl`), because ft-powerd can now write that level through the `ft_pointer` driver, and ft-powerd's `Standby` with its file `powerd-standby`. Two rules came with it, in `Presence` (325 states) and `Joint` (32,770): StandbyHasOwner (a standby ft-powerd asked for is one a key will end: ft-powerd runs and knows, or its file tells the next start) and StandbyFileIsFresh (the file is there only while a standby is under way or its ft-powerd was killed). Both hold, with the three older presence rules. The `.mmd` has the new states (x3, x4); the `.svg` and the preview don't yet.

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
