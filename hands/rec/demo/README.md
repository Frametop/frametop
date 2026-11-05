# Demo clips for the hand recorder

The v2 recording script shows a looping GIF for each part of `parts.json` (`script-v2.json` is made
from it). There are two kinds (hands/rec/DESIGN.md): skeleton clips in `hands/rec/demos/`, generated
animation made outside the Frame, and recorded clips in `hands/rec/demos-recorded/`, made here from a
recorder session.

## Recorded clips from a recorder session

The window's "Recorded hands" guidance plays clips of real hands from a Hand Recorder session that
ran the v2 script, instead of the drawn ones:
`frame-job --local -- PYTHON hands/rec/demo/camgifs.py SESSION_DIR` (PYTHON with NumPy, e.g.
frame-hands' `.venv/bin/python`) writes `hands/rec/demos-recorded/<id>.gif`. Each part uses its
right-hand step (or its both-hands step), redrawn from the two side cameras as a plain picture.
`--sheets` writes contact sheets first; `--view ID=PITCH,HFOV` aims a part's view, kept in
`views.json` next to the clips. They show the room: check them before they go anywhere public.
