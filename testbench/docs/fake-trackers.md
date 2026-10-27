# Fake eye and hand trackers

The testbench can stand in for the eye trackers and the hand tracker at Frametop's boundary.
Everything from ft-gaze and ft-hands' readers on then runs as with real eyes and hands: the gaze
pointer, gaze calibration's dot logic, the hand cutouts, pinch clicks and grips. No cameras are
involved, and no camera images are made or stored.

```sh
./frame-testbench gaze look 10 -5             # both eyes, head-relative degrees: yaw +left, pitch +up
./frame-testbench gaze look 10 -5 --right 6 -5   # the right eye apart (YAW PITCH = the left eye)
./frame-testbench gaze eyes open lost         # each eye open, closed or lost (the tracker can't see it)
./frame-testbench gaze release

./frame-testbench hand right set --position .1 -.2 -.35 --shape pinch
./frame-testbench hand right set --position .1 -.2 -.35 --shape open    # the pinch ends: one click
./frame-testbench hand left set --position -.1 -.2 -.35 --rotation 0 0 90 --shape fist
./frame-testbench hand all release

./frame-testbench release                     # also stops the fake trackers
```

Real tracker output can be recorded and replayed too (see below).

## How it works

The first `gaze look` or `hand ... set` starts `python3 -m frame_testbench.fakes` as the transient
user unit `frame-testbench-fakes` (`journalctl --user -u frame-testbench-fakes` for its log). It
takes commands on `$XDG_RUNTIME_DIR/frame-testbench-fakes.sock`, a user-only socket, and writes:

- **Eyes**, 90 times a second, to `$XDG_RUNTIME_DIR/frametop-testbench/`: `eye-server.mmap` in
  SteamOS 0.4's layout of SteamVR's file, and `frametop-eyes-gaze` in our tracker's (ft-eyes).
  ft-gaze reads a stand-in instead of the real `/dev/shm` file while it was written in the last
  2 s, and goes back to the real one by itself when it stops. The real files are never written.
- **Hands**: it touches `$XDG_RUNTIME_DIR/frametop-testbench/hands` every second, which parks
  ft-hands (no tracking, ft-camd stops copying frames) while it's under 3 s old. 0.6 s later it
  starts publishing `/run/user/UID/frametop-hands/hands` and `gestures` 30 times a second, in
  their layouts (`hands/include/fh_hands.h`, `fh_gestures.h`). The gesture counters carry on from
  where ft-hands left them, and a gesture ft-hands left down ends as lost, so nothing reads as a
  click at the handover.

A hand is built from its shape, wrist position and rotation in the head frame (+X right, +Y up,
-Z forward). With no rotation the fingers point forward and the palm faces down. Shapes:

| Shape | Landmarks | Gestures |
| --- | --- | --- |
| `open` | fingers out, thumb apart | none |
| `point` | index out, the rest curled | none |
| `pinch` | thumb tip on the index tip | pinch down |
| `fist` | all curled | grip down |

Changing a hand's shape is how to click: `pinch` then `open` is one pinch, begun and ended.
The skeleton (joint frames, bone lengths, elbow) is filled in too, so readers of version 2
work, but it's a fixed pose per shape, not a fit.

## Ending

The fakes stop, take their files back out and let the real trackers take over within a few
seconds when:

- the last fake is released (`gaze release`, `hand all release`, or `release`),
- the unit gets SIGTERM (`systemctl --user stop frame-testbench-fakes`),
- no command came for an hour,
- or it crashes: the stand-ins stop being fresh, and ft-gaze and ft-hands go back by themselves.

On the way out it publishes no hands and ends any gesture down as lost.

## Using it

- ft-gaze runs only while the gaze is in use: gaze mode on with the headset worn (`worn on`
  fakes that), a gaze check or calibration open, or the Gaze settings page open.
- The faked directions are what the tracker saw. ft-gazed applies the calibration and lessons
  on top, as with real eyes, so the pointer lands where this user's calibration puts that look.
- Lost eyes have SteamVR's high variance (0.02, over ft-gazed's `EYE_LOST`), closed ones an
  openness of 0, which ft-gazed reads as a blink.

## Recording and replaying

Real tracker output can be recorded and played back through the same fakes, for a look or a
pinch that a script can't pose well:

```sh
./frame-testbench record look-around --seconds 20             # gaze and hands, while worn
./frame-testbench record pinches --seconds 30 --only hands
./frame-testbench replay pinches                              # once, then the fake ends
./frame-testbench replay look-around --loop --only gaze       # until released
```

Recording only reads: SteamVR's `/dev/shm/eye-server.mmap`, our tracker's
`/dev/shm/frametop-eyes-gaze`, and ft-hands' `hands` and `gestures` files. It needs someone
wearing the headset with the gaze in use and ft-hands publishing, and refuses while the
testbench itself is faking them. It goes in `artifacts/recordings/NAME/`, a new folder only
this user can read, with:

- `gaze.jsonl`: a header line, then a line per new sample with its time and the fields ft-gaze
  reads (`mmap`: directions, origins, variances, openness; `own`: yaw, pitch, flags, eyes, slip).
- `hands.fhdump`: each new set of both files, in `ft-handreplay --publish-dump`'s format, so a
  dump made offline from a camera recording plays back the same way.

No camera image is recorded. Gaze is still how someone's eyes moved: recordings stay on this
machine in the ignored `artifacts/` folder, and never go into the repo, a PR, or anywhere else.

Replay keeps the recorded timing. Hands get their times shifted to now and their gesture
counters carried on from where the last publisher left them, so each recorded pinch is one
click. A scripted `gaze look` or `hand ... set` stops a replay of that tracker, and `release`
stops all of it. At the end of a single run the fake ends as if released.

## Limits

- SteamVR's own gaze action (`/actions/.../eyetracking`, the action source in ft-gaze) isn't
  faked. Neither is anything that asks ft-eyes directly, such as our tracker's calibration
  captures.
- Hands reach Frametop only: OpenXR and OpenVR skeletal input in games isn't faked.
- ft-hands has to be running for the park to matter; without it the fake publishes all the same.
- What these can't show (how a real look or pinch feels, tracking quality) needs a headset test.
