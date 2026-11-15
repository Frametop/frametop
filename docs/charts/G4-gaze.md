# G4 Gaze

The group's notes, from [the charts' README](README.md): what the chart and the model cover, their configs, and what's held by tests instead.


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
