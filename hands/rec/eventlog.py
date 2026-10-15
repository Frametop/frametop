#!/usr/bin/env python3
"""What Frametop did with the hands during a testing session (hands/include/fh_events.h): the
panels' (ft-screens) and the pointer helper's decisions, one JSON line each with its time, what
was tried (kind) and how it went (outcome: done, refused, missed, ...). Beside the take's
live.bin and the button's marks they say what a mark or a spoken note was about ("that opened
the lazy susan" next to a lazy-susan "done"), and how many pokes, pinches and strokes worked.

While a part records, events-want sits in the run directory, so the writers append to
events.jsonl there; at the end the lines from this part's time go into the take's events.jsonl
(events-2.jsonl, ... for later parts) and the run directory's copy is removed.

Standard library only.
"""
import json
import os
import sys

from recorder.common import mono_ns

WANT = "events-want"
FILE = "events.jsonl"
OUTCOMES = ("done", "refused", "missed", "passed", "lost", "end", "info")


def read_lines(path, start=0):
    """The events in path from byte start: a list of dicts (lines that aren't one are skipped)."""
    out = []
    try:
        with open(path, "rb") as f:
            f.seek(start)
            data = f.read()
    except OSError:
        return out
    for line in data.split(b"\n"):
        try:
            e = json.loads(line)
        except ValueError:
            continue
        if isinstance(e, dict) and isinstance(e.get("t"), int) and isinstance(e.get("kind"), str):
            out.append(e)
    return out


def summary(events):
    """{kind: {outcome: n}} over events, outcomes in OUTCOMES' order."""
    by = {}
    for e in events:
        k = by.setdefault(str(e.get("kind")), {})
        o = str(e.get("outcome"))
        k[o] = k.get(o, 0) + 1
    return {k: {o: v[o] for o in sorted(v, key=lambda o: OUTCOMES.index(o) if o in OUTCOMES else 99)}
            for k, v in sorted(by.items())}


def describe(by):
    """"poke 4 done, 2 refused; lazy-susan 1 done" for the log."""
    return "; ".join("%s %s" % (k, ", ".join("%d %s" % (n, o) for o, n in v.items())) for k, v in by.items()) or "none"


class EventLog:
    """Asks the writers for events while a part records and copies them into path at stop()."""

    def __init__(self, run_dir, path):
        self.run_dir, self.path = run_dir, path
        self.start_ns = 0
        self._offset = 0

    def start(self):
        os.makedirs(self.run_dir, mode=0o700, exist_ok=True)
        src = os.path.join(self.run_dir, FILE)
        try:
            os.remove(src)   # a crashed session's: its lines aren't this part's
        except OSError:
            pass
        self._offset = 0
        self.start_ns = mono_ns()
        with open(os.path.join(self.run_dir, WANT), "w") as f:
            f.write("%d\n" % os.getpid())
        return self

    def stop(self):
        """Ends it: {"events": n, "by": summary}."""
        end_ns = mono_ns()
        stop_writers(self.run_dir)
        src = os.path.join(self.run_dir, FILE)
        events = [e for e in read_lines(src, self._offset) if self.start_ns <= e["t"] <= end_ns]
        with open(self.path, "w") as f:
            for e in events:
                f.write(json.dumps(e, separators=(", ", ": ")) + "\n")
        try:
            os.remove(src)
        except OSError:
            pass
        return {"events": len(events), "by": summary(events)}


def stop_writers(run_dir):
    """No more events (also for a session that crashed with them asked for)."""
    try:
        os.remove(os.path.join(run_dir, WANT))
    except OSError:
        pass


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("eventlog.py EVENTS.JSONL: how many of each kind of interaction worked")
    print(json.dumps(summary(read_lines(sys.argv[1])), indent=1))
