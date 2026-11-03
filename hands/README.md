# Hands (experimental)

Hand tracking from the headset's own cameras. It's experimental and costs a lot of the headset's CPU. `install.sh` offers it (yes by default; `--no-hands` skips it), a release brings it built, and installed, it runs while the Frametop desktop does (`HANDS_MODE=frametop`). `hands/run.sh install` (below) installs it later.

It serves two things in Frametop:

- **Hand cutouts:** where your hand is between an eye and a screen, that eye sees the room through the screen (ft-screens, `screens/handcut.cpp`), so your hands show over the screens the way they do on a Vision Pro. The hole is round, one per hand, until the tracking is good enough for a hand-shaped one (also there, built from the skeleton; see "Hand cutouts and near input" below).
- **Rendered hands:** ft-handview draws your hands as a 3D hand model posed by the tracking, Quest-style, over everything (`screens/handview.cpp`; see "Rendered hands" below): with Room View off (a VR game, SteamVR Home, the dashboard, or the panels over the void), where ft-screens doesn't cut, and by default with Room View on too, floating over the hole.
- **Near input on Frametop's panels:** poke a screen with your index finger to click, pinch its bar, edges or any corner to move or resize it, hold it like a card or in a fist to carry and turn it, hold it with both hands to stretch it, flick it away or throw it (ft-screens, `screens/handinput.cpp`; see "Hand cutouts and near input"). SteamVR's own panels (the dashboard, the Steam menu, other apps' overlays) take pokes and pinch clicks through the pointer helper's laser, and the hand ray's pinch on a floating one's grab bar moves it.
- **Pinches and grips:** with `POINTER_HANDS=1`, the pointer helper takes them as clicks and drags. Look at something and pinch to click it, with the eye tracker doing the looking (`gaze/`), or close your hand to press and drag what the pointer is on. See "Pinches and grips in the pointer" below.

Two programs, each a user service that stops when SteamVR does:

- `ft-camd` (`camd/`, C) borrows XRService's camera buffers and publishes the four IR tracking cameras' frames to a shared-memory ring. It runs on the host.
- `ft-hands` (`track/`, C++) finds hands in those frames with MediaPipe's palm and landmark models on ncnn, triangulates them, and publishes them. It runs in the dev container.

`hands/run.sh install` builds them (a release brings them built), gives ft-camd its capabilities, installs both services, and links `hands/ft-handsctl` into `~/.local/bin`. `install.sh` runs it when you choose hand tracking, then sets `HANDS_MODE=frametop` unless it's `always`. When they run is `HANDS_MODE` in `~/.config/frametop.conf`, set on the Hands page of Frametop Input Settings or with `ft-handsctl mode`; `hands/handsmode.py` does the starting and stopping, for these two and the hand view (`frametop-handview.service`, when it's installed):

- `off` (the default before hand tracking is installed): nothing starts them. `ft-handsctl on` starts hand tracking and `ft-handsctl off` stops it, as before; it stops with SteamVR.
- `frametop`: they run while the Frametop desktop does. The session (`session/frametop-session.sh`, screens backend) starts them once ft-screens is up and stops them as it ends.
- `always`: they're wanted by `steamvr.service` (`systemctl --user add-wants`), so they run whenever SteamVR does, in games and the SteamVR dashboard too, with or without the desktop. Pausing Frametop for a VR game leaves them running and unparked.

Choosing a mode applies it at once: the services start or stop, with no desktop restart. Without ft-camd's capabilities nothing starts, and the Hands page says to run `hands/run.sh caps` (it asks for the password in the terminal; the page never does). `hands/run.sh install` and `caps` apply the mode too, but stop nothing, so what `ft-handsctl on` or `ft-cutouts on` started keeps running.

For the cutouts alone, `hands/ft-cutouts on` starts the same two programs with ft-hands' `--no-gestures`: your hands show through the screens, and no pinch or grip is detected, so nothing clicks or drags. It runs this checkout's build as transient user units, so it needs `hands/build.sh` and ft-camd's capabilities (`hands/run.sh caps`) but not `hands/run.sh install`. It and `ft-handsctl on` stop each other's services, and it stops with SteamVR too.

```
ft-handsctl on | off            # on the Frame: start or stop hand tracking (SteamVR must be running)
ft-handsctl mode [frametop|always|off]  # HANDS_MODE: show it, or set and apply it
ft-handsctl status              # the services, and ft-hands' last status lines
ft-handsctl log [lines]
ft-handsctl cutouts on|off|state  # ft-screens' hand cutouts, without stopping tracking
ft-handsctl gestures            # pinches, grips and middle pinches, live (tools/watch_gestures.py --distance)
ft-handsctl flag [note...]      # flag a tracking failure: the last seconds go to a folder (see "Flags")

hands/ft-cutouts on | off | status   # the cutouts only: no pinches or grips

hands/run.sh install            # build, give ft-camd its capabilities (sudo, once per build), install, apply HANDS_MODE
hands/run.sh start|stop         # start or stop the services
hands/run.sh restart            # after changing a setting
hands/run.sh status
hands/run.sh log [lines]
hands/run.sh caps               # after rebuilding ft-camd (a rebuild clears its capabilities)
hands/run.sh uncaps             # take them back, unless the Hand Recorder or the services use them
hands/run.sh uninstall          # the services, then uncaps
```

Settings in `~/.config/frametop.conf` (`FT_<name>` in the environment overrides them), read when ft-camd and ft-hands start:

