#!/usr/bin/env python3
"""Train the desk classifier (pointer/helper/deskclass.h): each tracked hand on the keyboard, on the
mouse or free, from the features pointer/tools/deskfeat writes, labelled from the takes' prompts.

  desk_train.py OUT.txt NAME|DIR... [--hidden 24,24] [--epochs 3000] [--seed 1]

NAME: a take's files from tools/replay_dumps.sh and deskfeat: NAME.csv (the features) and
NAME.prompts.jsonl. DIR: every such pair in it. OUT.txt: the model, as deskclass.h's Model reads it (POINTER_DESK_MODEL).
Before the model on all of them, each session is held out once (a model on the others, scored on its
takes), so the report says how it does on a session it hasn't seen. A session: the takes named
SESSION-TAKE with the same SESSION (YYYYMMDD-HHMMSS, as replay_dumps.sh names a hand-recorder take); one
person each, though one person can have several (then that person is partly seen), and a take named
otherwise is a session of its own. The report gives the share put at the desk at 0.5
and at DESK_MIN (POINTER_DESK_MIN's default: what parks a hand).

Labels (hands/rec/DESIGN.md has the events; the countdowns, redone tries and hands a step doesn't
name get none):
- the desk-work round (hands/rec/bounties/desk-work.json): desk/type and the "typing" cues: both
  hands keys; desk/mouse and the "mouse" cues: the mouse hand mouse (the hand whose palm wanders more
  across the desk in that stretch), the other none; desk/rest, desk-scene/look-up and pinch-look:
  both at the desk (keys or mouse, either). desk-scene/reach-*: during a lit act the step's hand is
  free from REACT s in to the act's end, and at the desk from SETTLE s into the rest after it; the
  other hand at the desk throughout.
- a full session's desk-work section (script.json): "typing" keys, "mouse" as above, "switch" at the
  desk; a step without a pose (the demos) none.
- anything else: the hands a step names (left, right or both) free while its prompt or a lit act
  holds (a step without hands, or an intro: none).
The first CUE_SKIP s after each change of prompt get none: the hands are still moving there. A hand
labelled at the desk (keys, mouse or either) that is higher than DESK_Y below the eyes gets none: it
has left the desk (to the face, a cup) whatever the step says.
"""
import argparse
import json
import math
import os
import re
import sys

import numpy as np

FREE, KEYS, MOUSE = 0, 1, 2
DESK = (KEYS, MOUSE)
CUE_SKIP = 0.8
REACT = 0.6
SETTLE = 1.2
DESK_Y = -0.2
DESK_MIN = 0.8
SESSION = re.compile(r"^(\d{8}-\d{6})-")


def load_csv(path):
    with open(path) as f:
        head = f.readline().strip().split(",")
        rows = [list(map(float, line.split(","))) for line in f if line.strip()]
    a = np.array(rows, dtype=np.float64).reshape(-1, len(head))
    return head, a


def intervals(path):
    """The prompts as [(start, end, event)] in ns, the countdowns, waits and redone tries left out."""
    events = []
    with open(path) as f:
        for line in f:
            try:
                e = json.loads(line)
            except ValueError:
                continue
            if e.get("event") in ("prompt", "ready", "wait", "end", "redo", "pause", "resume"):
                events.append(e)
    events.sort(key=lambda e: e["t"])
    redone = [(e["from"], e["to"]) for e in events if e["event"] == "redo"]
    out, cur, paused = [], None, False
    for e in events:
        ev = e["event"]
        if ev == "redo":
            continue
        if cur is not None and ev in ("prompt", "ready", "wait", "end", "pause"):
            out.append((cur["t"], e["t"], cur))
            cur = None
        if ev == "pause":
            paused = True
        elif ev == "resume":
            paused = False
        elif ev == "prompt" and not paused:
            cur = e
    return [(a, b, e) for a, b, e in out if not any(a < to and b > fr for fr, to in redone)]


