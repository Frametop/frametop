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
| Frametop Together: its units, a room and who is in it, the links between its people, away, voices and mouths, shared panels, control of a panel and the turn on a desktop, pointers, where people look, moving through the room and moving panels, the avatars people have on, a split compute PC's panels | G9 Frametop Together | `G9-together.mmd`, `G9-together.tla` |

All nine groups are charted. G9 lives on the branch `dee-193-together` until Frametop Together is merged, and was drawn from the code at `5496ae9` (2026-11-12).

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

In [G1-lifecycle.md](G1-lifecycle.md): what the chart and `G1-lifecycle` model cover, and their configs.

## G6 Remote displays and host input

In [G6-remote.md](G6-remote.md): what the chart and `G6-remote` model cover, and their configs.

## G3 Input and pointer

In [G3-input.md](G3-input.md): what the chart and `G3-input` model cover, and their configs.

## G4 Gaze

In [G4-gaze.md](G4-gaze.md): what the chart and `G4-gaze` model cover, and their configs.

## G5 Hands

In [G5-hands.md](G5-hands.md): what the chart and `G5-hands` model cover, and their configs.

## G2 Desktop and panels

In [G2-desktop.md](G2-desktop.md): what the chart and `G2-desktop` model cover, and their configs.

## G8 Verify, data and diagnostics

In [G8-verify.md](G8-verify.md): what the chart and `G8-verify` model cover, and their configs.

## G7 Build and release

In [G7-release.md](G7-release.md): what the chart and `G7-release` model cover, and their configs.

## G9 Frametop Together

In [G9-together.md](G9-together.md): what the chart and `G9-together` model cover, and their configs.

## Running a check

The checks need TLC 1.7.4 and Java. They look for TLC at `~/.cache/frametop-formal/tla2tools-1.7.4.jar` (the verify skills' `scripts/check` put it there), or at `TLA2TOOLS` for every group but G1. Java comes from `JAVA`, then `PATH`. G1 asks mise for `java@temurin-25`.

A check keeps a core busy for seconds to minutes, so run it through `frame-job` on the Frame, at the lowest priority. Without `frame-job` (anywhere but the maintainer's machines), run the same command without the `frame-job --local --` part:

```
cd docs/charts
J=$(mise which java --tool java@temurin-25)
frame-job --local -- nice -n 19 env JAVA=$J PATH=$(dirname $J):$PATH python3 G4-gaze-check.py           # every config and rule
frame-job --local -- nice -n 19 env JAVA=$J PATH=$(dirname $J):$PATH python3 G4-gaze-check.py Gaze:Code  # one model and config
frame-job --local -- nice -n 19 env JAVA=$J PATH=$(dirname $J):$PATH python3 G1-lifecycle-check.py PauseHide
```

G3 takes `Keys`, `Laser`, `Keys:Host` and so on. G5 takes `Hands:FreshBoot` and so on. G2 takes `Desktop:HeadOff` and so on. G8 takes `Verify`, `Guard`, `Verify:Overrides`, `Guard:LiveNoF` and so on, and reads the guard model from the verify-fake-worn-guard skill. G6 takes `Exit`, `Live` or `PreFix`. G9 takes `Room`, `Full`, `Reopen`, `Live` or `PreFix`, checks a config's rules in one run, and takes about half an hour on the Frame for all five (`Room`, `Reopen` and `Live` together about nine minutes). G1 and G7 take config names, such as `Rollback`.

Each script writes TLC's full output, with every counterexample trace, to `GROUP-traces/` next to the models. Those folders are ignored by git. A rule that breaks prints its shortest path, and the chart's red badges follow that path.

When code in a group's area changes, re-run its check. A rule that changes from breaks to holds means a bug was fixed: move its badge to green with the commit. A rule that changes from holds to breaks is a regression, or the model needs the new behaviour.