- `HANDS_SWAP_SIDES=auto` (the default): ft-hands tells from the hands which side camera is which, and corrects ft-camd's names when they're backwards (see "Which camera is which" below). `1` forces them exchanged and `0` forces ft-camd's names; ft-hands still checks, and if the hands disagree it logs a warning and publishes the hands' answer as the truth (`sides.json`), so recordings are labelled right. The example config said `0` until 2026-10-05; `scripts/conf-migrate.sh` (run by `install.sh` and `hands/rec/install.sh`) turns that untouched line into `auto`.
- `HANDS_CPUS=5,6,7`: the CPUs the model threads run on (below).
- `HANDS_CAMERAS` (`auto`), `HANDS_BRIGHT` (`all`), `HANDS_BRIGHT_ON` (40), `HANDS_BRIGHT_OFF` (25): which cameras ft-hands tracks with, as `--cams`, `--bright`, `--bright-on` and `--bright-off` (see ft-hands). `HANDS_CAMERAS=mono` also keeps ft-camd off the colour cameras.
- `HANDS_MODEL=latest`: which hand models to run, picked on Input Settings' Hands page with notes and benchmarks: `latest` (`hands/models/student`, kept current by `models.sh`), `stock` (`hands/models/ncnn`, Google's MediaPipe models), or a version from `hands/models.json`, downloaded into `hands/models/versions/NAME` (see `models/README.md`). A version that isn't downloaded falls back to latest.
- `HANDS_MODELS` (unset: what `HANDS_MODEL` picks; by default `hands/models/student`, the fine-tuned models, when `hands/models.sh` fetched them, else `hands/models/ncnn`, the stock MediaPipe models): a folder holding `palm.ncnn.*` and `hand.ncnn.*`, as `--models`. Use it to try other models without passing options to every launcher. The fine-tuned folders lack the `*-int8` files, so `--int8` without `--models` uses the stock ones.
- `HANDS_MODELS_UPDATE=1`: when hand tracking starts, fetch the fine-tuned models GitHub's branch pins if they changed, without a new release (`models.sh` below). `0` keeps the ones it came with.
- `HANDS_COLOR_LEFT` (`color_video0`), `HANDS_COLOR_CROP` (`subtract`): how the colour module's calibration maps onto its images, as `--color-left` and `--color-crop`.
- `HANDS_SKELETON=1`: fit a skeleton with your bone lengths and publish it (see "Skeleton" below); `0` publishes the filtered landmarks as before. As `--skeleton`.
- `HANDS_PINCH_DIP=0`: `1` also starts a pinch on a quick dip of the thumb-index distance that never reaches 2 cm (see the pinch under "Gestures" below). As `--pinch-dip`.
- `HANDS_FLAG_BUFFER=1`: keep the last seconds of hand crops in RAM for `ft-handsctl flag` (see "Flags"); `0` keeps nothing. As `--flag-buffer`. `HANDS_FLAG_SECONDS` (12), `HANDS_FLAG_CROP` (224 px), `HANDS_FLAG_MB` (80) size it. `HANDS_FLAG_DIR` (`~/.local/share/frametop/hands/flags`) is where flags go; `HANDS_FLAG_FLASH=1` pulses the drawn hands when a flag is taken, `HANDS_FLAG_TONE=1` adds a short tone, `HANDS_FLAG_VIEW=1` saves a frame of your view.

The pointer helper's `POINTER_HANDS` and `POINTER_PINCH_*`/`POINTER_GRIP_*` settings are in "Pinches and grips in the pointer" below.

Files, all in `/run/user/UID/frametop-hands/` (private to the user; not `/run/user/UID/frametop/`, which the desktop session deletes whenever it starts):

