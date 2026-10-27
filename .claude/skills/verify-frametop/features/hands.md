# Hand tracking

Frametop tracks the user's hands from the headset cameras (ft-hands): ft-handview draws them
over the panels (a robot or a hand model; the default since 2026-10-09), or with the cutouts on
ft-screens cuts a hole through the panels where they are, a pinch clicks where the pointer
aims, and a grip grabs. The testbench parks ft-hands and publishes posed hands in its place.

## Sub-features

- `hand-pose` shows a posed hand where it is (ft-handview's drawn hand).
- `hand-cutout` cuts the panel behind the hand (off by default; Room View on).
- `hand-pinch` clicks: a pinch begun and ended.
- `hand-grip` grabs with a fist.
- `hand-release` hands tracking back to the cameras.

## How to get to it (user POV)

- Hold a hand up in front of the headset; pinch thumb and index to click.

## Driving it with the testbench

Preconditions:

- Hand tracking runs: `V doctor`'s `hand tracking` check is ok.
- `T worn on`, `T compositor awake`, head pose facing the panels.

- **Pose a hand.** Run `T hand right set --position .1 -.1 -.4 --shape open` (head frame:
  +x right, +y up, -z ahead), wait 1.5 s, `V snapshot 2-hand --capture`.
  `testbench.fakes.hands_publishing` is `true` (and `hands_waiting` absent: ft-hands has
  parked), and the capture shows the drawn hand there. Move it (`--position .15 -.05 -.4
  --shape point`) and capture again: the hand follows, index out (proved 2026-10-27).
- **Cutout.** Put the hand between the eyes and a Frametop panel: `T pointer measure KEY` gives
  its centre in standing space; a remote display at 26 degrees up worked with
  `T pose set --position 0 1.1 0 --rotation 0 15 0` and the hand at `0 .082 -.4`. Snapshot with
  `--capture`, then `V ask ft_screens 'cutouts on'` and `V ask ft_screens 'cutouts roomview on'`
  (ft-screens cuts only with Room View on), wait 1.5 s and capture: a round hole through the
  panel around the hand, the room behind it. Put both back at once: `V ask ft_screens 'cutouts
  roomview auto'` and `V ask ft_screens 'cutouts off'` (if `cutouts state` said `ok off` and
  `room view ... (no game, ...)` before; `(forced)` means it was forced already).
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
- The pointer's overlay list leaves out the drawn hands (`frametop.handview.*`) on purpose, and
  `V ask ft_handview state` is the last minute's summary, not now: the capture is the readback.
- A floating window (`frametop.float.N`) showed no hole on 2026-10-27; prove cutouts on a screen
  or remote display.
- A cutout capture shows the room: it never leaves this machine without the user's yes.
