# Eye recorder: design (2026-10-14)

The eye recorder guides a person through recording their eyes with the headset's two eye cameras, at known gaze targets and through the cases where eye tracking breaks. It also fits the headset: it shows whether each eye sits where the cameras see it best, helps set the IPD, and catches the headset shifting on the face, with instructions to put it back. Recordings stay on the headset until the person uploads them. Only the maintainers can read what's uploaded (below, "Privacy").

It's the hand recorder's sibling (`hands/rec/DESIGN.md`): the same session runner pattern, headset button, review, export and Hugging Face upload, with eye cameras instead of hand cameras. Like the hand recorder it ships in Frametop and, later, as a standalone app for people without Frametop (Frametop/frametop-eye-recorder, Frametop as a submodule).

The data is for training a better eye tracker than `gaze/tracker/` (ft-eyes, a classic pupil and glint finder) and SteamVR's, one that holds up at the edges of view, through blinks and squints, with glasses and makeup, and when the headset moves.

## Decisions (2026-10-14)

| | Decided |
|---|---|
| Who reads the recordings | Maintainers only. The eye cameras see the iris, which identifies a person. Trained models can be public; the images never are. |
| How | Encrypted frames in a gated dataset, `DeeJanuz/frametop-eyes` (below, "Privacy"). |
| Keys | Two maintainer keys, both recipients of every upload: key 1 on the maintainer's training machine (the 7i), key 2 kept offline in the maintainer's password manager. |
| Packaging | Backend here in `gaze/rec/`, then a standalone repo with Frametop as a submodule, as the hand recorder does. |
| Sessions | Short rounds picked from a bounty board (below, "Bounty board"), not one long session. Each round starts with a fit check and 9 dots. |
| Bounty list | `bounties.json` in the dataset repo, fetched by the app (a copy ships for offline use). The board shows your own done marks and a maintainer-set priority tag per bounty, not community counts. |
| Fit and slip source | Our tracker, ft-eyes: pupil and glint positions in the camera image. It needs ft-eyegrab, the root service `gaze/tracker/install.sh` installs. |
| Where the fit coach runs | A fitment step in the recorder before each round, for better recordings, and the put-on and slip check in Frametop's gaze service. |
| Shared code | The hand recorder's non-hand parts move into a shared module first, and the bounty board is built there too, so the hand recorder can use it next. |
| License | As hands: contributors give the maintainer a license for any use, including commercial models; the eye images and health answers are never published or shared outside the maintainers (user, 2026-10-14). |
| Standalone repo | Created after the window and a full round pass in the headset; step 9 comes first (user, 2026-10-14). |
| Checklist | Glasses/inserts/contacts, eye makeup, measured IPD, and eye surgery or conditions. That last one is health data: a separate opt-in, and its answers are encrypted like the images. |

## Privacy