| File | Written by | Layout | Read by |
| --- | --- | --- | --- |
| `cam-ring` | ft-camd | `camd/fhring.h` (spec: `camd/fhring.md`) | ft-hands, `tools/ring.py` |
| `calib` | ft-hands, at start and when it exchanges the side cameras | `include/fh_calib.h` (spec: `include/fh_calib.md`): each ring camera's intrinsics, distortion and place on the head | other readers of the ring |
| `hands` | ft-hands, or another producer (`held`) | `include/fh_hands.h` (version 3: version 1, a skeleton per hand, then the producer and the head pose; spec: `include/fh_hands.md`) | ft-screens (`screens/handcut.cpp`, version 1's part), the hand recorder (`rec/session.py`) |
| `gestures` | ft-hands, or another producer (`held`) | `include/fh_gestures.h` (spec: `include/fh_gestures.md`) | the pointer helper (`pointer/helper/ft-pointer.cpp`), `tools/watch_gestures.py` |
| `held` | another hand tracker, touched every second while it publishes the hands | its name | ft-hands, which parks while it is under 3 s old, except while it records (`include/fh_hands.md`, "Producers") |
| `flag` | `ft-handsctl flag` (`handsflag.py`) | key=value lines: `dir`, `note`, `flag_unix`, `said_unix` | ft-hands, which removes it when it takes it |

The source keeps the `fh_` names and magic strings of frame-hands, the project it started as, so recordings made with it still work.

## ft-camd

XRService owns the headset cameras. ft-camd borrows its DMA-BUFs read-only with `pidfd_getfd`, the same way FrameEyeCameraFeed does. It never touches XRService's V4L2 descriptors. `discovery` in `camd/xrcams.c` is adapted from FrameEyeCameraFeed (MIT, see `camd/LICENSE.FrameEyeCameraFeed`).

Polling buffers for changes can catch a frame while the camera is still writing it. Instead, ft-camd listens to the `v4l2:v4l2_dqbuf` tracepoint, which fires when XRService takes a buffer. It gives the buffer index, the sequence number and the capture timestamp. ft-camd learns which DMA-BUF holds each V4L2 index by watching which buffer changes at each dequeue:

- Right after XRService allocates its buffers, the mapping is allocation order.
- After XRService restarts streaming, the order is shuffled, and the mapping is learned index by index.
- The two upper cameras share one run of buffers. For them, only allocation order can tell the cameras apart.
- It also re-maps an index on the fly when its buffer holds no new frame.

**Privileges.** Setting up needs three things. `pidfd_getfd` on XRService needs `CAP_SYS_PTRACE`, because the Frame has `ptrace_scope=1`. The system-wide tracepoint needs `CAP_PERFMON`, because `perf_event_paranoid` is 2. Its format files are root-only, which needs `CAP_DAC_READ_SEARCH`. `hands/run.sh install` gives the binary those capabilities with `sudo setcap`. File capabilities need a filesystem mounted without `nosuid`. The Frame's `/home` (ext4) has no `nosuid`. ft-camd drops them all once it has set up, before it reads a frame, and then runs as you. XRService runs as you too. It also runs under `sudo`, for trying it by hand, and then drops to the user who ran sudo. It reads nothing from the ring's readers.

The ring is mode 0600, in a folder only you can write. Frame handling:

- Only complete, bright frames are published. The cameras alternate a normal exposure with a near-black one, so each camera gets 30 of its 60 fps.
- A copy torn by the camera overwriting the buffer is dropped.
- Each copy takes about 0.1 ms, and a cache sync about 0.15 ms.

Options:

- `--dark R`: a frame dimmer than R times the camera's recent brightest counts as near-black. Default 0.4.
- `--with-dark`: also publish the near-black frames, as extra ring cameras flagged `FH_CAM_DARK`. They show only light sources, so they're no use for hands.
- `--with-color`: also publish the two Arcturus colour cameras, flagged `FH_CAM_COLOR`. The service leaves it off and runs the mono cameras only (see "Known issues"). To try the colour cameras, add it to `ExecStart` in `hands/frametop-camd.service` and run `hands/run.sh install` again; ft-hands then picks the cameras by the light. Each is the luma of the 10-bit frame's valid 1972x2464 (the top 8 bits), at half size (`--color-scale 2`: 986x1232). They run at `--color-idle` (2 fps), enough for ft-hands to tell how bright it is, until a reader asks for more in `/run/user/UID/frametop-hands/color-fps` (ft-hands writes 30 while it tracks or records with them), up to `--color-fps` (30; the cameras run at 60). `HANDS_CAMERAS=mono` leaves them out. Frames that carry the module's warped half-size copy are dropped. Their `capture_ns` is on the colour module's clock (2.2 s off the mono cameras' on 2026-09-29), so line them up with the mono cameras by `dqbuf_ns`. Each frame costs about 0.65 ms of cache sync and 1.1 ms of decoding, so both cameras at 30 fps take about 11% of a core.
- Each mono camera's latest near-black frame's mean goes in the ring (`dark_mean`): a short fixed exposure, so it follows the room's IR light, sunlight above all.
- The ring holds 8 cameras: 4 mono, plus 4 dark twins or 2 colour cameras.
- Colour isn't reliable yet. In the lit-room test of 2026-09-30, the colour cameras kept losing their buffer mapping while the headset was worn: 30 frames in a row looked unchanged, the camera relearned, and after 5 relearns ft-camd exited. Each relearn probed all 32 colour buffers, a whole-buffer cache sync each, which also made the mono cameras miss frames. Runs with the headset idle had none of this. So the passthrough compositor may be writing into the colour buffers while Room View shows. Since then a colour camera never takes the mono ones down: it probes at most 4 buffers a frame, and one that goes stale twice in a row is paused (10 s, doubling up to 160 s) and learned again, without ft-camd exiting. Whether a frame is new is judged on the luma rows only: the chroma after them hardly changes in a lit room.
- `FT_CAMD_DEBUG=1` in the environment: at each stale colour frame, ft-camd logs to stderr the camera, the frame's time, V4L2 index and sequence number, and, for every candidate buffer, how many of its sampled words changed, in all and in the last eighth of the samples.
- `--sensor S`: only the mono cameras whose sensor name contains S.
- `--status S`: a status line every S seconds (0: never).

It exits when XRService exits, when a camera's buffers keep going stale, which means XRService has reallocated them, or when a camera stops while the others keep running (3 s), which means XRService has moved it to another device (the colour module was unplugged or plugged in). The service starts it again, and it attaches to the new buffers: after 1 s the first time, then 1.7, 2.9 and 5 s while SteamVR runs (`RestartSteps`; it can't start itself again, having dropped its capabilities). ft-hands waits for the new ring (up to 15 s) and starts itself again on its cameras, without waiting out its own service's 5 s.

**Which camera is which:** video9 is `slam_right` (sensor `og01a1b 4-0036`), video13 is `slam_left` (`4-0060`), video6 is `upper_left` and video7 is `upper_right`; without the colour module the side pair is on video0 (`slam_right`) and video3. XRService says so in its log: `Found camera 'slam_left': ... v4l_subdev=/dev/v4l-subdev30` names each sensor, and each `TrackingCameraInit` line gives the device and subdev it opened. ft-hands and `camcheck.py` name the devices that way. Until 2026-10-05 they named them by the `TrackingCameraInit` index instead, which is only the order XRService opens them in (index 0 was `slam_right` on every start logged), and ft-camd bound each buffer queue to a device by the order of XRService's file descriptors, which changes when XRService restarts its cameras. The two mistakes made the side names come out swapped on most starts and right on some. ft-camd now asks each device which buffers it holds (`VIDIOC_QUERYBUF` names the descriptor XRService queued at each index) and only falls back to the order if that fails. With the names swapped, every hand is seen by one camera only, at the wrong depth, and the hand holes land beside the hands. ft-hands still checks by itself, in case (`track/sides.h`, `HANDS_SWAP_SIDES=auto`):
- Whenever a hand's landmarks are found in two cameras at once (one of them a side camera), it intersects the rays through the 21 landmarks twice: once with the calibrations as named, once with the two side cameras exchanged. The same hand seen the right way meets within a few mm, in front of both cameras and as far away as its size says. The wrong way misses by centimetres or meets behind a camera.
- With the names wrong, the tracker never gets such pairs on its own: it hands the hand over to where the wrong calibration puts it and finds nothing there. So 5 times a second while undecided, the check places a tracked hand in 3D under the other naming and runs the landmark model where that puts it in the other side camera.
- It decides after 10 votes one way and none the other, or 20 with at most a fifth the other way, over at least 1 s. That takes about 1-2 s of hands in view. If the names are backwards, it exchanges them; the tracked views move with their images. Then it checks once more, more strictly.
- The log says what it found (`side cameras: SWAPPED, now exchanged after 3.2 s (votes ...)`). So does `/run/user/UID/frametop-hands/sides.json`, which the hand recorder reads. Recordings get a `DIR/sides.json` (hands/rec/sides.py has the rules).
- `--record-only` can't tell (it tracks nothing): it records ft-camd's names unless `--sides 0|1` says otherwise.

