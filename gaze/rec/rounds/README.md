# Building an eye bounty

A bounty is two things:

- **A board entry** in `gaze/rec/bounties.json`: id, title, blurb, minutes, priority, the script it runs,
  and what it needs from the checklist (`recorder/bounties.py` has the format).
- **A round script**, `gaze/rec/rounds/<script>.json`: the steps (`session.py`'s docstring has the
  format of every step kind).

Scripts ship with the recorder. The dataset's copy of the board (`DeeJanuz/frametop-eyes`,
`bounties.json`) can only describe, reorder, reprioritise or retire bounties, so a new bounty reaches
people only with a Frametop release that carries its script.

`load_round` enforces the rules marked **(checked)**: a script that breaks one doesn't load, and the
tests fail.

## What the round is for

Every recorded second becomes training data, labelled by where the dot was. So:

- **Know what each step labels.** In most steps the eyes are on a dot, and the dot's position is the
  label. Where they aren't (eyes shut, reading, looking around freely), the cue words or the step's
  kind say so, and training leaves those frames out or treats them as their own case.
- **One edge case per bounty.** Say in the board's blurb and the DESIGN.md table which case it covers
  and why the trackers fail there.
- **Rounds open the same way.** The recorder adds putting the headset on and the nine dots before every
  round. Don't repeat them. The group `dots` is theirs **(checked)**.

## Cues the person can't see get a beep

The panel can't cue someone whose eyes are shut. A cue that asks for that plays a beep
(`gaze/rec/sounds.py`) as its third field: `["Close your eyes", 2.5, "close"]`.

- **Blink:** a cue whose word asks for a blink plays `blink` **(checked)**.
- **Both eyes shut:** a cue asking to close or shut both eyes plays `close` **(checked)**, and the next
  cue plays `open` **(checked)**, so the person hears when to open them. A step never ends with the eyes
  shut.
- **Say it in the step's text**, which is read before Next: "Close your eyes when you hear a beep, and
  then open your eyes when you hear the beep again." The words still show on the panel too.
- **One eye or half-closed** (winks, squints, half-closed lids) keep words alone: the open eye reads
  them. They may beep too.
- **No voice.** Beeps only; generated speech sounds odd in the headset. A new kind of cue sound goes
  into `sounds.SOUNDS` and the tests.

## Cue words are labels

Training reads the cue words in `prompts.jsonl` (`frame-eyes/train/data/ingest.py`, `shut_cams`):

- A word with **blink, close, shut, wink or yawn** marks the eyes as shut while it shows. **Left** or
  **right** limits that to one eye, and **half** means the lids are partly open, so the frames keep
  their labels.
- Use those words only for that. "Open", "Look", "Relax" and "Normal" are the words for going back.
- Keep words short (40 characters at most): they show big on the panel.

## Timing

- **Dots:** a hold of 1.2 s or more after a settle of about 0.6 s (the defaults). The eyes take that
  long to land on a dot and stay.
- **Shut eyes:** 2 s or more closed, then 2 s or more open before anything else, so the eyes settle back
  on the dot.
- **Blinks on a beep:** uneven gaps, 1.2 to 2 s, so the beep can't be predicted.
- **Length:** about 4 to 6 minutes recorded. Check with `python3 gaze/rec/session.py <script> --plan`, and
  put that, rounded, in the board's `minutes`.

## Where things can go

- **Dots and paths stay on the panel** **(checked)**: about 30 degrees to the sides, 23 up and down, less
  in the corners (`off_panel`).
- **Distance:** 0.25 to 10 m **(checked)**. 1.5 m unless distance is what the bounty is about.
- **Head still,** unless the bounty is about moving it: say "Keep your head still" in the text of dot steps.

## Takes

- Steps of the same `group` share one take, and a group's steps must follow each other **(checked)**.
  Group steps that a reviewer would delete together.
- A `reseat` step isn't recorded, and a round with one gets no shift check.
- Every step waits for Next unless it has `"ready": false`. Only chain steps that follow on naturally.

## Comfort, safety and privacy

- Comfortable eye turns only: "as far as is comfortable", never "as far as you can".
- No flashing: brightness changes are steps of a second or more (`light` levels), never a flicker.
- Nothing that asks for sound, the outward cameras, or anything about the person beyond the checklist.
  Talking is fine (it moves the face), but the microphone is never recorded.
- Text to read is neutral and made up: no names, places or questions about the person.

## Writing the text

- Plain words, one instruction per step, the action first: "Look at each dot until its ring fills."
- `|` starts a new line on the panel, `||` leaves a blank line.
- Say what to do, not why. The why goes in the round's `intro` or the board's blurb.

## Before it ships

1. `python3 gaze/rec/session.py <script> --plan`: the steps and their length.
2. The tests, in the dev container: `cd gaze/rec && for t in tests/test_*.py; do python3 $t; done`.
   `test_shipped_rounds_load` loads every script; update its count.
3. A dry run: `python3 gaze/rec/session.py <script> --dry-run --speed 20 --next-after 0.1`.
4. A replay end to end: `frame-eyes/tools/eyerec-dryrun <bounty>` (record, export, validate), and the
   export through `eyes_hub.py check` and `ingest.py` on dody. Delete those rounds afterwards: their
   labels are wrong.
5. A real round in the headset, with a note in `HEADSET-TESTS.md`: is every cue clear, and is the
   length right?
6. Add the bounty to `gaze/rec/bounties.json` and a row to DESIGN.md's bounty table. After a release
   carries the script, put it on the dataset's board too (`frame-eyes/dataset-repo/bounties.json`,
   uploaded to `DeeJanuz/frametop-eyes`).
