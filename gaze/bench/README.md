# Gaze Bench

A test bench for gaze tracking. It shows dots in the places gaze tracking finds hard (far out
toward the edges and corners, long holds, light changes, a moving head, after the headset moved).
You look at each one and press the headset's button. Every press is recorded with everything the
trackers saw, so gaze techniques can be compared afterwards on the same looks (`compare.py`),
including ones that didn't run live.

The dots show on a panel in the headset, 64 degrees across, that moves with your head: a dot is
always at the same direction from your head, wherever you face. So every run asks the same of the
eyes, and you never have to turn your head to find a target. Your eyes do all the looking.

## Use

1. Put the headset on and open **Frametop Gaze Bench** from the app menu (`gaze/bench/install.sh`
   adds it), or run `gaze/bench/ft-gazebench` from Frametop's desktop. A window opens; its
   status lines say whether the headset panel, the headset button and the trackers are ready.
   Gaze mode can be on or off: the bench keeps the trackers running itself.
2. Pick a scenario. The panel shows what to do; press the button on the headset's right side
   to begin.
3. For each dot: look at it and press the button. The dot turns green and the next one shows.
   Where you looked is the dot, so only press while you're on it.
   Two presses take the last one back. Holding the button ends the scenario.
   As in the gaze calibration, a left click, Enter or an index pinch (index to thumb, with hand
   tracking) marks too, and a right click or Meta+K ends: while the panel shows, the bench claims
   the pointer helper's calibration panel (`calpanel 1 ft_gazebench.PID`), so the helper sends
   those here and hides the pointer. Those marks are recorded `"by": "helper"`, at the time the
   helper's answer came in (a pinch's a frame or two after it began).
   The window's keys do the same: Space marks, Backspace takes the last one back, S skips a
   dot, Esc ends.
4. At the end, the panel and the window say how far our tracker and SteamVR's were off. A press
   (or a click, Enter or a pinch) closes the panel. Then run `compare.py` on the runs.

| Scenario | What it tests |
|---|---|
| Warm-up | nine dots around the middle |
| Edges | sixteen dots along the panel's edges and in its corners, 24 to 37 degrees out |
| Hold at the edges | edge dots you look at for 3 s before pressing (the tracker loses an eye out there) |
| Out from the middle | twenty-four dots on eight spokes, at a third, two thirds and all the way out |
| Head moving | nine dots; keep looking while you turn or nod your head, press while it moves |
| Light changes | dark, bright and grey backgrounds: pupil size |
| Small steps | twenty dots, each 2 to 4 degrees from the last |
| After a nudge | lift the headset and set it back, then nine dots: recovery |
| Long session | sixty dots anywhere at your own pace, a few seconds apart, about six minutes: drift and stale corrections |

Scenarios live in `scenarios.json` (the generators are described at its top).

## Compare

```
scripts/in-box /opt/frametop/venv/bin/python gaze/bench/compare.py ~/.local/share/frametop/gaze/bench
```

Give it runs, or the folder they're in. For every press it prints how far each technique was from
where you looked: overall, by scenario, by where the dot was (edge, corner, inside), by how far
from straight ahead, and by the time since the last nudge. `--per-click` adds a line per press;
`--json OUT` writes the records. The techniques are described at the top of `compare.py`: what
ran live (our tracker, SteamVR's combined gaze and its two eyes averaged, a 70/30 mix, SteamVR's
corrected by the presses), SteamVR's eyes corrected by the run's `fusion.json` (copied into
`start/` when the run begins) and the fusion with ours, the same fitted on the run's other presses
(`-loo`) or on the other runs only (`-xrun`), and replays of our classic tracker on its recorded
finds with changed rules (no glint jump, a jump only when both eyes see it, no lessons past the
calibrated range, learning from every press, bridging a lost eye, the size guard's two rules or
none). `fusion-loo:REPLAY` fuses a replay with SteamVR's.
`--fit-fusion OUT` fits SteamVR's correction on all the given runs' presses and saves it as a
`fusion.json` the gaze service reads (`~/.local/state/frametop/gaze/fusion.json`). The replay of the live rules should land close to what our tracker published (the
"replay vs live" line); the variants are only as trustworthy as that.

Runs from before 2026-10-17 showed their targets on a Frametop screen, with clicks. `compare.py`
still reads them: their truth comes from ft-gaze's screen hits, and they have a "pointer"
technique (where the gaze pointer was).

A long session's run (gaze.jsonl and own.jsonl) takes a while to replay: run it through
`frame-job` when the headset is in use.

## What a run records

`~/.local/share/frametop/gaze/bench/<time>-<scenario>/` (`benchrec.py` describes every file):
the dots and the presses, the panel's log of what the dot did and when it reached the eyes,
the headset's pose 250 times a second, ft-gaze's samples of every source except SteamVR's input
action (the one that costs vrserver anything), our tracker's per-frame pupil and glint finds,
and our tracker's and the gaze service's calibration and state at the start. With **Record the
eye cameras** on (the default), both eye cameras too, as the Eye Recorder records them: about
1.2 GB a minute.

Eye images identify a person. They stay on the headset: the bench uploads nothing, and the runs
are yours to delete (the folder above).

## Files

| File | |
|---|---|
| `gazebench.py`, `bench.qml` | the app and its window (PySide6, in Frametop's container); it runs its own `ft-gazepanel` |
| `benchrec.py` | the recording |
| `scenarios.json` | the scenarios |
| `compare.py` | the comparison (numpy, OpenCV) |
| `ft-gazebench`, `ft-gazebench.desktop`, `install.sh` | the launcher and its menu entry |
| `tests/` | `test_bench.py` (runs with stand-ins for ft-gaze, the panel, the button and the services), `test_compare.py` (a made-up run with known errors) |

Tests: `scripts/in-box python3 gaze/bench/tests/test_bench.py`, and
`scripts/in-box /opt/frametop/venv/bin/python gaze/bench/tests/test_compare.py`.
