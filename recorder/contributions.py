"""The contributions log: what a contributor has uploaded, kept after the recordings are deleted.

A session's uploads are recorded in its own session.json (recorder/hub.py record_upload), so
deleting the session to free space would lose them. This log keeps one entry per finished upload
in BASE/contributions.json, next to profile.json:

  {"schema": 1, "contributions": [
     {"session", "repo", "pr_url", "pr_num", "uploaded", "export_sha", "export_bytes",
      "bounty": {"id", "title"} | {}, "kind": "public" | "testing", "debug": bool,
      "started", "minutes", "local_deleted": "<time>" | ""}]}

An entry is keyed by (repo, pr_num), or (repo, session, export_sha) for one without a pull request
number. hub.py adds the entry when an upload finishes; the window backfills uploads that older
recorders recorded only in session.json, and marks an entry when its local copy is deleted.
Nothing here leaves the Frame.

Standard library only."""
import datetime
import json
import os

SCHEMA = 1
FILE = "contributions.json"


def path(base):
    return os.path.join(base, FILE)


def now_iso():
    return datetime.datetime.now().astimezone().isoformat(timespec="seconds")


def load(base):
    """The log's entries, oldest first ([] when there's none or it can't be read)."""
    try:
        with open(path(base), encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, ValueError):
        return []
    items = data.get("contributions") if isinstance(data, dict) else None
    return [c for c in items if isinstance(c, dict) and c.get("session")] if isinstance(items, list) else []


def _save(base, items):
    os.makedirs(base, mode=0o700, exist_ok=True)
    p = path(base)
    tmp = p + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump({"schema": SCHEMA, "contributions": items}, f, indent=2)
        f.write("\n")
    os.replace(tmp, p)


def _key(c):
    if c.get("pr_num") not in (None, ""):
        return (c.get("repo", ""), "pr", str(c["pr_num"]))
    return (c.get("repo", ""), c.get("session", ""), c.get("export_sha", ""))


def entry(session, meta, upload, export_bytes=0, minutes=None):
    """A log entry for a finished upload: session id, its session.json (meta), the upload's
    record (session.json "uploads", status done), and what the export held when known."""
    bounty = meta.get("bounty") if isinstance(meta.get("bounty"), dict) else {}
    consent = meta.get("consent") if isinstance(meta.get("consent"), dict) else {}
    debug = bool(meta.get("debug"))
    return {"session": session, "repo": upload.get("repo", ""), "pr_url": upload.get("pr_url", ""),
            "pr_num": upload.get("pr_num"), "uploaded": upload.get("uploaded", ""),
            "export_sha": upload.get("export_sha", ""), "export_bytes": int(export_bytes or 0),
            "bounty": {"id": bounty.get("id", ""), "title": bounty.get("title", "")} if bounty else {},
            "kind": "testing" if consent.get("kind") == "testing" or debug else "public", "debug": debug,
            "started": meta.get("started", ""), "minutes": minutes, "local_deleted": ""}


def done_uploads(meta):
    """session.json's finished uploads (records from before 2026-10-03 have no status: done)."""
    return [u for u in meta.get("uploads") or [] if isinstance(u, dict)
            and u.get("status", "done") == "done" and u.get("pr_url")]


def add(base, items):
    """Add entries (a list) the log doesn't have yet, and fill in what an existing entry lacks.
    Returns how many were new."""
    log = load(base)
    known = {_key(c): c for c in log}
    new = 0
    for c in items:
        old = known.get(_key(c))
        if old is None:
            log.append(dict(c))
            known[_key(c)] = log[-1]
            new += 1
        else:
            for k, v in c.items():
                if k != "local_deleted" and v not in (None, "", {}, 0) and old.get(k) in (None, "", {}, 0):
                    old[k] = v
    if items:
        _save(base, log)
    return new


def mark_deleted(base, session, when=None):
    """Note that a session's local copy was deleted (its entries stay)."""
    log = load(base)
    when = when or now_iso()
    hit = False
    for c in log:
        if c.get("session") == session and not c.get("local_deleted"):
            c["local_deleted"] = when
            hit = True
    if hit:
        _save(base, log)
    return hit


def sessions(base):
    """The session ids the log has a finished upload of."""
    return {c["session"] for c in load(base)}
