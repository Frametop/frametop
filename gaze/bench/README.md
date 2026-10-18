# Gaze Bench

A test bench for the gaze pointer. It shows targets in the places gaze tracking finds hard (the
screen's edges and corners, eyes-only looks, long holds, light changes, after the headset moved).
You look at each one and click what you're really looking at. Every click is recorded with
everything the trackers saw, so gaze techniques can be compared afterwards on the same looks
(`compare.py`), including ones that didn't run live.

## Use

1. Put the headset on, turn gaze mode on, and open **Frametop Gaze Bench** from the app menu
   (`gaze/bench/install.sh` adds it), or run `gaze/bench/ft-gazebench` from Frametop's desktop.
   It opens full screen on the screen it starts on.
2. Pick a scenario. Each one starts with what to do; click to begin.
3. For each target: look at it. The pointer goes where the tracker thinks you look. If it's off,
   nudge it onto the target (gaze mode's left button: press, steer with the mouse, let go), then
   click. If you were really looking somewhere else, click that instead: a click more than
   two target radii from the target counts as where you looked.
   Right click or S skips a target, Backspace takes the last one back, Esc ends the scenario.
4. At the end, the window says how far our tracker and SteamVR's were off. Then run
   `compare.py` on the runs.

| Scenario | What it tests |
|---|---|
| Warm-up | nine targets in the middle |
| Screen edges | sixteen targets along the edges and in the corners |
| Hold at the edges | edge targets you look at for 3 s before clicking (the tracker loses an eye out there) |
| Eyes only | head facing the cross, eyes out to the edges (a blue ring shows where the head faces) |
| Turn to it | the same places, head turned to each |
| Light changes | dark, bright and grey backgrounds: pupil size |
| Small and close | two targets 40 px apart: click the filled one |
| After a nudge | lift the headset and set it back, then nine targets: recovery |
| Long session | sixty targets anywhere at your own pace, about six minutes: drift and stale corrections |

Scenarios live in `scenarios.json` (the generators are described at its top).

## Compare

```
scripts/in-box /opt/frametop/venv/bin/python gaze/bench/compare.py ~/.local/share/frametop/gaze/bench
```

Give it runs, or the folder they're in. For every click it prints how far each technique was from
where you looked: overall, by scenario, by where the target was (edge, corner, inside), by how
far from straight ahead, and by the time since the last nudge. `--per-click` adds a line per
click; `--json OUT` writes the records. The techniques are described at the top of `compare.py`:
what ran live (our tracker, SteamVR's, the pointer, a 70/30 mix, SteamVR's corrected by the
clicks), and replays of our classic tracker on its recorded finds with changed rules (no glint
jump, a jump only when both eyes see it, no lessons past the calibrated range, learning from every
click, bridging a lost eye). The replay of the live rules should land close to what our tracker
published (the "replay vs live" line); the variants are only as trustworthy as that.

A long session's run (gaze.jsonl and own.jsonl) takes a while to replay: run it through
`frame-job` when the headset is in use.

## What a run records

`~/.local/share/frametop/gaze/bench/<time>-<scenario>/` (`benchrec.py` describes every file):
the trials and clicks, ft-gaze's samples of every source except SteamVR's input action (the one
that costs vrserver anything), our tracker's per-frame pupil and glint finds, the lessons the
nudges sent, and our tracker's and the gaze service's calibration and state at the start. With
**Record the eye cameras** on (the default), both eye cameras too, as the Eye Recorder records
them: about 1.2 GB a minute.

Eye images identify a person. They stay on the headset: the bench uploads nothing, and the runs
are yours to delete (the folder above).

## Files

| File | |
|---|---|
| `gazebench.py`, `bench.qml` | the window (PySide6, in Frametop's container) |
| `benchrec.py` | the recording |
| `scenarios.json` | the scenarios |
| `compare.py` | the comparison (numpy, OpenCV) |
| `ft-gazebench`, `ft-gazebench.desktop`, `install.sh` | the launcher and its menu entry |
| `tests/` | `test_bench.py` (a run with stand-ins for ft-gaze and the services), `test_compare.py` (a made-up run with known errors) |

Tests: `scripts/in-box /opt/frametop/venv/bin/python gaze/bench/tests/test_bench.py`, and the same for `test_compare.py`.
