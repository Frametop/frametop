# Eye recorder: design (draft, 2026-10-14)

The eye recorder guides a person through recording their eyes with the headset's two eye cameras, at known gaze targets and through the cases where eye tracking breaks. It also fits the headset: it shows whether each eye sits where the cameras see it best, helps set the IPD, and catches the headset shifting on the face, with instructions to put it back. Recordings stay on the headset until the person uploads them. Only the maintainers can read what's uploaded (below, "Privacy").

It's the hand recorder's sibling (`hands/rec/DESIGN.md`): the same session runner pattern, headset button, review, export and Hugging Face upload, with eye cameras instead of hand cameras. Like the hand recorder it ships in Frametop and, later, as a standalone app for people without Frametop (Frametop/frametop-eye-recorder, Frametop as a submodule).

The data is for training a better eye tracker than `gaze/tracker/` (ft-eyes, a classic pupil and glint finder) and SteamVR's, one that holds up at the edges of view, through blinks and squints, with glasses and makeup, and when the headset moves.

## Decisions so far

| | Decided (2026-10-14) |
|---|---|
| Who reads the recordings | Maintainers only. The eye cameras see the iris, which identifies a person. Trained models can be public; the images never are. |
| Packaging | Backend here in `gaze/rec/`, then a standalone repo with Frametop as a submodule, as the hand recorder does. |
| Fit and slip source | Our tracker, ft-eyes: pupil and glint positions in the camera image. It needs ft-eyegrab, the root service `gaze/tracker/install.sh` installs. |

### Open: how "maintainers only" works on Hugging Face

A private Hugging Face repo can't take contributions: people outside the org can't see it, so they can't open a pull request. Org membership would let every member read every contribution. So "private" needs another mechanism. Options:

1. **Encrypted files in a gated dataset (recommended).** The export encrypts every image file to the maintainers' public keys with [age](https://age-encryption.org) (pyrage, which has aarch64 wheels: `pyrage-1.4.0-cp310-abi3-manylinux_2_17_aarch64`). The dataset repo works like `DeeJanuz/frametop-hands`: gated, contributors open pull requests from their own accounts, and `hub.py` and the review tools carry over. Only the frame files are encrypted. Labels, timings, fit readings and session.json stay readable, so `validate.py` and the review can check a pull request before anyone decrypts it. The private keys stay with the maintainers (the Frame and dody), never in a repo. age takes several recipients, so a second maintainer or a new key is one more line in `recipients.txt`. The risk: if a private key leaks, every upload made to it can be read. Withdrawal still means deleting the files and squashing history, as with hands.
2. **An upload service.** A small service (a Hugging Face Space, or a bucket with signed upload links) holds the write token and puts uploads into a truly private repo. Nothing is public, even encrypted. But it's a server to run and pay for, and the 10 GB uploads go through it.
3. **Per-contributor private repos.** Each person uploads to a private repo under their own account, then shares it. Hugging Face can't share a private user repo with one other user, so this doesn't work without an org.

The rest of this design assumes option 1.

## What's measured already

- **The eye cameras** (`gaze/tracker/ft-eyegrab.c`): 512x400 8-bit IR, about 90 fps each when SteamVR runs them at 90 (`gaze/eye-rate.py`; at its default 15 fps a recording is worth little, so the recorder checks the rate first). Camera 0 is the right eye (steep angle), camera 1 the left (head-on). ft-eyegrab copies the frames read-only into `/dev/shm/frametop-eyes-cams` while someone touches `/dev/shm/frametop-eyes-want`.
- **Size.** Raw, both cameras are 36.9 MB/s, 2.2 GB a minute. zstd level 1 on 200 MB of the `fit1` capture (2026-10-14): 56 % of the size at about 250 MB/s on one core, so compressing while recording costs about 15 % of a core. Level 3: 53 %, 2.3x slower. Level 9: 49 %, 9.5x slower. The recorder uses level 1 while recording: about 1.2 GB per recorded minute, so a session of about 10 recorded minutes (below) is about 12 GB, close to a hand session.
- **IPD.** SteamVR reports it (2026-10-14, a probe against the live vrserver): `Prop_UserIpdMeters_Float` 0.0651, `Prop_IpdUIRangeMinMeters` 0.057, `Max` 0.071, `Prop_MinimumIpdStepMeters` 0.0015, and the eye-to-head transforms are ±IPD/2. `driver_cv.so` reads the range from the headset's EEPROM. So the recorder can read the IPD setting live and record it with every take. Not yet known: whether the eye cameras move with the lenses when the IPD changes. That decides how the IPD guidance works (below), and it's the first headset test.
- **ft-eyes** already finds each pupil (centre, ellipse) and glint pair per frame, keeps a per-eye slip estimate from the glints (`eyes_model.SlipTracker`), and resets its shift when frames stop for 3 s (the headset was off). `gaze/fitcheck.py` already keeps per-eye tracked and lost by gaze direction, with hints.

