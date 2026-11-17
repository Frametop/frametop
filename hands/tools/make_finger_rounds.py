#!/usr/bin/env python3
"""Writes the 2026-11-17 finger rounds (single-fingers, finger-play, hands-together) into hands/rec/bounties/
(DESIGN.md, "Bounties"). The acts are drawn here once, with fixed seeds, so the JSON is the round as shipped;
edit this and run it again rather than editing the JSON's acts by hand.

  python3 hands/tools/make_finger_rounds.py hands/rec/bounties
"""
import json
import os
import random
import sys

OUT = sys.argv[1]

HAND_SIZE = {
    "id": "hand-size", "title": "Hand size", "requires": [], "go": "Hold", "quick": True,
    "intro": "First, your hand size: both hands up about 40 cm out, still.",
    "prompts": [{"text": "Both hands about 40 cm out, still|Palms toward you", "seconds": 6, "hands": "both",
                 "distance": "mid",
                 "switch": [{"at": 0.5, "to": "backs toward you",
                             "text": "Both hands about 40 cm out, still|Backs toward you"}]}],
}


def round_script(name, title, welcome, sections):
    return {
        "version": 1, "name": name, "record_hz": 30, "intro_s": 4, "between_s": 3,
        "welcome": {"title": title, "seconds": 8, "text": welcome},
        "done": {"title": "All done", "seconds": 6,
                 "text": "Thank you! This recording session will close. Continue in the application.|"
                         "Review your recordings in the Hand recorder window."},
        "stopped": {"title": "Stopped", "seconds": 4,
                    "text": "What was recorded so far is saved. Review it in the Hand recorder window."},
        "sections": [HAND_SIZE] + sections,
    }


# Piano fingering: the act's label says which finger, machine-readable in prompts.jsonl (act.label) and drawn
# on the lit button. A poke label ending in 3, 4 or 5 (after an optional L_ or R_) names another finger than the
# index, so it isn't the index poke the open cues assume (DESIGN.md); every other poke label is an index poke.
FINGER_NO = {"index": "2", "middle": "3", "ring": "4", "little": "5"}


def finger_pokes(side, seed):
    """Pokes with a named finger, each finger twice, in a fixed shuffled order."""
    rng = random.Random(seed)
    fingers = ["index", "middle", "ring", "little"] * 2
    rng.shuffle(fingers)
    hand = side.capitalize() + " hand"
    acts = []
    for k, f in enumerate(fingers):
        acts.append({"s": 2.2, "win": "a", "lit": "button:%d" % ((k * 4 + rng.randrange(2)) % 6), "gesture": "poke",
                     "label": FINGER_NO[f], "say": "%s: poke with your %s finger (%s)" % (hand, f, FINGER_NO[f])})
        acts.append({"s": 0.8, "win": "a", "gesture": "rest", "say": "%s: back a little, fingers relaxed" % hand})
    return acts


WELCOME = ("Steps without windows come first: do what each one says with your fingers while the cameras record "
           "your hands. Then windows will appear in front of you: they don't move or click.|"
           "Press the button on the right side of the headset to start each step. Pressed while recording, it pauses.")