A private Hugging Face repo can't take contributions: people outside the org can't see it, so they can't open a pull request, and org membership would let every member read every contribution. So the dataset is gated like `DeeJanuz/frametop-hands`, and the export encrypts what identifies a person before upload:
- Every frame chunk is encrypted with [age](https://age-encryption.org) (pyrage, which has aarch64 wheels: `pyrage-1.4.0-cp310-abi3-manylinux_2_17_aarch64`) to the recipients in `recipients.txt`: the maintainers' public keys: key 1 on the 7i, key 2 offline. The private keys never go in a repo.
- The health answers go in `health.json.age`, encrypted the same way.
- Labels, timings, fit readings, the IPD and the rest of session.json stay readable, so `validate.py` and a review can check a pull request before anyone decrypts it.
- Contributors open pull requests from their own accounts, so `hub.py` and the review tools carry over from hands.

The risk: if a private key leaks, every upload made to it can be read. A new key is one more recipient for new uploads; old uploads stay readable to the old key until they're re-encrypted. Withdrawal means deleting the files and squashing history, as with hands.

## What's measured already

- **The eye cameras** (`gaze/tracker/ft-eyegrab.c`): 512x400 8-bit IR, about 90 fps each when SteamVR runs them at 90 (`gaze/eye-rate.py`; at its default 15 fps a recording is worth little, so the recorder checks the rate first). Camera 0 is the right eye (steep angle), camera 1 the left (head-on). ft-eyegrab copies the frames read-only into `/dev/shm/frametop-eyes-cams` while someone touches `/dev/shm/frametop-eyes-want`.
- **Size.** Raw, both cameras are 36.9 MB/s, 2.2 GB a minute. zstd level 1 on 200 MB of the `fit1` capture (2026-10-14): 56 % of the size at about 250 MB/s on one core, so compressing while recording costs about 15 % of a core. Level 3: 53 %, 2.3x slower. Level 9: 49 %, 9.5x slower. The recorder uses level 1 while recording: about 1.2 GB per recorded minute, so a session of about 10 recorded minutes (below) is about 12 GB, close to a hand session.
- **IPD.** SteamVR reports it (2026-10-14, a probe against the live vrserver): `Prop_UserIpdMeters_Float` 0.0651, `Prop_IpdUIRangeMinMeters` 0.057, `Max` 0.071, `Prop_MinimumIpdStepMeters` 0.0015, and the eye-to-head transforms are ±IPD/2. `driver_cv.so` reads the range from the headset's EEPROM. So the recorder can read the IPD setting live and record it with every take. Not yet known: whether the eye cameras move with the lenses when the IPD changes. That decides how the IPD guidance works (below), and it's the first headset test.
- **ft-eyes** already finds each pupil (centre, ellipse) and glint pair per frame, keeps a per-eye slip estimate from the glints (`eyes_model.SlipTracker`), and resets its shift when frames stop for 3 s (the headset was off). `gaze/fitcheck.py` already keeps per-eye tracked and lost by gaze direction, with hints.

## Parts

| Part | What it does |
|---|---|
| `gaze/rec/session.py` (Python, no Qt) | The session runner, like `hands/rec/session.py`: reads a round's script, drives the panel, starts and stops the frame writer per step, reads the live fit for feedback, writes each take. `--dry-run`, `--next-after`, stdin steering and `--button-device` as in hands. |
| `gaze/rec/rounds/<id>.json` | Each kind of round's steps, targets and timings (below, "Bounty board"). |
| `gaze/rec/eyerec.py` | The frame writer: reads the shared frames (as `lab/ft-eyes-record` does) and writes zstd-1 chunks, one per second per camera, with an index (below, "Files"). A thread of the session, not a process: ft-eyes-record shows the copy needs no numpy. |
| `gaze/fitcoach.py` (shared) | The fit coach: from ft-eyes' pupil and glint positions, how the headset sits on this face, and what to do about it (below, "Fit coach"). The recorder, the Gaze page's fit check and the put-on check all use it. |
| ft-gazepanel `rec` mode | The headset panel. The gaze panel already draws dots at exact head-relative directions, which the ground truth needs, so it gains a `rec` mode with the hand panel's text, step, countdown and paused lines rather than a second panel (below, "Panel"). |
| `gaze/rec/takes.py`, `validate.py`, `hub.py` | Review, export, checks and upload, from `hands/rec/`'s. The shared parts move into a common module rather than being copied (below, "Shared with the hand recorder"). |
| `gaze/rec/ft_eyerec.py` + `main.qml` | The desktop window: consent, the bounty board, the checklist and eye camera check, the round with its fit card, review, export, upload. Launched by `ft-eyerec` (the dev container), installed to the app menu by `install.sh`. The standalone app gets a Qt Quick Controls window, as the hand recorder's does. |
| `gaze/rec/CONSENT.md`, `UPLOAD.md` | The texts. Eye images need a stronger consent than hands (biometric data, who can read it, how to withdraw). They need a review like `frame-hands/notes/legal-review-2026-10-03.md` before uploads open. |

## Fit coach

It reads, per eye, from ft-eyes: the pupil centre, its ellipse, the glint pair (both glints and their spacing), and how often the eye was found over the last second. It compares them with a reference: where a well-fitted eye sits in that camera's image. At first the reference comes from the maintainer's headset; once contributions come in, it comes from their rounds with good nine-dot fits (every round's fitment step records exactly that), per headset model. The coach says which reference it used, and until it's built from more than one face its hints are marked provisional.

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

The IPD row depends on the first headset test. If the cameras move with the lenses, a pupil's sideways offset in its image directly says how far that eye is from its lens axis, and the coach turns the IPD wheel's direction into "wider" or "narrower" until both offsets are about zero, at any IPD. If the cameras are fixed, the offset depends on the IPD setting too, and the coach needs the reference at each IPD step: the fitment step's IPD sweep (below, "Bounty board") builds that.

**Put-on and shift check.** After a good fit, the coach keeps that fit as the person's baseline (`~/.local/state/frametop/gaze/fit-baseline.json`: per eye, the pupil looking straight ahead and the glint pair, with the IPD setting). Then:
- When the headset goes back on (frames resume after ft-eyes' 3 s gap), it shows the fit card for a few seconds while the person looks ahead. If the pupils sit more than a threshold (start with 1.5 mm equivalent, tuned from data) from the baseline, it says which way to move the headset, live, until it's back. If the IPD setting changed, it says so first.
- While in use, ft-eyes' glint slip estimate already tracks small movement. A shift past the threshold that stays for 2 s gets a quiet notice ("Your headset slipped down: push it up, or look at the dot to recalibrate"), not a modal.
In the recorder the fit coach runs as each round's fitment step, and the shift warning runs during the round. In Frametop the put-on and slip check becomes part of the gaze service, off by default until it's been tried.

## Bounty board

People pick what to record from a board of bounties. Each bounty is one round of about 5 minutes, in step mode: the headset's right button starts each step (press = next or pause, two presses = redo, hold = stop), as in the hand recorder. Every round starts the same way:
1. **Fitment** (not recorded as a take, but its readings go into fit.json): the fit coach until the headset sits well (below, "Fit coach"). Skippable after a warning, and the round records that it was skipped. On a contributor's first round, and whenever the IPD setting has changed since their last, it ends with an IPD sweep: turn the wheel slowly from narrow to wide and back while looking at a centre dot, with the IPD logged as it changes (the panel looks 30 times a second, and on SteamVR's IPD event). Its own take, `NN-ipd`, before the nine dots.
2. **Nine dots** (recorded): a 3x3 grid to 20 degrees on the mid background. It's the round's own reference, so each round can be labelled alone, even after a re-seat.
3. **The bounty's steps.**

Every round uploads on its own. The board marks the bounties you've done, by your contributor id in `profile.json`, with how many times. You can do one again.

The launch bounties:

| Bounty | What | Edge case it covers |
|---|---|---|
| View edges | Dots at the panel's edges and corners: 30 degrees to the sides, 23 up and down, about 35 in the corners (the rec panel is 64 degrees across; every round's dots are checked against it) | Eyes turned far: the lid, the glints lost, the pupil foreshortened |
| Blinks and lids | Blink on cue, close both for 2 s, wink each eye, squint, eyes wide, half-closed, looking down with lids lowered | Closed and partly closed eyes; one eye missing |
| Headset slips | Nudge the headset up, down, left, right, tilt it; then take it off and put it back on; dots after each | Headset shifting on the face: the put-on check's data |
| Glasses | A 5x5 grid and the edges with glasses or inserts on (offered when the checklist says so) | Lens reflections, the glasses' frame in view |
| Jumps and pursuit | A dot jumping between random spots (0.5 to 1.5 s each), then moving on circles, lines and a figure eight at two speeds | Saccades, overshoot, smooth pursuit, the tracker's lag |
| Depth | Dots at 0.5 m, 1.5 m and 5 m in turn (the panel's distance changes) | Vergence; the eyes' angle with distance |
| Light changes | The background steps dark to bright and back while looking at a dot, then a 5x5 grid at each of three backgrounds | Pupil dilation and constriction; error against pupil size |
| Face moves | Smile, raise eyebrows, frown, yawn, talk (read a line aloud: the microphone isn't recorded), chew | The face moving the headset and the lids |
| Head turning | A dot fixed in the room while turning the head (head poses logged) | The eyes counter-rotating (VOR), headset jiggle |
| Reading | Lines of text to read, then a page of icons to look over | Natural use |

Later candidates, not at launch: eye makeup (the grid and blinks with mascara or eyeliner; the checklist still records makeup on every round), long wear (the grid after 30+ minutes in the headset).

### bounties.json

In the dataset repo, fetched when the board opens (a copy in the app for offline use, used when the fetch fails):

```json
{"schema": 1, "updated": "2026-10-14",
 "bounties": [{"id": "edges", "title": "View edges", "blurb": "...", "minutes": 5,
               "priority": "high|normal|low|retired", "script": "edges", "needs": {"glasses": true}}]}
```

`script` names a round in the app's `rounds/` folder: a new kind of round needs a release, but the list, wording and priorities don't. `needs` hides a bounty unless the checklist matches. A bounty id the app doesn't know is shown as "needs an update".

The board is in the shared recorder module, so the hand recorder's v2 sections can become bounties later.

### Checklist

Asked once and kept in profile.json, editable before each round, all optional, none identifying: glasses (none, glasses, prescription inserts, contacts), eye makeup today (none, mascara, eyeliner, lash extensions), the face pad and strap in use, and a measured IPD if they know it (from an optician). Separately, behind its own consent tick: eye surgery or conditions (LASIK or other surgery, a lazy or turned eye, a droopy lid, nystagmus, other). Those answers go into `health.json`, which the export encrypts. Eye colour isn't asked: the IR images show what matters about it.

## Panel

ft-gazepanel gains `show rec`: a large head-locked panel (FULL_DEG, 64 degrees, with a solid background whose brightness the script sets) that keeps every existing command (`dot`, `bg`, `title`, `text`, `note`, `eye`, `hints`) and adds what the hand panel has: `step`, `big` (countdown), `action`, `keys`, `rec on|off`, `paused on|off`. New for the ground truth:
- `path <kind> <params> <seconds>`: the panel moves a dot itself (circle, line, figure eight), so it's smooth at the headset's frame rate, not at the socket's.
- `distance <metres>`: moves the panel for the Depth bounty.
- `world <x> <y> <z>`: a dot fixed in the room (the Head turning bounty), as the hand panel's touch target.
- Every picture the panel shows with a dot logs that dot's direction and the frame's time to `targets.jsonl` (CLOCK_MONOTONIC_RAW, the eye frames' clock). The ground truth is what was on screen when, not what was asked.

## Files

```
~/.local/share/frametop/eyes/contrib/
  profile.json                  consent, contributor id, checklist, bounties done
  rounds/<YYYYMMDD-HHMMSS>-<bounty>/
    session.json                the bounty, checklist, versions, takes, eye camera rate, IPD at start
    fit.json                    the fit coach's readings: the fitment step, and any slip warnings
    health.json                 the opt-in health answers (encrypted in the export)
    takes/<NN>-<step group>/    numbered in order: NN-ipd (when there's a sweep), NN-dots (the nine dots), then the bounty's
      cam0/000000.zst ...       right eye: one zstd chunk per second, frames back to back (512x400 u8)
      cam1/000000.zst ...       left eye
      index.tsv                 per frame: camera, chunk, offset in it, slot, CLOCK_MONOTONIC_RAW time
      targets.jsonl             what the panel showed, when (direction, distance, state, background)
      prompts.jsonl             what the person was asked to do, when (as hands)
      poses.jsonl               head poses, 250 Hz (as hands; needed for Head turning)
      ipd.jsonl                 the IPD setting at the start and when it changes
      steamvr.jsonl             SteamVR's gaze from ft-gaze, per sample (a baseline, not a label)
      own.jsonl                 ft-eyes' pupils, glints and gaze (a baseline, not a label)
      take.json                 status, deleted ranges, parts, clock samples, as hands
  exports/<session>/
```

The export recompresses nothing (the chunks are zstd already), drops deleted ranges by rewriting the chunks they touch, encrypts each chunk and `health.json` to `recipients.txt` (`.age`), and writes the manifest and `SHA256SUMS` over the encrypted files. It never includes `/persist` files: unlike hands, nothing here needs the unit's calibration, and the eye cameras' isn't in `/persist` anyway (2026-10-14: `device_config.json` has the displays' transforms only).

## Shared with the hand recorder

Much of `hands/rec/` isn't about hands: the headset button reader, profile and consent, take bookkeeping (deleted ranges, redo, parts, clock samples), the Hugging Face login and upload, validate's manifest and checksum checks, and most of the window. These live in `recorder/` at the top of the tree, with the bounty board, and both recorders use them: `common.py`, `units.py`, `button.py`, `bounties.py`, `files.py` (ranges, checksums, sizes), `checks.py` (validate's report, SHA256SUMS, typed manifest lookups), `hub.py` (login, errors, upload records, the pull request flow), `age.py` (the export's encryption) and `window.py` (the window backend's shared half: consent, disk, session controls, export, login and upload; each recorder's `Backend` subclasses it). The hand recorder's modules keep their names on top of them, and its tests pass.

Uploads: `gaze/rec/hub.py` sends nothing to the real dataset until `CONSENT.md` and `UPLOAD.md` exist and aren't drafts. `FT_EYEREC_ALLOW_UPLOAD=1` opens rehearsals, and only to another dataset (`FT_EYEREC_DATASET`). Rehearsed 2026-10-14 against the private `DeeJanuz/frametop-eyes-test` with a round of random bytes: uploaded, downloaded, checked, and decrypted with both keys by age 1.1.1 on the 7i.

## Build order

1. **Headset test 1:** do the eye cameras move with the IPD wheel? (Look at one dot, turn the wheel end to end, watch ft-eyes' pupil x.) Decides the IPD guidance.
2. Pull the shared recorder code out of `hands/rec/` (hand recorder tests pass).
3. `eyerec.py` (frame writer) and its replay test against a capture.
4. `fitcoach.py` with readings from a capture, and the fit card on the existing fit panel.
5. ft-gazepanel `rec` mode, `path`, `distance`, target logging.
6. `session.py`, the nine-dot opener and the first rounds, run with `--dry-run`, then on a replay. The bounty board and `bounties.json`.
7. Review, export with encryption (two recipients), `validate.py`, `hub.py` against a test dataset.
8. The window, then the standalone repo.
9. The put-on check in the gaze service.

Before uploads open: the consent texts and a review like `frame-hands/notes/legal-review-2026-10-03.md`, with the health opt-in in it.

Not in scope here: the training. The dataset format is designed so that frame-eyes or a later trainer can label pupils from the targets and `own.jsonl`, but that's its own project.