## Parts

| Part | What it does |
|---|---|
| `gaze/rec/session.py` (Python, no Qt) | The session runner, like `hands/rec/session.py`: reads `script.json`, drives the panel, starts and stops the frame writer per step, reads the live fit for feedback, writes each take. `--dry-run`, `--next-after`, stdin steering and `--button-device` as in hands. |
| `gaze/rec/script.json` | Sections, steps, targets and timings (below, "Script"). |
| `gaze/rec/eyerec.py` | The frame writer: reads the shared frames (as `lab/ft-eyes-record` does) and writes zstd-1 chunks, one per second per camera, with an index (below, "Files"). A thread of the session, not a process: ft-eyes-record shows the copy needs no numpy. |
| `gaze/fitcoach.py` (shared) | The fit coach: from ft-eyes' pupil and glint positions, how the headset sits on this face, and what to do about it (below, "Fit coach"). The recorder, the Gaze page's fit check and the put-on check all use it. |
| ft-gazepanel `rec` mode | The headset panel. The gaze panel already draws dots at exact head-relative directions, which the ground truth needs, so it gains a `rec` mode with the hand panel's text, step, countdown and paused lines rather than a second panel (below, "Panel"). |
| `gaze/rec/takes.py`, `validate.py`, `hub.py` | Review, export, checks and upload, from `hands/rec/`'s. The shared parts move into a common module rather than being copied (below, "Shared with the hand recorder"). |
| `gaze/rec/ft_eyerec.py` + `main.qml` | The desktop window: consent, the checklist, fit, session controls, review, export, upload. The standalone app gets a Qt Quick Controls window, as the hand recorder's does. |
| `gaze/rec/CONSENT.md`, `UPLOAD.md` | The texts. Eye images need a stronger consent than hands (biometric data, who can read it, how to withdraw). They need a review like `frame-hands/notes/legal-review-2026-10-03.md` before uploads open. |

## Fit coach

It reads, per eye, from ft-eyes: the pupil centre, its ellipse, the glint pair (both glints and their spacing), and how often the eye was found over the last second. It compares them with a reference: where a well-fitted eye sits in that camera's image. At first the reference comes from the maintainer's headset; once contributions come in, it comes from their sessions with good calibrations (the "fit" section below records exactly that), per headset model. The coach says which reference it used, and until it's built from more than one face its hints are marked provisional.

What it can tell, and the instruction it gives:

| Reading | Means | Instruction |
|---|---|---|
| Both pupils high or low in their images | The headset sits low or high on the face | "Push the headset up" / "Let it down a little", with the top strap |
| Both pupils left or right | The headset sits off to one side | "Slide the headset right" |
| Pupils apart or together, compared with the reference at this IPD | IPD set too narrow or wide | "Turn the IPD wheel wider", until both are centred |
| One pupil higher than the other | The headset is tilted | "Level the headset" |
| Glint pair spacing larger or smaller than the reference | The eye is nearer or further than usual (eye relief: face pad, straps, glasses) | "Tighten the back strap" / "The headset is pressing close: loosen it" |
| A glint missing, or extra bright spots | Glasses reflections, or the lens fogged or smudged | "Tilt your glasses" / "Wipe the lenses" |
| An eye found in under half the frames, looking straight ahead | Lashes on the lens, a droopy lid, or the eye outside the camera's view | The per-eye card says which |