single = round_script("single-fingers", "One finger at a time", WELCOME, [
    {"id": "fingers", "title": "One finger at a time",
     "intro": "Move each finger on its own, mostly with your palm toward you, near and far. Keep the other fingers "
              "as still as you can. There's no wrong way: what helps is every finger moving all the way, on its own.",
     "prompts": [
         {"id": "fingers/right-mid", "hands": "right", "distance": "mid", "pose": "flat", "seconds": 16,
          "text": "Right hand up, palm toward you, about 40 cm out. Bend each finger all the way down and back up,|"
                  "one at a time, thumb to little finger. Keep the others still.",
          "switch": [{"at": 0.5, "to": "little finger to thumb"}]},
         {"id": "fingers/left-mid", "hands": "left", "distance": "mid", "pose": "flat", "seconds": 16,
          "text": "Left hand up, palm toward you, about 40 cm out. Bend each finger all the way down and back up,|"
                  "one at a time, thumb to little finger. Keep the others still.",
          "switch": [{"at": 0.5, "to": "little finger to thumb"}]},
         {"id": "fingers/right-close", "hands": "right", "distance": "near", "pose": "flat", "seconds": 16,
          "text": "Right hand close, palm toward you, about a forearm's length from your eyes.|"
                  "One finger at a time, all the way down and back up.",
          "switch": [{"at": 0.5, "to": "fast, in any order"}]},
         {"id": "fingers/left-close", "hands": "left", "distance": "near", "pose": "flat", "seconds": 16,
          "text": "Left hand close, palm toward you, about a forearm's length from your eyes.|"
                  "One finger at a time, all the way down and back up.",
          "switch": [{"at": 0.5, "to": "fast, in any order"}]},
         {"id": "fingers/odd-ways", "hands": "any", "pose": "spread", "seconds": 20,
          "text": "One hand, palm toward you. Bend only the fingertip's knuckle, then only the base knuckle,|"
                  "wiggle fingers sideways, spread them in pairs, cross the index over the middle.",
          "switch": [{"at": 0.5, "to": "the other hand"}]},
         {"id": "fingers/both-different", "hands": "both", "seconds": 16,
          "text": "Both hands up, palms toward you. Move a different finger on each hand at the same time,|"
                  "never the same one on both.",
          "switch": [{"at": 0.5, "to": "hands closer to your face"}]},
         {"id": "fingers/moving", "hands": "any", "pose": "flat", "seconds": 16,
          "text": "One hand, palm toward you. Keep moving fingers one at a time while the hand travels:|"
                  "to the edge of your view, low, high, close to your face and back out.",
          "switch": [{"at": 0.5, "to": "the other hand"}]},
         {"id": "fingers/backs", "hands": "any", "pose": "flat-back", "seconds": 12,
          "text": "Now the back of the hand toward you, fingers up.|One finger at a time, down and back up.",
          "switch": [{"at": 0.5, "to": "the other hand"}]},
     ]},
    {"id": "finger-pokes", "title": "One finger pokes", "kind": "scene",
     "intro": "A window close to you. Poke each lit button with the finger it names, keeping the other fingers out "
              "of the way. Nothing happens when you do: the cameras record your hand.",
     "prompts": [
         {"id": "right", "hands": "right",
          "text": "Right hand: poke the lit button with the finger it names, and only that finger.|"
                  "This window is close: about a forearm's length from your eyes.",
          "windows": [{"id": "a", "at": [0.06, -0.1, -0.3], "size": [0.26, 0.18], "look": "grid:3x2"}],
          "acts": finger_pokes("right", "single-fingers/right")},
         {"id": "left", "hands": "left",
          "text": "Left hand: poke the lit button with the finger it names, and only that finger.|"
                  "Let the hand come close to your face.",
          "windows": [{"id": "a", "at": [-0.06, -0.1, -0.3], "size": [0.26, 0.18], "look": "grid:3x2"}],
          "acts": finger_pokes("left", "single-fingers/left")},
     ]},
])

PIANO_ORDER = {"right": ["index", "middle", "ring", "little"], "left": ["little", "ring", "middle", "index"]}


# Presses per finger in a one-hand piano step (10): the ring and little fingers, the ones the board's data has
# least of moving alone, get as many as the index. A two-hand step gets each finger of each hand once.
PIANO_ONE = {"index": 3, "middle": 2, "ring": 3, "little": 2}


def _no_repeats(rng, items):
    """items shuffled with no finger twice running."""
    while True:
        rng.shuffle(items)
        if all(x != y for x, y in zip(items, items[1:])):
            return items


