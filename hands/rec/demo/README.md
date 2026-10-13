# Demo clips for the hand recorder

The v2 recording script shows a looping GIF for each part: the maintainer's own hands doing the
motions, as the headset shows them (Room View passthrough). These tools make them.

1. **Record.** `hands/rec/demo/ft-handdemo` opens a window on Frametop's desktop with the next part
   of `parts.json` and its motions. With Room View on, press the headset's right-side button: 3, 2, 1,
   then Frametop's panels hide, hand tracking parks (no drawn hands, no laser), gaze mode goes off
   and vr-record records the headset view with the mic. Say the part's number, do the motions,
   press again: the take stops, everything comes back, and the next part shows. The part and its
   motions also stay up on a panel fixed to the head (`hands/rec/build/ft-handpanel`, built by
   `hands/rec/build.sh`), raised so only its bottom strip dips into the recorded view
   (`--panel-up`, `--panel-width`, `--no-panel`). SteamVR takes the same button press for Steam's
   menu, so the app closes the menu after each press and as a take starts. Twice during a take:
   stop and redo the part (twice otherwise: back one part). Hold: end. Takes go to
   `~/Videos/Recordings/hand-demos/<date time>/<part>-take<N>.mp4`, listed in `takes.jsonl`.
   `--from 7a` starts at a part, `--out DIR` carries on in a folder, `--dry` records nothing.
2. **Sheets.** `frame-job --local -- python3 hands/rec/demo/sheets.py SESSION` makes a contact sheet
   per take (a frame every 0.5 s with its time) and checks the spoken number with whisper.cpp
   (`heard.json`; a take whose words don't hold its part's number is printed).
3. **Trim by hand.** From the sheets, write `SESSION/trims.json`: per part, the take to use and its in
   and out seconds (about 10 s; up to 15 s for 2b, 7a and 11a), optionally a crop of the 1920x1080 view (without one, gifs.py crops off the panel's strip at the
   top, from `session.json`).
4. **GIFs.** `frame-job --local -- python3 hands/rec/demo/gifs.py SESSION` writes `SESSION/gifs/<part>.gif`
   (640 px, 10 fps, gifsicle `--lossy=60`; `--width`, `--fps`, `--lossy` to make them smaller).

The display shows less than the cameras see: look toward your hands in each take. VR Recorder's
censor blocks are applied to the takes too; the GIFs are public, so leave them on and face away
from what they cover.

## Recorded clips from a recorder session

The window's "Recorded hands" guidance plays clips of real hands from a Hand Recorder session that
ran the v2 script, instead of the drawn ones:
`frame-job --local -- PYTHON hands/rec/demo/camgifs.py SESSION_DIR` (PYTHON with NumPy, e.g.
frame-hands' `.venv/bin/python`) writes `hands/rec/demos-recorded/<id>.gif`. Each part uses its
right-hand step (or its both-hands step), redrawn from the two side cameras as a plain picture.
`--sheets` writes contact sheets first; `--view ID=PITCH,HFOV` aims a part's view, kept in
`views.json` next to the clips. They show the room: check them before they go anywhere public.