def label(head, rows, prompts):
    """Per row, the classes its label allows (a tuple), or None."""
    col = {n: i for i, n in enumerate(head)}
    t, side = rows[:, col["t"]], rows[:, col["side"]].astype(int)
    low = rows[:, col["palm_y"]] <= DESK_Y
    labels = [None] * len(rows)
    spans = intervals(prompts)

    def mouse_hand(a, b):
        sel = (t >= a) & (t < b)
        wander = []
        for s in (0, 1):
            m = sel & (side == s)
            w = rows[m, col["wander_x"]] + rows[m, col["wander_z"]]
            wander.append(float(w.mean()) if m.any() else -1)
        return 0 if wander[0] > wander[1] * 1.3 else 1

    def put(a, b, s, allowed):
        if b <= a:
            return
        sel = (t >= a) & (t < b) & (side == s)
        if allowed != (FREE,):
            sel &= low
        for i in np.nonzero(sel)[0]:
            labels[i] = allowed

    for a, b, e in spans:
        pid, pose, hands = e.get("id", ""), e.get("pose", ""), e.get("hands", "")
        act = e.get("act") or {}
        a0 = a + CUE_SKIP * 1e9
        sec = pid.split("/")[0]
        if pid.startswith("desk/") or (sec == "desk-work" and pose in ("typing", "mouse", "switch")):
            if pose == "typing" or pid == "desk/type":
                put(a0, b, 0, (KEYS,)), put(a0, b, 1, (KEYS,))
            elif pose == "mouse":
                m = mouse_hand(a0, b)
                put(a0, b, m, (MOUSE,))
            elif pose in ("switch", "flat") or pid == "desk/rest":
                put(a0, b, 0, DESK), put(a0, b, 1, DESK)
            continue
        if sec == "desk-work":
            continue  # an old demo step: typing, the mouse and the phone mixed
        if sec == "desk-scene":
            step = e.get("step") or pid
            if step.endswith("/look-up") or step.endswith("/pinch-look"):
                put(a0, b, 0, DESK), put(a0, b, 1, DESK)
            elif "/reach-" in step:
                h = 1 if step.endswith("right") else 0
                put(a, b, 1 - h, DESK)
                if act.get("gesture", "rest") != "rest":
                    put(a + REACT * 1e9, b, h, (FREE,))
                else:
                    put(a + SETTLE * 1e9, b, h, DESK)
            continue
        if act and act.get("gesture") == "rest":
            continue
        named = {"left": (0,), "right": (1,), "both": (0, 1)}.get(hands, ())
        if pid.endswith("/intro"):
            continue
        if e.get("position") == "down":
            continue  # low hands on a non-desk step: no telling
        for s in named:
            put(a0, b, s, (FREE,))
    return labels


class MLP:
    def __init__(self, sizes, rng):
        self.W = [rng.normal(0, math.sqrt(2 / i), (o, i)) for i, o in zip(sizes[:-1], sizes[1:])]
        self.b = [np.zeros(o) for o in sizes[1:]]

    def forward(self, x):
        acts = [x]
        for k, (W, b) in enumerate(zip(self.W, self.b)):
            z = acts[-1] @ W.T + b
            acts.append(np.maximum(z, 0) if k < len(self.W) - 1 else z)
        return acts

    def probs(self, x):
        z = self.forward(x)[-1]
        z = z - z.max(axis=1, keepdims=True)
        p = np.exp(z)
        return p / p.sum(axis=1, keepdims=True)

    def train(self, x, mask, weight, epochs, lr=3e-3, l2=1e-4):
        params = self.W + self.b
        m = [np.zeros_like(p) for p in params]
        v = [np.zeros_like(p) for p in params]
        for step in range(1, epochs + 1):
            acts = self.forward(x)
            z = acts[-1] - acts[-1].max(axis=1, keepdims=True)
            p = np.exp(z)
            p /= p.sum(axis=1, keepdims=True)
            # partial labels: -log of the allowed classes' total
            allowed = (p * mask).sum(axis=1, keepdims=True)
            q = p * mask / np.maximum(allowed, 1e-12)
            g = (p - q) * weight[:, None] / weight.sum()
            grads_W, grads_b = [None] * len(self.W), [None] * len(self.W)
            for k in range(len(self.W) - 1, -1, -1):
                grads_W[k] = g.T @ acts[k] + l2 * self.W[k]
                grads_b[k] = g.sum(axis=0)
                if k:
                    g = (g @ self.W[k]) * (acts[k] > 0)
            for i, (p_, gr) in enumerate(zip(params, grads_W + grads_b)):
                m[i] = 0.9 * m[i] + 0.1 * gr
                v[i] = 0.999 * v[i] + 0.001 * gr * gr
                p_ -= lr * (m[i] / (1 - 0.9 ** step)) / (np.sqrt(v[i] / (1 - 0.999 ** step)) + 1e-8)


def session(name):
    """A take's session (its group when held out): SESSION of SESSION-TAKE, else the take."""
    base = os.path.basename(name)
    m = SESSION.match(base)
    return m.group(1) if m else base


def dataset(names):
    """The labelled rows of all the takes: (feature names, x, the allowed classes as a mask, each
    row's session, each row's label). The features: the CSV's columns after t, side and id, not a
    model's p_* (deskfeat --model); every take must have the same."""
    xs, masks, groups, kinds = [], [], [], []
    feats = None
    for name in names:
        head, rows = load_csv(name + ".csv")
        cols = [i for i, n in enumerate(head) if n not in ("t", "side", "id") and not n.startswith("p_")]
        these = [head[i] for i in cols]
        if feats is None:
            feats = these
        elif these != feats:
            sys.exit("%s: other features than the first take's" % name)
        labels = label(head, rows, name + ".prompts.jsonl")
        keep = [i for i, l in enumerate(labels) if l is not None]
        if not keep:
            print("%s: no labelled rows" % os.path.basename(name), file=sys.stderr)
            continue
        xs.append(rows[np.ix_(keep, cols)])
        mk = np.zeros((len(keep), 3))
        for j, i in enumerate(keep):
            mk[j, list(labels[i])] = 1
        masks.append(mk)
        groups += [session(name)] * len(keep)
        kinds += [labels[i] for i in keep]
    if not xs:
        sys.exit("no labelled rows in any take")
    return feats, np.vstack(xs), np.vstack(masks), np.array(groups), kinds