`tools/check_sides.py --ring` is the independent check from the scene (ORB matches meeting under each naming): exit 0 as named, 3 swapped, 2 can't tell. `--pair upper` checks the upper pair the same way: in every recording so far (3 XRService starts, both side namings) the upper pair was named right. The colour cameras are video3 (`arcimx616 0-0010`) and video0 (`0-001a`); which of them is `passthrough_left` in the module's calibration is for `tools/check_color.py` to settle, on a recording with texture in view.

## ft-hands

```
hands/build/ft-hands                  # status every 5 s; Ctrl+C to stop
hands/build/ft-hands --int8           # the 8-bit models (models/ncnn/*-int8.ncnn.*)
```

Run it in the dev container (`distrobox enter dev -- ...`). It reads the factory calibration from `/persist` (`/run/host/persist` in the container).

Options:

- `--threads N`: model threads, pinned to the `--cpus` list. Default 3.
- `--cpus LIST`: CPUs for the model threads and the main loop. Default `5,6,7` (`HANDS_CPUS`). SteamOS starts user processes on CPUs 0-4, and XRService's head tracking runs on 2-3. With the headset on, a step took 8.4 ms on 5-7 against 13.2 ms on 2-4, and SteamVR's frame timing didn't change (2026-09-29, three rounds of the same replayed frames).
- `--contrast MODE` or `PALM/HAND`: how crops are equalized before the models see them: `clahe[:CLIP]`, `none`, or `stretch` (1st-99th percentile). Default `clahe:2/none`. In the dim recording, CLAHE let the palm search find about 10% more hands, but it made the landmarks jitter more (published median 6.9 mm, against 6.0 mm with plain landmark crops).
- `--sides auto|0|1`: which side camera is which (`HANDS_SWAP_SIDES`, see "Which camera is which"); `--swap-sides` is `--sides 1`.
- `--seconds N`: stop after N seconds.
- `--status S`: how often to print status, in seconds.
- `--models DIR`: where the models are.
- `--nice N`: niceness. Default 5, so the VR stack wins contested CPUs.
- `--no-publish`: don't write the hands and gestures files.
- Parked: while `/run/user/UID/frametop-hands/park` exists, ft-hands processes no frames (the models stay loaded), publishes no hands, and ends any gesture down, as lost; it writes `0` to `hands-want` there every second, and ft-camd stops copying the mono cameras' frames while that file is fresh (3 s) and says 0. The input relay parks it while Frametop is paused, and unparks it while the dashboard is open, so the hands work there; it tracks again from a palm search. With hand interaction on (`POINTER_HANDS=1`) the park is instead of stopping the services. Without it the pause stops them, and the park holds ft-cutouts' tracker, which the pause doesn't stop. The relay makes the folder if hand tracking hasn't run since login. `HANDS_MODE=always` is never parked. Not while recording. The testbench parks it the same way while it publishes fake hands, through `$XDG_RUNTIME_DIR/frametop-testbench/hands`, which counts while it's under 3 s old ([testbench/docs/fake-trackers.md](../testbench/docs/fake-trackers.md)). Another hand tracker takes the hands and gestures files over the same way, through `held` in the same folder, and ft-hands leaves `hands-want` alone then, so ft-camd keeps copying frames for it ([include/fh_hands.md](include/fh_hands.md#producers)).
- Frame sets: a set is the side pair's newer frames, taken within 3 ms, with the upper pair's from the same moment (it waits up to 8 ms after the sides for theirs). XRService keeps the uppers in step with the sides, but after a colour module hot plug they can stay out of step: on 2026-10-17 XRService logged `Set upper camera driver not in-sync` and never `in-sync` again, the uppers ran at 40 fps with no dark frames, and XRService's own tracking replaced them with dummy frames. Waiting for all four to meet left 6-7 sets a second (27-30 normally). Now, when the uppers met fewer than half of the recent sets, ft-hands says so, drops their views and tracks with the side pair alone, at the full rate; when they meet 95% again it takes them back. Taking the headset off and putting it back on restarts the camera streams, which normally brings the uppers in step within half a second (untested after a hot plug). A recording keeps only full sets.
- `--no-gestures`: hands for the cutouts only. No pinch or grip detection, so nothing reaches the pointer and a closing hand doesn't raise the rate to 30 Hz. The gestures file is removed at start. `ft-cutouts` runs it this way.
- `--record DIR`, `--record-for S`, `--record-hz N`: save every frame set for S seconds (default 120) to `DIR/sets.bin`, or at most N sets a second with `--record-hz` (the hand recorder uses 10). Every set is about 80 MB/s. Sending the tracker SIGUSR1 (`pkill -USR1 -x ft-hands`) starts a recording in `~/.local/share/frametop/hands/rec-<time>` without a restart. Recordings are images of your hands and room: they stay on the headset unless you move them.
- `--record-only`: record without tracking or publishing, so it can run beside the live tracker. Give it `--record DIR`, since SIGUSR1 would reach both trackers. With `ft-camd --with-dark`, recordings also hold each camera's newest dark frame as `<name>_dk`, which doubles the rate. With `--with-color`, each colour camera's newest frame is saved with every set, as `color_video<N>`, which adds about 70 MB/s. Run the recorder at normal I/O priority: idle I/O priority stalled a 165 MB/s recording.
- `--reacquire S` (`HANDS_REACQUIRE`, default 1.5): how long after losing a hand the palm search aims at where it should be (see "Scheduling"). 0 turns it off.
- `--pair-sides 0|1` (`HANDS_PAIR_SIDES`, default 1): two hands seen together are one left and one right (see "Scheduling").
- `--hold S` (default 0.6): how long a lost hand tracked for 30+ frames is held before it's forgotten (0.3 s with `--reacquire 0`, as before).
- `--keep-presence P`: the landmark presence a tracked view needs to stay tracked. New views always need 0.5. Default 0.5. Lowering it to 0.2 barely helped in the bright recording, because lost hands drop to near-zero presence.
- `--ring PATH`: read frames from another ring, such as `ft-ringplay`'s.
- `--cams auto|mono|color|all` (`HANDS_CAMERAS`, default `auto`): which cameras to track with. The mono IR cameras light the hands themselves and track well in dim rooms, but in bright light they expose for the room and the hands come out dark. The colour pair is the other way round. `auto` goes by the colour frames' mean brightness: at `--bright-on` (`HANDS_BRIGHT_ON`, 40) or over for 2 s it tracks with `--bright` (`HANDS_BRIGHT`: `all`, every camera, the default, or `color`), and under `--bright-off` (`HANDS_BRIGHT_OFF`, 25) for 2 s with the mono cameras again. A dim evening room read 9. The switch is logged (`cameras: mono -> all (...)`), and the status line gives the colour level, the mono cameras' ambient IR, and how many steps had colour frames. Colour frames arrive on their own schedule, so a step holds the mono set, the colour pair, or both, and views wait in their camera for its next frame. With no colour cameras in the ring (ft-camd without `--with-color`, as the service runs it), ft-hands tracks with the mono cameras whatever this says.
- `--color-left NODE` (`HANDS_COLOR_LEFT`, `color_video0`) and `--color-crop subtract|none` (`HANDS_COLOR_CROP`, `subtract`): how the colour module's calibration maps onto the images. Not settled yet: `tools/check_color.py` on a recording with a lit, textured view tells.
- `--skeleton 0|1` (`HANDS_SKELETON`, 1): the skeleton fit (below). With 0 no skeleton is published, and version 1's points and capsules are the filtered landmarks, as before 2026-10-08.
- `--pinch-dip 0|1` (`HANDS_PINCH_DIP`, 0): the dip rule for pinches (below).
- `--grip-begin R`, `--grip-end R`: the grip detector (below). Defaults 1.2 and 1.45.
- `--gesture-log`: print what the pinch and grip detectors measure, 10 times a second: each hand's thumb-to-index distance (world and triangulated), its palm-down reading and its finger curl.

