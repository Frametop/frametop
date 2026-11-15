# G2 Desktop and panels

The group's notes, from [the charts' README](README.md): what the chart and the model cover, their configs, and what's held by tests instead.


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
