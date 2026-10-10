# Hands while typing on a real keyboard

Branch `hands-typing` (2026-10-10). The problem, from the user's headset session that day (hand
tracking with student-v10a): typing on the real keyboard made hands click (pinches, middle pinches,
grips, pokes), and the hand ray took the pointer from the mouse or the gaze during or right after
typing.

## What the logs showed (2026-10-10)

Sources: `journalctl --user -u frametop-pointer` (the helper), `-u frametop-hands`, `-u
frametop-input-relay`, and the four flags the user said "flag that" for
(`frame-hands/captures/flags/flag-20261010-145753`, `-145841`, `-145857`, `-150115`: 16 s of hands
in the head frame plus head poses). Keys aren't logged anywhere, so how long after a key a gesture
came is known only as "within 1 s" (the old guard's "ignored (no ray up; typing)") or not.

- The old guard (`POINTER_PINCH_TYPING`, 1 s) caught 23 pinches today (09:12-09:42 and
  14:59-15:02), always "no ray up" ones. It never looked at ray pinches, middle pinches, grips,
  ft-screens' pokes and grabs, or the ray showing.
- 14:57:10-14:57:30, typing into a text field on the TK-KB006 keyboard (connected 14:56:27, field
  focused 14:56:34): 11 pinches became "no ray up: a press where the pointer is, on nothing" and 2
  more were "ignored (a press is already held)". All began at 1.7-2.0 cm (deliberate pinches read
  0.4-1.0 cm), so typing thumbs. None was within 1 s of a key: typing pauses are longer than that.
- 14:57:37 the right hand's ray came up ("hand ray: none -> right (raised)") with the hand on the
  keyboard, and at 14:57:38 a typing pinch became a ray press on `frametop.remote.103.bar`, held
  1.89 s. Flag 145753 shows that moment: both palms 0.29-0.33 m under the eyes, 0.26-0.34 m ahead,
  palms down (0.90-1.00 of the normal pointing down), 0.17-0.22 m apart. The knuckles sat 0.29-0.31 m
  under the eyes, right at the ray's "raised" limit (0.30 m to show).
- 15:00:37 and 15:02:24: two more presses at the pointer between "ignored (typing)" pinches.
- 15:01:15 (flag 150115): both hands typing at 0.40-0.42 m under the eyes, 0.18-0.21 m ahead, palms
  down 0.84-0.97, 0.15-0.16 m apart; the right hand rose, gripped, dragged a panel, then came back
  to 0.28 m under the eyes, palm down 0.84, and a ray pinch clicked `frametop.screen.1`.
- Flags 145841 and 145857 are deliberate ray use: the pointing hand 0.15-0.30 m under the eyes and
  0.33-0.46 m ahead, palm anywhere from sideways to down; the other hand resting low (0.41-0.52 m
  under the eyes, -0.1 to 0.2 m ahead), 0.35-0.6 m away.
- "pinch right: left to ft-screens (the hand is near its panel)" bursts (14:58:26-14:58:42,
  09:14:51-09:15:02) are hands near a low panel; with typing nearby those are ft-screens' to judge.

The hand recorder's desk take (PR #3, `frame-hands/dumps/pr3-04-desk-work`, head poses known):

| phase | palms under the eyes | ahead | palm down | apart | height difference |
| --- | --- | --- | --- | --- | --- |
| typing | 0.36-0.39 m | 0.22-0.29 m | 0.85-0.99 | 0.17 m (p90 0.40) | 0.00-0.01 m |
| mouse (one hand on the keys) | 0.38-0.41 m | 0.21-0.29 m | 0.82-0.99 | 0.41 m | 0.00-0.02 m |
| switching | 0.37-0.40 m | 0.21-0.29 m | 0.82-1.00 | 0.17-0.41 m | 0.00-0.02 m |

Deliberate gestures in the same recording (`pr3-03-gestures`): the hand 0.0-0.10 m under the eyes,
0.26-0.37 m ahead. The touch take (`pr3-06-touch`, low dots) had the touching hand 0.24-0.30 m
under the eyes, palm down 0.7-0.9, but 0.10-0.17 m higher than the other hand.

So typing hands sit anywhere from 0.29 to 0.42 m under the eyes depending on the desk and chair,
which is also where deliberate gaze pinches with the elbow resting sit (0.35-0.45 m): no fixed
height tells them apart. What does: both hands down together (level within a few cm, 0.15-0.25 m
apart, palms down), and a hand that types rising well above where it typed when it's meant.

## What the branch does

`pointer/helper/typing.h` (tests: `pointer/tests/typing-test.cpp`) decides, per side, whether typing
holds a hand back; the pointer helper feeds it the relay's keys and each hands frame, and ft-screens
hears the result (`typing <L> <R>` on @ft_screens).

- Keys: for `POINTER_PINCH_TYPING` (2 s, was 1) after a key, every hand is held back, except one
  that has left the keyboard or is busy out of the keyboard zone (its ray up, a gesture held:
  pointing with one hand and pressing Enter with the other).
- On the keyboard: a hand in the keyboard zone (palm at least `POINTER_TYPING_BELOW`, 0.2 m, under
  the eyes, 0.05-0.45 m ahead, palm down) while typing is parked, and stays held back through any
  pause until it rises `POINTER_TYPING_RAISE` (0.08 m) above where it typed, or turns its palm up,
  for 0.1 s. Freed, it works at once, keys or not. A hand on the mouse beside the keyboard is parked
  too once a key comes (its pinches are mouse clicks). Unseen 3 s, it's forgotten.
- The posture: both hands in the zone, level within 8 cm and at most 32 cm apart, for
  `POINTER_TYPING_POSTURE` (0.3 s), is typing before any key, within `POINTER_TYPING_RECENT` (600 s)
  of a key. Without that limit, both hands resting on a desk would hold gaze pinches back.
- Held back: no pinch, middle pinch or grip (ray, gaze or plain) in the helper; no ray (an owner
  drops the pointer to the mouse or the gaze, "hand ray: right -> none (typing)"); no poke, near grab
  or fingertip cursor in ft-screens. A gesture already held carries on.

Replaying the four flags and the PR #3 takes through typing.h (`/tmp` driver, not in the repo; the
flags have no keys, so a key a minute before the window was assumed):

| recording | result |
| --- | --- |
| flag 145753 (typing, the ray click at 14:57:38) | both hands parked by the posture 0.3 s into the window, held back 98% of it; the 14:57:38 pinch came 0.27 s into the window, before the replay's posture (live, the hands had been on the keys since 14:57:10) |
| flag 150115 (typing, then the right hand up for a grip, a drag and a click) | posture at once; the right hand freed by its raise (0.18 m), its grip and two pinches allowed; the left held back 98% |
| flags 145841, 145857 (deliberate ray use, the other hand low) | nothing held back, all 11 pinches allowed |
| desk take, keys during the typing prompt | typing, mouse and switching: both hands held back 100% |
| desk take, no keys | the posture alone: typing 84-100%, mouse phase: the mouse hand free (0.41 m from the other hand) |
| gestures take (pinches, grabs, pointing) | nothing held back |
| touch take (low dots) | the posture took both hands while they waited low; the touching hand freed by its first raise, then held back 9% |