**Gestures** (`/run/user/UID/frametop-hands/gestures`, `include/fh_gestures.h`), for the pointer helper:

- A pinch: the thumb and index tips within 2 cm, ending past 3.5 cm (see "Pinch" below).
  - With `HANDS_PINCH_DIP=1`, also on a sharp dip that doesn't reach 2 cm. Quick taps with the hand raised often read 25-30 mm at contact and spring back, so they never click. The dip rule begins a pinch when, within 150 ms, the distance falls by 40% of its recent highest to under 3.2 cm, with the palm moving under 0.3 m/s and the thumb at least 4 cm from the middle fingertip (a hand closing into a grab brings both in). It ends once the distance is 6 mm over its lowest, and becomes an ordinary pinch (drags and all) if the tips get within 2 cm. It is off by default. In ft-handreplay on frame-hands' guided tap capture (2026-10-09, student v4), it caught 25 of 30 quick taps on a 2 Hz click instead of 10, and 74% of the teacher's clear taps instead of 62% at the same precision (0.91 against 0.93). The cost is that fingers stopped about 1 cm apart click too: 16 pinches in the near-miss segment instead of 5. Pointing and grabs in the hand recorder's takes gave 22 pinches, against 21 without it (`PinchParams::dip`; frame-hands notes/taps.md, Part D).
- A grip, a closed hand: every finger's tip nearer the wrist than 1.2 times its knuckle is (from the model's 3D hand, so hand size doesn't matter), ending when they open past 1.45 on average. It begins only on a hand seen open within the last second (closing it is the gesture), with the palm at most 35 degrees below straight ahead and at least 15 cm in front of the eyes, and not with the thumb within 3 cm of the index tip (that's a pinch with the other fingers curled). A grip ends a pinch on the same hand, as lost. In the 2026-09-30 lit recording (no deliberate fists), the checks cut false grips from 14 to 6, all with the hands on the desk while looking down at it. The pointer helper ignores grips that begin more than `POINTER_GRIP_BELOW` (0.35 m) below the eyes, which it can tell and ft-hands can't.
- A middle pinch (gestures version 3): the thumb on the middle fingertip, with the pinch's thresholds. It begins only with the thumb at least 1 cm nearer the middle tip than the index tip, the index extended (its tip at least 1.5 times as far from the wrist as its knuckle), and the hand tracked for 5 frames; a pinch and a middle pinch on one hand exclude each other, and a pinch begins only with the thumb nearer the index tip than the middle one. The pointer helper's hand ray turns it into a right click or scrolling. Closing the hand into a grab brought the middle tip to the thumb with the index curled: before the index and frames rules, the recorder's gestures take (PR #8, about 95 s of pinches and grabs, none of them middle pinches) gave 6 false middle pinches; after, 1. The 2026-09-30 lit recording (typing and 26 deliberate pinches) gave none, and the same 48 pinches as without the middle pinch.
- `tools/watch_gestures.py --distance` shows them live; `ft-handreplay --timeline` logs them (middle pinches as `pinch2`) and each hand's finger curl.