def piano(sides, seed, lit_s, rest_s):
    """Keys at finger pitch (grid:4x1, 0.12 m: about 2.7 cm apart), one finger over each: index to little
    finger left to right for the right hand, little to index for the left. sides: {window: hand}; with two,
    the hands take turns. Each press names its finger; with two hands, the hand too (L_ or R_ before it)."""
    rng = random.Random(seed)
    wins = list(sides)
    per = {w: _no_repeats(rng, [f for f, n in PIANO_ONE.items() for _ in range(n)] if len(wins) == 1
                          else list(PIANO_ORDER[sides[w]])) for w in wins}
    acts = []
    for j in range(len(per[wins[0]]) * len(wins)):
        w = wins[j % len(wins)]
        h, f = sides[w], per[w][j // len(wins)]
        act = {"s": lit_s, "win": w, "lit": "button:%d" % PIANO_ORDER[h].index(f), "gesture": "poke",
               "label": FINGER_NO[f], "say": "Press it with your %s finger (%s)" % (f, FINGER_NO[f])}
        if len(wins) > 1:
            act.update(hand=h, label=("L_" if h == "left" else "R_") + FINGER_NO[f],
                       say="%s hand, %s finger (%s)" % (h.capitalize(), f, FINGER_NO[f]))
        acts.append(act)
        acts.append({"s": rest_s, "win": w, "gesture": "rest", "say": "Fingers resting over the keys"})
    return acts


play = round_script("finger-play", "Finger play", WELCOME, [
    {"id": "play", "title": "Finger play",
     "intro": "Anything with your fingers, as long as they each do something different. The steps give ideas: "
              "there's no wrong way, so make it odd. Mostly palm toward you, near and far.",
     "prompts": [
         {"id": "play/piano-right", "hands": "right", "pose": "flat", "seconds": 18,
          "text": "Right hand, palm toward you, about 40 cm out. Play an invisible piano in the air:|"
                  "each finger pressing on its own, fast and slow.",
          "switch": [{"at": 0.5, "to": "close to your face",
                      "text": "Keep playing, the hand close now:|about a forearm's length from your eyes."}]},
         {"id": "play/piano-left", "hands": "left", "pose": "flat", "seconds": 18,
          "text": "Left hand, palm toward you, about 40 cm out. Play an invisible piano in the air:|"
                  "each finger pressing on its own, fast and slow.",
          "switch": [{"at": 0.5, "to": "close to your face",
                      "text": "Keep playing, the hand close now:|about a forearm's length from your eyes."}]},
         {"id": "play/count", "hands": "any", "pose": "open", "seconds": 18,
          "text": "Palm toward you. Count on your fingers in a strange order, raising and dropping them one at a time,|"
                  "skipping around. Then two at a time that aren't next to each other.",
          "switch": [{"at": 0.5, "to": "the other hand"}]},
         {"id": "play/wave", "hands": "any", "pose": "open", "distance": "near", "seconds": 16,
          "text": "Hand close, a forearm's length from your eyes, palm toward you.|"
                  "Ripple the fingers in a wave, little to index and back, then flick each out from under the thumb.",
          "switch": [{"at": 0.5, "to": "the other hand"}]},
         {"id": "play/shapes", "hands": "any", "pose": "ok", "seconds": 18,
          "text": "Palm toward you. Make shapes, a new one every second or two: rock horns, a finger gun,|"
                  "the OK sign, crossed fingers, a claw, or anything else.",
          "switch": [{"at": 0.5, "to": "the other hand"}]},
         {"id": "play/turning", "hands": "any", "pose": "turn-in-out", "seconds": 16,
          "text": "Keep the fingers moving while you slowly turn the hand: palm toward you,|"
                  "on its side, back toward you, and round again.",
          "switch": [{"at": 0.5, "to": "the other hand"}]},
         {"id": "play/both", "hands": "both", "seconds": 16,
          "text": "Both hands, palms toward you, each doing its own thing:|"
                  "one playing piano while the other counts, or anything else.",
          "switch": [{"at": 0.5, "to": "hands closer to your face"}]},
         {"id": "play/reach", "hands": "any", "pose": "open", "seconds": 16,
          "text": "Palm toward you, fingers moving the whole time: start at arm's length,|"
                  "bring the hand all the way in to a forearm's length from your eyes and back out.",
          "switch": [{"at": 0.5, "to": "the other hand"}]},
     ]},
    {"id": "piano", "title": "Piano keys", "kind": "scene",
     "intro": "Keys low in front of you, like a piano, one for each finger from index to little finger. "
              "Rest your fingers over them and press the lit key with the finger it names. Nothing happens when "
              "you do: the cameras record your hand.",
     "prompts": [
         {"id": "right", "hands": "right",
          "text": "Right hand over the keys, index finger on the left key, little finger on the right.|"
                  "Press the lit key with the finger it names, the others staying over their keys.",
          "windows": [{"id": "a", "at": [0.04, -0.24, -0.28], "size": [0.12, 0.08], "look": "grid:4x1"}],
          "acts": piano({"a": "right"}, "finger-play/piano-right", 1.1, 0.4)},
         {"id": "left", "hands": "left",
          "text": "Left hand over the keys, little finger on the left key, index finger on the right.|"
                  "Press the lit key with the finger it names, the others staying over their keys.",
          "windows": [{"id": "a", "at": [-0.04, -0.24, -0.28], "size": [0.12, 0.08], "look": "grid:4x1"}],
          "acts": piano({"a": "left"}, "finger-play/piano-left", 1.1, 0.4)},
         {"id": "both-hands", "hands": "both",
          "text": "Both hands, one over each set of keys, index fingers in the middle. Each lit key says L or R|"
                  "and a finger: press it with that finger of that hand.",
          "windows": [{"id": "a", "at": [-0.08, -0.24, -0.28], "size": [0.12, 0.08], "look": "grid:4x1"},
                      {"id": "b", "at": [0.08, -0.24, -0.28], "size": [0.12, 0.08], "look": "grid:4x1"}],
          "acts": piano({"a": "left", "b": "right"}, "finger-play/piano-both", 2.0, 0.5)},
     ]},
])


def crossed_taps():
    """Arms crossed: the right hand on the left window (R), the left hand on the right window (L)."""
    rng = random.Random("hands-together/crossed-taps")
    acts = []
    for k in range(8):
        right = k % 2 == 0
        acts.append({"s": 2.2, "win": "a" if right else "b", "lit": "button:%d" % rng.randrange(4), "gesture": "poke",
                     "hand": "right" if right else "left", "label": "R" if right else "L",
                     "say": "Arms crossed: poke it with your %s hand" % ("right" if right else "left")})
        acts.append({"s": 0.8, "win": "a" if right else "b", "gesture": "rest", "say": "Arms crossed, keep them there"})
    return acts


together = round_script("hands-together", "Hands touching and crossing", WELCOME, [
    {"id": "together", "title": "Hands together", "defaults": {"hands": "both"},
     "intro": "Both hands touching, laced and crossing each other, mostly with your palms toward you. "
              "There's no wrong way: what helps is the fingers of both hands moving while they touch.",
     "prompts": [
         {"id": "together/tips", "seconds": 16,
          "text": "Both hands up, palms toward you. Touch matching fingertips together, like a steeple,|"
                  "then tap each pair of fingertips together one at a time.",
          "switch": [{"at": 0.5, "to": "hands close to your face"}]},
         {"id": "together/laced", "pose": "overlap", "seconds": 16,
          "text": "Lace your fingers together, open and close them,|"
                  "twist the hands around and pull them apart again."},
         {"id": "together/counting", "pose": "count-5", "seconds": 16,
          "text": "Touch each finger of your left hand with your right index, as if counting on it.|"
                  "Palms toward you.",
          "switch": [{"at": 0.5, "to": "the left index counting",
                      "text": "Now the other way: your left index touches each finger of your right hand.|"
                              "Palms toward you."}]},
         {"id": "together/crossed", "pose": "cross", "seconds": 16,
          "text": "Cross your wrists in an X, palms toward you, and keep the fingers moving.|"
                  "Uncross and cross the other way round.",
          "switch": [{"at": 0.5, "to": "each hand on the other side",
                      "text": "Arms crossed: right hand out to your left, left hand out to your right,|"
                              "fingers moving."}]},
         {"id": "together/passing", "pose": "overlap", "seconds": 16,
          "text": "Pass one hand slowly in front of the other and back, palms toward you,|"
                  "fingers moving.",
          "switch": [{"at": 0.5, "to": "backs toward you"}]},
         {"id": "together/holding", "seconds": 16,
          "text": "Hold your left wrist with your right hand while the left hand's fingers move, its palm toward you.|"
                  "Then hold one of its fingers instead.",
          "switch": [{"at": 0.5, "to": "the other way round",
                      "text": "Now the left hand holds the right wrist, then one of its fingers,|"
                              "while the right hand's fingers move, its palm toward you."}]},
         {"id": "together/contact", "pose": "overlap", "seconds": 16,
          "text": "Slow claps, rub your palms together, then stack the hands:|"
                  "one palm on the other's back, swapping which is on top."},
         {"id": "together/close", "distance": "near", "seconds": 14,
          "text": "Any of these close to your face, about a forearm's length away, palms toward you:|"
                  "fingertips touching, fingers laced, one hand over the other."},
     ]},
    {"id": "crossed", "title": "Arms crossed", "kind": "scene",
     "intro": "Two windows, left and right. Use them with your arms crossed: the right hand on the left window, "
              "the left hand on the right. Nothing happens when you do: the cameras record your hands.",
     "prompts": [
         {"id": "crossed-pokes", "hands": "both",
          "text": "Arms crossed. Each lit button says L or R: poke it with that hand,|"
                  "the right hand on the left window and the left hand on the right.",
          "windows": [{"id": "a", "at": [-0.16, -0.1, -0.4], "size": [0.22, 0.16], "look": "grid:2x2"},
                      {"id": "b", "at": [0.16, -0.1, -0.4], "size": [0.22, 0.16], "look": "grid:2x2"}],
          "acts": crossed_taps()},
         {"id": "crossed-pinch", "hands": "both",
          "text": "Arms crossed: the right hand pinches the lit left corner, the left hand the right corner.|"
                  "Stretch or turn the window like the ghost, then let go.",
          "windows": [{"id": "a", "at": [0, -0.12, -0.42], "size": [0.34, 0.22], "look": "plain"}],
          "acts": {"gen": "moves", "gesture": "pinch", "every_s": 3.5, "gap_s": 1.2, "count": 4,
                   "say": "Arms crossed: pinch both corners, like the ghost",
                   "moves": [{"grab": "tl+tr", "ghost": [0, 0, 0, 0, 0, 0, 1.3]},
                             {"grab": "bl+br", "ghost": [0, 0.06, 0, 25, 0, 0]},
                             {"grab": "tl+tr", "ghost": [0, -0.04, 0.08, 0, 0, 0, 0.8]},
                             {"grab": "bl+br", "ghost": [0.05, 0, 0, -25, 0, 0]}]}},
     ]},
])

for s in (single, play, together):
    with open(os.path.join(OUT, s["name"] + ".json"), "w") as f:
        json.dump(s, f, indent=1, ensure_ascii=False)
        f.write("\n")
    print("wrote", s["name"])
