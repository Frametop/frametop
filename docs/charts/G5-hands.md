# G5 Hands

The group's notes, from [the charts' README](README.md): what the chart and the model cover, their configs, and what's held by tests instead.


Covers ft-camd and ft-hands (how they start, restart and stop), when hand tracking runs and when the pause parks it, the input relay's park state and the helper's dashboard word, the hand or controller role on each side (`handoff.h`), near input on ft-screens' panels, and the hand ray and pinches. The pipeline from XRService's cameras to the readers of `/run/user/UID/frametop-hands` is drawn by hand.

Model: `G5-hands.tla` (module `G5Hands`). Configs: `Steady` (hand interaction on, the run folder there), `FreshBoot` (a pause before hand tracking ever ran), `FreshBootFix` (the same with the park remembered only once written), `SwitchOff` (hand interaction switched off mid-pause), `Cutouts` (ft-cutouts without hand interaction), `Restarts` (the helper and the relay can restart), `RestartsFix` and `DashFix` (the dashboard word resent after a restart), `NoHands` and `NoHandsL2` (hand interaction off, with G1's L2), `Code` (every fault on). Each fix config switches off one bug, to show that bug alone breaks the rule.

Results at `78e93b1`:

| Rule | What it says | Result |
| --- | --- | --- |
| TypeOK | | Holds in all 11 configs |
| TrackerIdleInPause | While paused with the dashboard closed, a running hand tracker soon stops or parks | Breaks in FreshBoot, 3 steps (H5-1), SwitchOff, 4 steps (H5-2), Cutouts, 4 steps (H5-3), Steady and Restarts (H5-4), NoHandsL2 (G1's L2) and Code. Holds in FreshBootFix, DashFix, RestartsFix and NoHands |
| ParkLifts | A park on a running tracker lifts once the pause ends or the dashboard opens | Breaks in Restarts, 6 steps (H5-4) and Code. Holds in every config without relay restarts |

States per run: 64 (Steady, DashFix) to 1,741 (Code).

Not in the model yet: the Hand Recorder's `needed` file (`hands/rec/handsneeded.py`), fresh while it records, over which the relay's park doesn't hold (the testbench's and another producer's still do: `hands/track/park.cpp`, tested in `hands/tests/park_test.cpp`), and ft-hands letting go of ft-npud while parked. TrackerIdleInPause doesn't hold while the recorder records in a pause, by design.

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