The status line also says how often a hand was on each side (by where the wrist is), and why views and hands came and went: views lost (the landmark model stopped seeing the hand), handoff misses (a crop projected from the hand's 3D position found nothing), duplicates, splits (two views disagreed in 3D), and hands created, merged and forgotten.

### Scheduling

- Each hand is tracked in its best two cameras, the way MediaPipe tracks: the landmark model runs on a crop placed from the previous landmarks, with no palm detection.
- A hand seen in too few cameras is projected into the others through the calibration. Where it lands well inside a camera, that camera gets a crop to try. This is how a hand raised out of the side cameras reaches the upper ones.
- The palm detector runs only while fewer than two hands are tracked, at most 5 times a second, on a few zoomed tiles per search. Tiles are picked in proportion to how likely hands are there. Each tile is turned so the expected shoulder-to-hand direction points up. There are 42 tiles and a search tries 2 (3 with no hand tracked), so even the likeliest tile comes round only every 10-11 searches: about 2 s at 5 searches a second, and 4-5 s live, where a search round takes about 50 ms and the tracker manages 2-3 a second.
- **Re-acquiring a lost hand** (`--reacquire`, on by default since 2026-10-08). A hand that stops being seen is first held, not published: 0.6 s if it was tracked for 30 frames or more (`--hold`), else 0.3 s. Meanwhile its crops are retried where it was heading (its last position plus its velocity). From the first step it's missing and for 1.5 s after it's forgotten, the palm search aims up to 2 tiles at where it should be, in the cameras that see that spot best. The aimed tiles grow with the time since the loss, and they run every processed set while such a hand is missing, not only 5 times a second. The rotation keeps at least one tile per search. In replays this brought a lost hand back on the next processed set (median 0.05 s, against 0.12-0.25 s), and the slowest cases from seconds down to under 1 s (see frame-hands' `notes/reacquire.md`).
- **Left and right** (`--pair-sides`, on by default since 2026-10-08). Each hand used to be called left or right by a running average of the landmark model's call, and both hands could get the same side for seconds. Now each hand also accumulates the model's calls as fading log-odds, and two hands seen together are one left and one right: the hand with more right-hand evidence is the right one, and which wrist is further right only tips a close call.
- Frame sets are processed at 30 Hz while a hand moves faster than 0.25 m/s (or a pinch is down or closing), at 15 Hz otherwise, and at 5 Hz while no hand is in view. A reader that needs the full rate writes it to `/run/user/UID/frametop-hands/want-hz` and keeps the file fresh: ft-screens writes 30 there while a hand is near one of its panels, because at 15 Hz a poke's press would come up to 67 ms later. A file older than 1 s lapses.

### 3D

- **Two or more views:** each landmark is triangulated from the camera rays, weighted by the model's presence score. The median ray distance is reported as the residual.
- **Pairing views across cameras.** The side cameras sit side by side, so two hands next to each other at the same height fall on the same epipolar lines, and rays to two different hands can nearly meet close to the cameras. That made phantom hands 12-15 cm in front of the eyes, which tore holes through the screens. Each step now scores every way of pairing the views in two cameras and keeps the best. A pair scores well when its rays meet, when each view's apparent size matches the triangulated distance, and when the model calls both the same hand. The size check uses a fixed prior: with the model's average hand, clean pairs measure 0.71-1.51 times the one-view distance, and mismatched pairs mostly far less.
- **One view:** depth comes from the model's metric world landmarks, their spread across the palm against the angle it covers in the image, scaled by the user's hand size (learned while two views are available). That distance is off by 10-30% and wanders about 10% between frames, so a hand that drops to one camera keeps its last distance and drifts toward the one-view guess by 10% a frame.
- **Smoothing.** The landmarks go through a One Euro filter: it smooths hard while the hand is still (tracking noise is several mm per frame) and hardly at all while it moves fast. The palm speed that sets the update rate is the filtered one; the raw speed read about 0.25 m/s from noise alone. The gestures (pinch point, palm-down reading) use these filtered landmarks. What's published is the skeleton's (below), unless `--skeleton 0`.
- **Capsules.** Forearms follow the hand's own axis, and nothing within 12 cm in front of the eyes is published.

How good the depth is, measured from recordings (2026-09-30, `--depth` below): the two lower cameras see the hands about 77% of the time, a lower and an upper camera 7-12%, and one camera 12-15%. Depth is the noisy direction. With the lower pair, it jitters 4-6 times as much as sideways position (published: 3-7 mm against 1-2 mm). The one-camera guess is a median 2-6 cm off. When a camera drops out, drifting 10% a frame toward that guess is worse than keeping the last distance (after 0.5 s a median 23-30 mm off, against 11-12 mm).

## Skeleton

Moved to [tracking.md](tracking.md#skeleton).

## Pinch

Moved to [tracking.md](tracking.md#pinch).

## Pinches and grips in the pointer

Moved to [tracking.md](tracking.md#pinches-and-grips-in-the-pointer).

## Flags

Moved to [tracking.md](tracking.md#flags).

## Recordings

`hands/build.sh --tools` also builds the offline tools.

`ft-handreplay DIR` runs a recording through the tracker with the live scheduling and reports how well it kept the hands: hands per set, left and right coverage, track lengths, pinches, jitter, and the same reasons as the status line. `--publish-dump FILE` also writes what ft-hands would have published after each processed set (the hands and gestures files' bytes, and the set's dequeue time), for ft-screens' offline tests; a hand-recorder take's `poses.jsonl` and `prompts.jsonl` copied next to the dump as `NAME.poses.jsonl` and `NAME.prompts.jsonl` give them the head's poses and the touch take's targets (`scripts/test-handinput.sh`, `scripts/bench-matte.sh`). To match the live tracker, replay with `--reacquire 1.5 --reacquire-fast --pair-sides --hold 0.6` and the models in use.

```
hands/build/ft-handreplay ~/.local/share/frametop/hands/rec-20260929-120000 --cost --oracle 10 --timeline /tmp/tl.txt
```

- `--cost`: instead of timing the steps, charge each round of model calls what it typically costs live (10 ms landmarks, 18 ms palms), so results repeat exactly.
- `--oracle N`: every N-th set, also search every tile of every camera, and report how often the tracker had the hands that full search could find.
- `--slow F`: live, the tracker skips sets that arrive while it's busy. Replay counts each step's time times F as busy (default 1; the headset is busier live).
- `--timeline FILE`: a line per processed set and hand, with pinch events and distances.
- `--ray FILE`: a line per processed set and hand with what the pointer helper's hand ray reads (time, side, stereo, pinch and grip state, the published points, the wrist's frame, the elbow), for `pointer/tools/rayeval` with the same session's `ft-handtest` probe log (head poses).
- `--cams mono|color|all`: which cameras to track with (default `mono`). `color` tracks with the Arcturus pair alone, for comparing it with the IR cameras on the same recording. It needs a recording made with `ft-camd --with-color`. `--color-left NODE` (`color_video0` or `color_video3`) and `--color-crop subtract|none` say how the module's calibration maps onto the images; `tools/check_color.py` finds out.
- The skeleton (see "Skeleton"): the report ends with its measures for the measured, One Euro filtered and fitted landmarks (and the fit before its filter): its time per hand, also on its own pinned to a little and a big core; its rms; each side's calibration and when it settled; how much bone lengths spread per track; jitter at rest and moving for the palm and the thumb and index tips; how far the tips trail during fast motion (against a centred average of the measured tips); how steady the palm's normal is; and the elbow. `--no-skeleton` replays without it; `--skel NAME=VALUE` changes a setting (`skel::Params`); `--labels FILE` compares with frame-hands' auto-labels (`labels.jsonl`), `--skeleton-dump FILE` writes the matched hands; `--hmd-poses FILE` (a take's `poses.jsonl`) compares the elbow with gravity's up against the head's; `--pinch-fitted` counts pinches on the fitted tips too.
- A hand-recorder take is a recording too: its `sets.bin` is followed by `sets-2.bin` and later parts, which are read in order. It has no `sides.json`: whether its side cameras need exchanging is `take.json`'s `names_swapped` against `session.json`'s `sides` (`rec/sides.py`), so give `--sides 0` or `1` (`--sides auto` checks from the hands).
- `--depth FILE`: a line per hand per processed set for `tools/depth_report.py`, which measures the depth without ground truth: how the hands were seen, the noise along the line of sight against across it, each camera's one-view distance against the triangulated one, and what a camera dropping out would do.
- The pinch, contrast and presence options are ft-hands'. `--reacquire S` (off unless given; `--reacquire-fast` searches every set as ft-hands does, `--reacquire-tiles N`, `--reacquire-clear M`), `--pair-sides`, `--hold S`, `--near F` and `--search-budget N` are the tracker's re-acquire settings, off or at their old values by default so old replays repeat.
- `--cost-hand MS`, `--cost-palm MS`: with `--cost`, what a round of landmark and palm calls costs. The defaults (10, 18) are the headset's idle costs. The 2026-10-08 live probe (CPUs 0,1,4 beside SteamVR, 3 threads) measured about 14 and 52, and the live tracker loses hands far more at those costs.
- `--drop-every S`: the re-acquire test. Every S s, while two hands are tracked, one of them is made invisible for 0.35 s (just past the 0.3 s the tracker holds a lost hand), and the report gives how long it then took to have two hands again: the time to find a hand that never left the view.
- `--flag-at S1,S2`, `--flag-every S`, `--flag-out DIR`: flags as `ft-handsctl flag` makes them live (see "Flags"), at those times into the recording.
- With `--timeline`, the tracker also writes why views and hands came and went (`ev` lines: lost, miss, split, search and its tiles, palms kept or rejected, new, forget, pair-sides), and `--oracle` writes what the full search found. `tools/gap_report.py TIMELINE --list` turns that into every gap of 0.3 s or more per side, with its cause. `tools/replay_gaps.sh OUT MODELS "ARGS" REC...` runs several replays at once (on the 7i through frame-job) and reports them. For pinches, `--tips FILE` logs each hand's thumb, index and middle tips and the pinch detector's distance measures per processed set, with its pinch events; `--rate HZ` fixes the processing rate (15 or 30, against the live rule) and `--want-hz HZ` plays a reader asking for it; `tools/replay_tips.sh OUT NAME=MODELS "ARGS" REC...` runs them (frame-hands' `train/taps/tapeval.py` scores them against auto-labels).

`ft-ringplay DIR --ring PATH [--from S] [--to S] [--loop]` publishes a recording into a ring file in real time, as ft-camd would, so `ft-hands --ring PATH --no-publish` runs the same frames run after run. It needs no privileges, and it skips the dark frames.

## Tools

Python, with NumPy and OpenCV. `setup/dev-container.sh` doesn't install them, because Fedora's `python3-opencv` pulls in over a gigabyte; in the dev container, run `sudo dnf install python3-numpy python3-opencv` once. Off the Frame, `FRAME_JOB_DEVICE_ROOT` can point at a folder with copies of the headset's calibration files.

- `tools/check_sides.py --ring` (or a recording, a sets file, `--pair upper`, `--calib DIR`, `--json`): are the side cameras named right, from the scene? ft-handreplay's `--sides file|0|1|auto` replays with DIR/sides.json's names (the default), as recorded, exchanged, or as auto decides, and reports what the side check found and when.
- `tools/check_color.py REC`: how the colour module's calibration maps onto its images.
- `tools/show_set.py REC`: a recording's frame sets as images.
- `tools/watch_gestures.py [--distance]`: pinches and grips, live.
- `tools/depth_report.py DEPTH`: the depth measures above.
- `tools/cut_sets.py REC OUT [--sets N | --at I,J,...]`: copies a few frame sets (by default 8, spread evenly) out of a recording into a small one, to look at or check elsewhere without moving gigabytes. Plain Python, so it also runs on the Frame's host.
- `tools/replay_dumps.sh OUT MODELS REC...`: replays recordings as the live tracker runs, each into a publish dump with its head poses and prompts beside it (a hand-recorder take named SESSION-TAKE), several at once, through frame-job on a compute host.
- The desk model's training (`docs/hands-typing.md`): `pointer/tools/deskfeat DUMP POSES OUT.csv [--model M]` turns a publish dump into the desk classifier's features (and a model's probabilities), and `tools/desk_train.py OUT.txt DIR...` labels them from the prompts (the `desk-work` round's steps, hand-size and gesture takes as free), trains it with each session held out in turn, and writes the model `POINTER_DESK_MODEL` reads. It needs numpy (dody's frame-hands venv).
- `tools/convert_models.py`: how `models/ncnn` was made from the OpenCV Zoo ONNX ports of MediaPipe's models (see `models/NOTICE`).
- `models.sh`: fetches the fine-tuned models into `models/student` (`hands/build.sh` runs it, so source installs and the release image both get them): MediaPipe's two models retrained on the Frame's infrared cameras with the [Frametop Hands](https://huggingface.co/datasets/DeeJanuz/frametop-hands) dataset, from a fixed commit of [DeeJanuz/frametop-hands-models](https://huggingface.co/DeeJanuz/frametop-hands-models), each file checked against its SHA-256. They're licensed CC BY-NC 4.0 (non-commercial), not MIT, so they aren't in this repository; `models/student/LICENSE` and `README.md` come with them. `hands/models.pin` pins them: the commit, the version folder and every file's sum (frame-hands' `model-repo/pin.py` writes it); `hands/models.sh check` checks them against it. Installs don't need a release for a new version: when hand tracking starts, `frametop-hands.service` runs `hands/models.sh auto`, which reads `hands/models.pin` from GitHub (main for a stable release, experimental for an experimental one or a clone on that branch; at most every 15 minutes, within a minute) and, if it pins other models, fetches them into `models/.student-new`, checks every sum, and swaps that folder in whole. Offline, or anything not matching, it keeps the models it has. A pin only names the files above, and its `format` says which ft-hands it needs: raise it when a model needs a newer one, and installs without it keep theirs. `HANDS_MODELS_UPDATE=0` turns it off; `hands/models.sh update` checks now.

To try the hand cutouts without restarting the desktop, `screens/build/ft-handtest [--distance m] [--width m] [--seconds s] [--matte circle|mesh|capsules] [--margin deg]` (built by `screens/build.sh`, run in the dev container, with hand tracking on) shows a test panel of its own, a light grid 1 m wide and 0.8 m ahead by default, and cuts your hands out of it the way ft-screens cuts them out of the screens. Every 2 s it prints the composite's cost.

## Hand cutouts and near input

Moved to [cutouts.md](cutouts.md#hand-cutouts-and-near-input).

## Rendered hands

Moved to [rendered-hands.md](rendered-hands.md#rendered-hands).

## Camera check

Moved to [rendered-hands.md](rendered-hands.md#camera-check).

## Build

`hands/build.sh` builds in the dev container on the Frame, into `hands/build/`, with `hands/Makefile`. The first build fetches ncnn's source at a pinned release (`NCNN_TAG` in the Makefile, its full-source zip checked by `NCNN_SHA256`) and builds it into `hands/build/ncnn`, which takes a few minutes; `NCNN=DIR` points at an ncnn install already built instead. ft-camd is linked statically, because it runs on the host, which has an older glibc than the container.

## Known issues

- **The side cameras could come out swapped** (fixed 2026-10-05, see "Which camera is which"). If they still do, ft-hands corrects it from the hands (`HANDS_SWAP_SIDES=auto`, the default): until it has seen about 1-2 s of hands in both namings' reach, the cutouts may sit beside the hands. ft-camd's log says `bound by VIDIOC_QUERYBUF` for each camera when its buffers were matched exactly.
- **The colour cameras can't be used while the headset is worn.** The colour module then writes only a half-size image into the top-left quarter of its buffers, and ft-camd drops those frames. So the service runs the mono cameras only, and tracking in bright light, where the mono cameras see dark hands, doesn't get the colour pair's help.
- **The colour calibration mapping isn't settled.** Which colour camera is `passthrough_left` (`HANDS_COLOR_LEFT`) and how the module's crop applies (`HANDS_COLOR_CROP`) still need `tools/check_color.py` on a recording with a lit, textured view.
- **Depth when one camera loses the hand.** A hand seen in one camera drifts 10% per update toward the one-camera depth guess (`kMonoDepthGain`, 0.1, in `track/tracker.cpp`). In the 2026-09-30 replays that was worse than keeping the last distance (see "3D" above). A smaller gain, such as 0.02, is the next thing to try.
- **Pinches aren't reliable enough for everyday use yet.** That's why hand tracking is off by default (`HANDS_MODE=off`: only `ft-handsctl on` starts it), and `POINTER_HANDS` is 0 by default.
- **SteamVR can leave the upper cameras and the IR light off after a wake**, and then no hands are found. See "Camera check" above: `camcheck.py` tells, the hand recorder won't start a session, and the fix is a SteamVR restart or a reboot.
- **The skeleton (2026-10-08) is tested on recordings only**, not in the headset: the cutouts now follow it (version 1's points and capsules), so check them there; `HANDS_SKELETON=0` brings back the filtered landmarks.
- **Floating windows don't get hand cutouts or glows.** Their panels show crops of the client buffer, which the cutouts' side-by-side buffer doesn't match (`screens/vr.cpp`, `UpdateCutouts`). Near input works on them.
- **The matte and near input are tested offline only** (2026-10-08): `HEADSET-TESTS.md` has the first headset tests.