def weights(kinds):
    """Each kind of label (free, keys, mouse, at the desk) weighs the same in all."""
    count = {}
    for k in kinds:
        count[k] = count.get(k, 0) + 1
    return np.array([len(kinds) / (len(count) * count[k]) for k in kinds])


def fit(x, mask, kinds, hidden, epochs, seed):
    mean, std = x.mean(axis=0), x.std(axis=0)
    std[std < 1e-9] = 1
    net = MLP([x.shape[1]] + hidden + [3], np.random.default_rng(seed))
    net.train((x - mean) / std, mask, weights(kinds), epochs)
    return net, mean, std


def score(net, mean, std, x, kinds):
    """Per kind of label: how many rows, the share the model gets right (free: P(desk) under 0.5;
    at the desk: over 0.5; keys and mouse: that class the likeliest) and the shares it puts at the
    desk (P(desk) 0.5 or more, and DESK_MIN or more: what parks a hand)."""
    p = net.probs((x - mean) / std)
    desk = p[:, KEYS] + p[:, MOUSE]
    out = {}
    for k in sorted(set(kinds)):
        sel = np.array([kk == k for kk in kinds])
        if k == (FREE,):
            ok = desk[sel] < 0.5
        elif k == DESK:
            ok = desk[sel] >= 0.5
        else:
            ok = p[sel].argmax(axis=1) == k[0]
        out[k] = (int(sel.sum()), float(ok.mean()), float((desk[sel] >= 0.5).mean()),
                  float((desk[sel] >= DESK_MIN).mean()))
    return out


NAMES = {(FREE,): "free", (KEYS,): "keys", (MOUSE,): "mouse", DESK: "desk"}


def write_model(path, names, net, mean, std):
    """Written beside it and renamed over it: the pointer helper never reads half a model."""
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        f.write("deskclass 1\nfeatures %d\nclasses free keys mouse\n" % len(names))
        f.write("# %s\n" % " ".join(names))
        f.write("mean %s\n" % " ".join("%.6g" % v for v in mean))
        f.write("std %s\n" % " ".join("%.6g" % v for v in std))
        for k, (W, b) in enumerate(zip(net.W, net.b)):
            f.write("layer %d %d %s\n" % (W.shape[1], W.shape[0], "relu" if k < len(net.W) - 1 else "none"))
            for row in W:
                f.write(" ".join("%.6g" % v for v in row) + "\n")
            f.write(" ".join("%.6g" % v for v in b) + "\n")
    os.replace(tmp, path)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("out")
    ap.add_argument("names", nargs="+")
    ap.add_argument("--hidden", default="24,24")
    ap.add_argument("--epochs", type=int, default=3000)
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    hidden = [int(v) for v in a.hidden.split(",") if v]
    takes = []
    for n in a.names:
        if os.path.isdir(n):
            takes += sorted(os.path.join(n, f[:-4]) for f in os.listdir(n)
                            if f.endswith(".csv") and os.path.exists(os.path.join(n, f[:-4] + ".prompts.jsonl")))
        else:
            takes.append(n)
    names, x, mask, groups, kinds = dataset(takes)
    print("%d labelled hand frames: %s" % (len(kinds), ", ".join(
        "%s %d" % (NAMES[k], sum(1 for kk in kinds if kk == k)) for k in sorted(set(kinds)))))
    print("held out, one session at a time (right; at the desk at 0.5 / %.1f):" % DESK_MIN)
    for g in sorted(set(groups)):
        tr, te = groups != g, groups == g
        if not tr.any():
            continue
        net, mean, std = fit(x[tr], mask[tr], [k for k, s in zip(kinds, tr) if s], hidden, a.epochs, a.seed)
        res = score(net, mean, std, x[te], [k for k, s in zip(kinds, te) if s])
        print("  %-16s %s" % (g, "  ".join("%s %d %.0f%% (%.0f/%.0f%%)" % (NAMES[k], n, 100 * r, 100 * d, 100 * d8)
                                            for k, (n, r, d, d8) in res.items())))
    net, mean, std = fit(x, mask, kinds, hidden, a.epochs, a.seed)
    res = score(net, mean, std, x, kinds)
    print("all (trained on): " + "  ".join("%s %.0f%%" % (NAMES[k], 100 * r) for k, (n, r, d, d8) in res.items()))
    write_model(a.out, names, net, mean, std)
    print("wrote", a.out)


if __name__ == "__main__":
    main()
