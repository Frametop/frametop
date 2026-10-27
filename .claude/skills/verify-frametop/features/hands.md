# Hand tracking

Frametop tracks the user's hands from the headset cameras (ft-hands): their cutouts show over
the panels, a pinch clicks where the pointer aims, and a grip grabs. The testbench parks
ft-hands and publishes posed hands in its place.

## Sub-features

- `hand-pose` shows a posed hand where it is.
- `hand-pinch` clicks: a pinch begun and ended.
- `hand-grip` grabs with a fist.
- `hand-release` hands tracking back to the cameras.

## How to get to it (user POV)

- Hold a hand up in front of the headset; pinch thumb and index to click.

## Driving it with the testbench

Preconditions:

- Hand tracking runs: `V doctor`'s `hand tracking` check is ok.
- `T worn on`, `T compositor awake`, head pose facing the panels.

- **Pose a hand.** Run `T hand right set --position .1 -.2 -.35 --shape open`, wait 1 s,
  `V snapshot 2-hand`. `testbench.fakes.hands_publishing` is `true` (and
  `hands_waiting` absent: ft-hands has parked).
- **Pinch reaches the pointer.** Run `T hand right set --position .1 -.2 -.35 --shape pinch`,
  wait 0.3 s, then the same with `--shape open`. `journalctl --user -u frametop-pointer --since
  -1min` has `pinch right: ignored (no ray up: ...)`: without gaze mode and the hand's ray down, a
  pinch only wakes the pointer, by design (`POINTER_PINCH_NO_RAY` is off).
- **Pinch clicks at the gaze.** Turn gaze mode on (`G on`), aim the eyes at a harmless
  spot (`T gaze look YAW PITCH`, see `gaze-pointer.md`), and pinch as above. The log line reads
  `pinch right: clicked at the gaze on KEY ...`; a capture shows the click's effect.
- **Grip.** Same with `--shape fist`, then `open`.
- **Release.** Run `T hand all release`: within 3 s ft-hands unparks and publishes camera
  tracking again (`hands_publishing` false).

## Gotchas

- `hands_waiting: true` means ft-hands hasn't parked yet: wait, or check that the installed
  ft-hands has the testbench park (it parks while `$XDG_RUNTIME_DIR/frametop-testbench/hands`
  is under 3 s old).
- A pinch while the headset reads as off is logged as ignored: set `worn on` first.
- A pinch that clicks without gaze mode needs the hand's ray up, which a posed hand doesn't raise
  yet (2026-10-27): prove ray clicks with a headset test.
- A pinch that isn't ended stays held: always end it with `--shape open`.
- How tracking feels, and the cutouts' fit to real hands, need a headset test.