The IPD row depends on the first headset test. If the cameras move with the lenses, a pupil's sideways offset in its image directly says how far that eye is from its lens axis, and the coach turns the IPD wheel's direction into "wider" or "narrower" until both offsets are about zero, at any IPD. If the cameras are fixed, the offset depends on the IPD setting too, and the coach needs the reference at each IPD step: the fit section records an IPD sweep (below) to build that.

**Put-on and shift check.** After a good fit, the coach keeps that fit as the person's baseline (`~/.local/state/frametop/gaze/fit-baseline.json`: per eye, the pupil looking straight ahead and the glint pair, with the IPD setting). Then:
- When the headset goes back on (frames resume after ft-eyes' 3 s gap), it shows the fit card for a few seconds while the person looks ahead. If the pupils sit more than a threshold (start with 1.5 mm equivalent, tuned from data) from the baseline, it says which way to move the headset, live, until it's back. If the IPD setting changed, it says so first.
- While in use, ft-eyes' glint slip estimate already tracks small movement. A shift past the threshold that stays for 2 s gets a quiet notice ("Your headset slipped down: push it up, or look at the dot to recalibrate"), not a modal.
In the recorder this runs at every section start. In Frametop it becomes part of the gaze service, off by default until it's been tried.

## Script

One session is about 20 minutes, about 10 of them recorded, in step mode: the headset's right button starts each step (press = next or pause, two presses = redo, hold = stop), as in the hand recorder. Every step records the panel's targets as ground truth. Each section starts with a fit check, which isn't recorded unless the section is about fit.

| # | Section | What | Edge case it covers |
|---|---|---|---|
| 1 | Fit | The fit coach, then 5 dots. Then the IPD sweep: turn the wheel slowly from narrow to wide and back, looking at a centre dot (the IPD setting is logged at 90 Hz) | Fit reference; IPD per camera position |
| 2 | Calibration grid | 25 dots on a 5x5 grid to 25 degrees, three backgrounds (dark, mid, bright), shuffled | Baseline per session; pupil size against brightness |
| 3 | Edges | Dots at 30 to 45 degrees and the corners, as far as the panel reaches | Eyes turned far: the lid, the glints lost, the pupil foreshortened |
| 4 | Saccades | A dot jumping between random spots, 0.5 to 1.5 s each | Fast movement, overshoot, settling |
| 5 | Pursuit | A dot moving on circles, lines and a figure eight at two speeds | Smooth pursuit; the tracker's lag |
| 6 | Depth | Dots at 0.5 m, 1.5 m and 5 m in turn (the panel's distance changes) | Vergence; the eyes' angle with distance |
| 7 | Blinks and lids | Blink on cue, close both for 2 s, wink each eye, squint, eyes wide, half-closed, looking down with lids lowered | Closed and partly closed eyes; one eye missing |
| 8 | Light | The background steps dark to bright and back while looking at a dot | Pupil dilation and constriction |
| 9 | Face | Smile, raise eyebrows, frown, yawn, talk (read a line aloud: the microphone isn't recorded), chew | The face moving the headset and the lids |
| 10 | Head moving | A dot fixed in the room while turning the head (head poses logged) | The eyes counter-rotating (VOR), headset jiggle |
| 11 | Slip | Nudge the headset up, down, left, right, tilt it; then take it off and put it back on; dots after each | Headset shifting on the face: the put-on check's data |
| 12 | Reading | Lines of text to read, then a page of icons to look over | Natural use |
| 13 | Glasses (if worn) | Section 2's grid again with glasses on (the checklist asks) | Lens reflections, the glasses' frame in view |
| 14 | Grid again | Section 2's grid, the mid background only | Drift and tiredness over the session |

The checklist before a session asks (all optional, none identifying): glasses (none, glasses, prescription inserts, contacts), eye makeup (none, mascara, eyeliner, lash extensions), the face pad and strap in use, a measured IPD if they know it (from an optician), and whether they've had eye surgery that changed how the eye looks. Eye colour isn't asked: the IR images show what matters about it.

## Panel

ft-gazepanel gains `show rec`: a large head-locked panel (FULL_DEG, 64 degrees, with a solid background whose brightness the script sets) that keeps every existing command (`dot`, `bg`, `title`, `text`, `note`, `eye`, `hints`) and adds what the hand panel has: `step`, `big` (countdown), `action`, `keys`, `rec on|off`, `paused on|off`. New for the ground truth:
- `path <kind> <params> <seconds>`: the panel moves a dot itself (circle, line, figure eight), so it's smooth at the headset's frame rate, not at the socket's.
- `distance <metres>`: moves the panel for the depth section.
- `world <x> <y> <z>`: a dot fixed in the room (section 10), as the hand panel's touch target.
- Every picture the panel shows with a dot logs that dot's direction and the frame's time to `targets.jsonl` (CLOCK_MONOTONIC_RAW, the eye frames' clock). The ground truth is what was on screen when, not what was asked.

## Files

```
~/.local/share/frametop/eyes/contrib/
  profile.json                  consent and contributor id, as hands
  sessions/<YYYYMMDD-HHMMSS>/
    session.json                checklist, versions, mode, takes, eye camera rate, IPD at start
    fit.json                    the fit coach's readings at the start and per section
    takes/<NN>-<section>/
      cam0/000000.zst ...       right eye: one zstd chunk per second, frames back to back (512x400 u8)
      cam1/000000.zst ...       left eye
      index.tsv                 per frame: camera, chunk, offset in it, slot, CLOCK_MONOTONIC_RAW time
      targets.jsonl             what the panel showed, when (direction, distance, state, background)
      prompts.jsonl             what the person was asked to do, when (as hands)
      poses.jsonl               head poses, 250 Hz (as hands; needed for section 10)
      ipd.jsonl                 the IPD setting when it changes (90 Hz during the sweep)
      steamvr.jsonl             SteamVR's gaze from ft-gaze, per sample (a baseline, not a label)
      own.jsonl                 ft-eyes' pupils, glints and gaze (a baseline, not a label)
      take.json                 status, deleted ranges, parts, clock samples, as hands
  exports/<session>/
```

The export recompresses nothing (the chunks are zstd already), drops deleted ranges by rewriting the chunks they touch, encrypts each chunk to `recipients.txt` (`.zst.age`), and writes the manifest and `SHA256SUMS` over the encrypted files. It never includes `/persist` files: unlike hands, nothing here needs the unit's calibration, and the eye cameras' isn't in `/persist` anyway (2026-10-14: `device_config.json` has the displays' transforms only).

## Shared with the hand recorder

Much of `hands/rec/` isn't about hands: the headset button reader, profile and consent, take bookkeeping (deleted ranges, redo, parts, clock samples), the Hugging Face login and upload, validate's manifest and checksum checks, and most of the window. These move to `rec/common/` (or `ft/rec/`), and both recorders use them. That's a refactor of the hand recorder, so it's done first, on its own commit, with the hand recorder's tests passing before anything eye-specific lands on it.

## Build order

1. **Headset test 1:** do the eye cameras move with the IPD wheel? (Look at one dot, turn the wheel end to end, watch ft-eyes' pupil x.) Decides the IPD guidance.
2. Pull the shared recorder code out of `hands/rec/` (hand recorder tests pass).
3. `eyerec.py` (frame writer) and its replay test against a capture.
4. `fitcoach.py` with readings from a capture, and the fit card on the existing fit panel.
5. ft-gazepanel `rec` mode, `path`, `distance`, target logging.
6. `session.py` and `script.json`, run with `--dry-run`, then on a replay.
7. Review, export with encryption, `validate.py`, `hub.py` against a test dataset.
8. The window, then the standalone repo.
9. The put-on check in the gaze service.

Not in scope here: the training. The dataset format is designed so that frame-eyes or a later trainer can label pupils from the targets and `own.jsonl`, but that's its own project.
