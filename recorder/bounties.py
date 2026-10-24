"""The bounty board (gaze/rec/DESIGN.md, "Bounty board"): what a recorder asks contributors for,
and which they've done. A bounty is a short round of recordings for one edge case; the board
says which are wanted most (priority) and the profile keeps how often each was done.

The board ships with the recorder (its bounties.json) and a newer copy is fetched from the
dataset when it can be. A fetched board is untrusted: it can only describe, reorder, reprioritise
or retire bounties. Each bounty's steps come from a round script (its "script" names one): one
shipped with the recorder, or for the hand recorder since 2026-10-23 one fetched from the
dataset beside the board (bounties/<script>.json) that the recorder checks as untrusted and
keeps only if it passes (hands/rec/session.py check_round: scene steps of known gestures, in
limits, nothing that changes what is recorded or where it goes). A shipped script always wins
over a fetched one of the same name. A bounty whose script the recorder has neither way is
listed as needing a newer recorder.

bounties.json:
  {"schema": 1, "updated": "YYYY-MM-DD", "bounties": [{"id", "title", "blurb", "minutes",
   "priority": "high" | "normal" | "low" | "retired", "script", "needs": {"glasses": true}}]}

The profile's part (profile.json, which the recorder's window keeps):
  "bounties": {"<id>": {"rounds": N, "last": "<round id>"}}

Standard library only.
"""
import json
import os
import re
import urllib.request

SCHEMA = 1
PRIORITIES = ("high", "normal", "low", "retired")
ID = re.compile(r"^[a-z0-9][a-z0-9-]{0,39}$")
MAX_BYTES = 256 * 1024        # a fetched board bigger than this isn't one
MAX_BOUNTIES = 100
# The checklist answers a bounty can need (the recorder's checklist keys), and how a profile's
# answer meets each.
NEEDS = {"glasses": lambda checklist: checklist.get("glasses") in ("glasses", "inserts")}


def _text(v, limit):
    if not isinstance(v, str):
        return ""
    v = " ".join(v.split())
    return v[:limit]


def validate(board):
    """A board as it came (dict), cleaned: only what this recorder knows, in its limits. Raises
    ValueError when it isn't a board at all."""
    if not isinstance(board, dict) or board.get("schema") != SCHEMA or not isinstance(board.get("bounties"), list):
        raise ValueError("not a bounty board (schema %d)" % SCHEMA)
    updated = board.get("updated")
    if not isinstance(updated, str) or not re.match(r"^\d{4}-\d\d-\d\d$", updated):
        raise ValueError("the board has no updated date")
    out, seen = [], set()
    for b in board["bounties"][:MAX_BOUNTIES]:
        if not isinstance(b, dict) or not isinstance(b.get("id"), str) or not ID.match(b["id"]) or b["id"] in seen:
            continue
        script = b.get("script", b["id"])
        if not isinstance(script, str) or not ID.match(script):
            continue
        minutes = b.get("minutes")
        needs = b.get("needs") if isinstance(b.get("needs"), dict) else {}
        seen.add(b["id"])
        out.append({"id": b["id"], "title": _text(b.get("title"), 60) or b["id"], "blurb": _text(b.get("blurb"), 400),
                    "minutes": float(minutes) if isinstance(minutes, (int, float)) and 0 < minutes <= 60 else None,
                    "priority": b.get("priority") if b.get("priority") in PRIORITIES else "normal",
                    "script": script, "needs": sorted(k for k, v in needs.items() if k in NEEDS and v is True)})
    return {"schema": SCHEMA, "updated": updated, "bounties": out}


def load(path):
    """A board file, validated, or None."""
    try:
        with open(path, encoding="utf-8") as f:
            return validate(json.load(f))
    except (OSError, ValueError):
        return None


def newest(shipped, fetched):
    """The newer of two boards (either may be None): the fetched one wins a tie, since it's the one
    the maintainers changed."""
    if not fetched:
        return shipped
    if not shipped:
        return fetched
    return fetched if fetched["updated"] >= shipped["updated"] else shipped


def fetch_json(url, token=None, max_bytes=MAX_BYTES, timeout=5.0):
    """A JSON file from url (the dataset is gated: token is the contributor's Hugging Face token),
    at most max_bytes. Raises OSError or ValueError."""
    req = urllib.request.Request(url, headers={"Authorization": "Bearer " + token} if token else {})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        data = r.read(max_bytes + 1)
    if len(data) > max_bytes:
        raise ValueError("it is too big")
    return json.loads(data.decode("utf-8"))


def keep_json(path, value):
    """value written to path whole (a temporary file, then renamed)."""
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(value, f, indent=1, ensure_ascii=False)
    os.replace(tmp, path)


def fetch(url, cache_path, token=None, timeout=5.0):
    """Fetch the board from url, validate it and keep it at cache_path. Returns the board; raises
    OSError or ValueError."""
    try:
        data = fetch_json(url, token, MAX_BYTES, timeout)
    except ValueError as e:
        raise ValueError("the board: %s" % e) from None
    board = validate(data)
    keep_json(cache_path, board)
    return board


def view(board, profile, checklist, have_script):
    """The board as the window lists it: the bounties that aren't retired, each with "done"
    (rounds recorded), "last" and "available" ("" when it can be done, else why not), sorted by
    priority, then the ones done least, then the board's own order (so a fetched board can rank
    bounties within a priority). have_script(name) says whether this recorder has that round
    script."""
    done = (profile or {}).get("bounties") or {}
    checklist = checklist or {}
    rank = {p: i for i, p in enumerate(PRIORITIES)}
    out = []
    for k, b in enumerate((board or {}).get("bounties", [])):
        if b["priority"] == "retired":
            continue
        mark = done.get(b["id"]) if isinstance(done.get(b["id"]), dict) else {}
        why = ""
        if not have_script(b["script"]):
            why = "Needs a newer recorder"
        else:
            for need in b["needs"]:
                if not NEEDS[need](checklist):
                    why = "For people who wear %s" % {"glasses": "glasses or inserts"}.get(need, need)
                    break
        out.append((k, dict(b, done=int(mark.get("rounds") or 0), last=mark.get("last") or "", available=why)))
    out.sort(key=lambda kb: (kb[1]["available"] != "", rank[kb[1]["priority"]], kb[1]["done"], kb[0]))
    return [b for _, b in out]


def mark_done(profile, bounty_id, round_id):
    """Count a round recorded for a bounty in the profile (a dict, changed in place)."""
    marks = profile.setdefault("bounties", {})
    m = marks.get(bounty_id) if isinstance(marks.get(bounty_id), dict) else {}
    m["rounds"] = int(m.get("rounds") or 0) + 1
    m["last"] = round_id
    marks[bounty_id] = m
    return profile
