#!/usr/bin/env python3
"""Upload a hand recorder export to the hand dataset on Hugging Face (DESIGN.md "Upload").

The window runs this as a child process (so Cancel can end it, and huggingface_hub stays out
of the window's process); it also runs from the command line. It uses huggingface_hub (in the
dev container: python3-huggingface-hub, from setup/dev-container.sh) with the login that
`hub.py login` (the window's Log in) or `hf auth login` saved. Nobody types a token anywhere.

`login` is huggingface_hub's browser login (OAuth device code, as `hf auth login` does): it gets a
link and a short code, the person enters the code in their browser and approves, and the token goes
straight from Hugging Face into huggingface_hub's token file. This process saves it; it never
prints it, and the window never sees it.

An upload:
  1. checks the export with validate.py, and stops on errors;
  2. stops if this export (same SHA256SUMS) was uploaded before, unless --again;
  3. stops while CONSENT.md or UPLOAD.md is a draft, unless FT_HANDREC_ALLOW_UPLOAD=1;
  4. checks the login (whoami: a read-only token can't open a pull request) and access to the
     dataset (auth_check: a gated dataset's terms must be accepted first);
  5. opens the pull request first (a draft, empty), so its link can be shown while the files go,
     and records it under "uploads" in session.json with "status": "started";
  6. upload_folder(..., revision="refs/pr/N") to contributions/<contributor>/<session>; a retry of
     the same export goes on in the same pull request while it's still open;
  7. marks the record {"repo", "pr_url", "pr_num", "uploaded", "export_sha", "status": "done"},
     and adds it to BASE/contributions.json (recorder/contributions.py), which outlives the
     session: deleting the local copy to free space keeps it.
--dry-run does all of it except the network calls (4 to 6) and recording (5, 7), and says what
it would upload.

The dataset is HF_DATASET; FT_HANDREC_DATASET overrides it (a test repo, for rehearsals). In the
bounties' test phase (DESIGN.md "Testing") a session recorded under TESTING.md's consent (a bounty
round or a debug session: manifest "consent_kind": "testing", its recordings encrypted) goes to
HF_TEST_DATASET instead (FT_HANDREC_TEST_DATASET overrides it), and its gate is TESTING.md's.

`board` fetches the bounty board (bounties.json) from the dataset into BASE/board-cache.json, and the
round scripts it names that the recorder doesn't ship (bounties/<script>.json, checked) into BASE/board-rounds/,
with the saved login when there is one (the dataset is gated): the window's board then uses it
when it's newer than the one shipped here (recorder/bounties.py checks what it may say).

usage: hub.py [--base DIR] whoami [--json]
       hub.py [--base DIR] board [--json]
       hub.py [--base DIR] login [--json]
       hub.py [--base DIR] upload SESSION [--dry-run] [--again] [--json]
With --json, each line of output is one JSON object: {"phase", "text", "fraction"} as it goes,
with {"phase": "opened", "pr_url"} once the pull request exists, then {"done": {...}} or
{"error": {"kind", "text", "link", "errors"}}. login says {"phase": "code", "url", "code",
"expires_in"} once it has the link, then {"done": {"name", "role"}}. Exit status 0: done.
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import takes  # noqa: E402  (next to this file)
sys.path.insert(0, os.path.dirname(os.path.dirname(HERE)))
from recorder import bounties  # noqa: E402
from recorder import contributions  # noqa: E402
from recorder import hub as rhub  # noqa: E402
from recorder.hub import HF_ENV, REPO_RE, TOKENS_URL, HubError, explain, export_sha, is_draft, now_iso  # noqa: E402,F401

# The datasets contributions go to (DESIGN.md "Licensing and consent", "Testing").
HF_DATASET = "DeeJanuz/frametop-hands"
HF_TEST_DATASET = "DeeJanuz/frametop-hands-test"   # gated; everything in it encrypted to the maintainer
DATASET_ENV = "FT_HANDREC_DATASET"
TEST_DATASET_ENV = "FT_HANDREC_TEST_DATASET"
ALLOW_ENV = "FT_HANDREC_ALLOW_UPLOAD"
CONSENT_PATH = os.path.join(HERE, "CONSENT.md")
TESTING_PATH = os.path.join(HERE, "TESTING.md")
UPLOAD_PATH = os.path.join(HERE, "UPLOAD.md")
KINDS = ("public", "testing")


def dataset_id(kind="public"):
    """HF_DATASET (FT_HANDREC_DATASET when set), or for "testing" HF_TEST_DATASET
    (FT_HANDREC_TEST_DATASET when set)."""
    if kind == "testing":
        return rhub.dataset_id(TEST_DATASET_ENV, HF_TEST_DATASET)
    return rhub.dataset_id(DATASET_ENV, HF_DATASET)


def dataset_url(repo=None):
    return rhub.dataset_url(repo or dataset_id())


def texts_draft(kind="public"):
    return is_draft(TESTING_PATH if kind == "testing" else CONSENT_PATH) or is_draft(UPLOAD_PATH)


def upload_allowed(kind="public"):
    """Real uploads: once the kind's texts aren't drafts, or for the maintainer's rehearsal
    against a test repo (FT_HANDREC_ALLOW_UPLOAD=1)."""
    return not texts_draft(kind) or os.environ.get(ALLOW_ENV) == "1"


def session_kind(store, session):
    """The consent a session was recorded under (takes.consent_kind of its session.json)."""
    return takes.consent_kind(takes.read_json(meta_path(store, session)))


# ---------------------------------------------------------------- records in session.json

def meta_path(store, session):
    return os.path.join(store.session_dir(session), "session.json")


def uploads(store, session):
    return rhub.uploads(meta_path(store, session))


def previous_upload(store, session, sha):
    return rhub.previous_upload(meta_path(store, session), sha)


def unfinished_upload(store, session, sha, repo):
    return rhub.unfinished_upload(meta_path(store, session), sha, repo)


def record_upload(store, session, record):
    rhub.record_upload(meta_path(store, session), record, takes.write_json)


# ---------------------------------------------------------------- the pull request's text

def consent_line(s):
    """The consent the upload goes under, and the one it was recorded under when that's older."""
    now, then = s.get("consent_version") or "", s.get("recorded_consent_version") or ""
    return now + (f" (recorded under {then})" if then and then != now else "")


def describe(summary, report, sha, nbytes):
    """(commit message, commit description) from validate's summary of the manifest."""
    s = summary
    if s.get("testing"):
        what = "debug session" if s.get("debug") else f"{s.get('bounty') or 'test'} round"
        lines = [f"Test upload ({what}) for the Frametop hand tracking, from the hand recorder. Its recordings are "
                 "encrypted to the maintainer's keys; for testing only.", "",
                 f"- Session: {s.get('session')}",
                 f"- Kind: {'debug session (Frametop in use, the live tracker recorded)' if s.get('debug') else 'bounty round'}",
                 f"- Bounty: {s.get('bounty') or 'none'}",
                 f"- Contributor: {s.get('contributor')}",
                 f"- Takes: {s.get('takes')} ({s.get('minutes')} minutes, {s.get('sets')} frame sets"
                 f"{', %g a second' % s['record_hz'] if s.get('record_hz') else ''})",
                 f"- Lighting: {s.get('lighting') or 'not given'}",
                 f"- Consent: testing, version {consent_line(s)}",
                 f"- Tool: {s.get('tool')}",
                 f"- Size: {takes.human_bytes(nbytes)}",
                 f"- SHA256 of SHA256SUMS: {sha}", "",
                 "Checked with hands/rec/validate.py: no errors" + (f", {len(report.warnings)} warnings:"
                                                                     if report.warnings else ".")]
        lines += [f"- {w}" for w in report.warnings]
        return f"Hands test: {what} {s.get('session')} from {s.get('contributor')}", "\n".join(lines) + "\n"
    objects = ", ".join(s.get("objects") or []) or "none"
    if s.get("own_objects"):
        objects += f" (+{s['own_objects']} of their own)"
    bounty = f"{s['bounty']} " if s.get("bounty") else ""
    message = f"Hands: {bounty}session {s.get('session')} from {s.get('contributor')}"
    lines = ["Contribution to the Frametop hand dataset, uploaded from the hand recorder.", "",
             f"- Session: {s.get('session')}",
             f"- Bounty: {s.get('bounty') or 'none (a full session or a quick round)'}",
             f"- Contributor: {s.get('contributor')}",
             f"- Takes: {s.get('takes')} ({s.get('minutes')} minutes, {s.get('sets')} frame sets"
             f"{', %g a second' % s['record_hz'] if s.get('record_hz') else ''})",
             f"- Lighting: {s.get('lighting') or 'not given'}",
             f"- Objects: {objects}",
             f"- Controllers: {s.get('controllers') or 'not given'}",
             f"- Consent version: {consent_line(s)}",
             f"- Tool: {s.get('tool')}",
             f"- Size: {takes.human_bytes(nbytes)}",
             f"- SHA256 of SHA256SUMS: {sha}", "",
             "Checked with hands/rec/validate.py: no errors" + (f", {len(report.warnings)} warnings:"
                                                                 if report.warnings else ".")]
    lines += [f"- {w}" for w in report.warnings]
    return message, "\n".join(lines) + "\n"


# ---------------------------------------------------------------- the bounty board

def board_url(repo):
    return f"{rhub.dataset_url(repo)}/resolve/main/bounties.json"


def round_url(repo, script):
    return f"{rhub.dataset_url(repo)}/resolve/main/bounties/{script}.json"


def fetch_rounds(board, base, repo, token, fetch=bounties.fetch_json):
    """The round scripts the board names that this recorder doesn't ship, from the dataset's
    bounties/<script>.json into base/board-rounds/ (session.bounty_script finds them), each kept
    only if session.check_round passes it; the ones no longer wanted are deleted. Returns
    {"rounds": [names kept], "rounds_failed": {name: why}}."""
    import session   # noqa: E402  (here: only the board needs it)
    out_dir = session.fetched_rounds_dir(base)
    wanted = sorted({b["script"] for b in board["bounties"] if b["priority"] != "retired"
                     and not os.path.exists(session.bounty_script(b["script"], session.BOUNTIES_DIR))})
    kept, failed = [], {}
    for name in wanted:
        path = os.path.join(out_dir, name + ".json")
        try:
            script = session.check_round(name, fetch(round_url(repo, name), token, session.ROUND_MAX_BYTES))
        except (OSError, ValueError) as e:
            failed[name] = str(e) or type(e).__name__
            continue   # an older copy that passed when it came stays
        bounties.keep_json(path, script)
        kept.append(name)
    try:
        for n in os.listdir(out_dir):
            if n.endswith(".json") and n[:-5] not in wanted:
                os.remove(os.path.join(out_dir, n))
    except OSError:
        pass
    return {"rounds": kept, "rounds_failed": failed}


def fetch_board(base):
    """The dataset's bounties.json into base/board-cache.json (session.load_board reads it), and the
    round scripts it names that the recorder lacks (fetch_rounds). Returns {"updated", "bounties",
    "rounds", "rounds_failed"}; raises HubError."""
    repo = dataset_id("public")   # the board is the public dataset's
    token = None
    try:
        token = _hf().get_token()
    except (HubError, AttributeError):
        pass   # no huggingface_hub, or an old one: try without a login
    try:
        board = bounties.fetch(board_url(repo), os.path.join(base, "board-cache.json"), token)
    except ValueError as e:
        raise HubError("hub", f"The dataset's bounty board isn't valid: {e}") from None
    except OSError as e:
        status = getattr(e, "code", None)
        if status in (401, 403):
            raise HubError("terms", "Log in and accept the dataset's terms to get its newest bounty board.",
                           rhub.dataset_url(repo)) from None
        if status == 404:
            raise HubError("not-found", f"{repo} has no bounty board yet.") from None
        raise HubError("network", f"Couldn't fetch the bounty board ({type(e).__name__}: {e}).") from None
    return dict({"updated": board["updated"], "bounties": len(board["bounties"])},
                **fetch_rounds(board, base, repo, token))


# ---------------------------------------------------------------- talking to the Hub

def _hf():
    return rhub.hf_module("run setup/dev-container.sh (or: pip install --user huggingface_hub)")


def whoami():
    """{"name", "role"} for the saved login; HubError if there's none or it doesn't work."""
    return rhub.whoami(_hf(), dataset_id())


def login(progress=None):
    """The browser login (recorder/hub.py's login). Returns whoami()."""
    _hf()
    return rhub.login(dataset_id(), progress, lambda: whoami(), "update the dev container (setup/dev-container.sh).")


def upload(store, session, dry_run=False, again=False, progress=None, log=None):
    """Upload exports/<session> (the steps in this file's docstring). progress(phase, text,
    fraction or None); log(text) for the dry run's account. Returns the result; raises HubError."""
    tell = progress or (lambda phase, text, fraction=None, **extra: None)
    say = log or (lambda text: None)
    import validate
    try:
        export = store.export_dir(session)
    except ValueError as e:
        raise HubError("no-export", str(e)) from None
    if not os.path.isfile(os.path.join(export, "manifest.json")):
        raise HubError("no-export", f"No export of session {session}: export it first")

    tell("check", "Checking the export", 0.0)
    report = validate.validate(export, progress=lambda f, text: tell("check", text, f))
    if not report.ok:
        raise HubError("invalid", f"The export has {len(report.errors)} problems, so it can't be uploaded. Export "
                                  "the session again; if that doesn't help, report it.", errors=report.errors)
    summary = report.summary
    kind = "testing" if summary.get("testing") else "public"
    repo = dataset_id(kind)
    contributor = summary.get("contributor", "")
    sha = export_sha(export)
    before = previous_upload(store, session, sha)
    if before and not again:
        raise HubError("duplicate", f"This export was uploaded already, on {before.get('uploaded', '?')}: "
                                    f"{before.get('pr_url', '')}", before.get("pr_url", ""), extra={"previous": before})
    if not dry_run and not upload_allowed(kind):
        if kind == "testing":
            raise HubError("closed", "Test uploads aren't open yet: the testing text is a draft waiting for the "
                                     f"maintainer. (For a rehearsal against a test repo: {ALLOW_ENV}=1.)")
        raise HubError("closed", "Contributions aren't open yet: the texts are drafts waiting for a legal review. "
                                 f"(For a rehearsal against a test repo: {ALLOW_ENV}=1.)")
    path_in_repo = f"contributions/{contributor}/{session}"
    message, description = describe(summary, report, sha, report.bytes)
    files = []
    for root, _, names in os.walk(export):
        files += [os.path.relpath(os.path.join(root, n), export) for n in names]
    plan = {"repo": repo, "repo_type": "dataset", "folder_path": export, "path_in_repo": path_in_repo,
            "create_pr": True, "commit_message": message, "commit_description": description,
            "files": len(files), "bytes": report.bytes, "warnings": report.warnings}

    if dry_run:
        say(f"dry run: would check the login (whoami) and access to {repo} (auth_check)")
        say(f"dry run: would upload_folder {len(files)} files, {takes.human_bytes(report.bytes)}, "
            f"from {export} to datasets/{repo}/{path_in_repo}, as a pull request")
        for rel in sorted(files):
            say(f"  {rel}  {takes.human_bytes(os.path.getsize(os.path.join(export, rel)))}")
        say(f"dry run: commit message: {message}")
        say("dry run: commit description:\n" + description.rstrip())
        record = {"repo": repo, "pr_url": "", "uploaded": now_iso(), "export_sha": sha, "status": "done"}
        say(f"dry run: would record in session.json's uploads: {json.dumps(record)}")
        tell("done", "Dry run: nothing was uploaded", 1.0)
        return dict(plan, dry_run=True, pr_url="", record=record)

    pr, who, record = rhub.push(_hf(), repo, export, path_in_repo, message, description, meta_path(store, session), sha,
                                len(files), report.bytes, tell, takes.write_json, takes.human_bytes)
    try:   # the log is for the contributor's own record: a failure to write it doesn't undo the upload
        contributions.add(store.base, [contributions.entry(session, takes.read_json(meta_path(store, session)), record,
                                                           report.bytes, summary.get("minutes"))])
    except OSError as e:
        say(f"couldn't add the upload to {contributions.FILE}: {e}")
    tell("done", "Uploaded", 1.0)
    return dict(plan, dry_run=False, pr_url=record["pr_url"], pr_num=pr.num, user=who.get("name", ""), record=record)


# ---------------------------------------------------------------- the command line

def main():
    ap = argparse.ArgumentParser(description="Upload a hand recorder export to the hand dataset on Hugging Face.")
    ap.add_argument("--base", default=takes.DEFAULT_BASE)
    sub = ap.add_subparsers(dest="cmd", required=True)
    w = sub.add_parser("whoami", help="show the saved Hugging Face login")
    w.add_argument("--json", action="store_true")
    li = sub.add_parser("login", help="log in to Hugging Face in the browser")
    li.add_argument("--json", action="store_true")
    b = sub.add_parser("board", help="fetch the dataset's bounty board")
    b.add_argument("--json", action="store_true")
    u = sub.add_parser("upload", help="upload exports/SESSION as a pull request")
    u.add_argument("session")
    u.add_argument("--dry-run", action="store_true", help="no network: say what would be uploaded")
    u.add_argument("--again", action="store_true", help="upload even if this export was uploaded before")
    u.add_argument("--json", action="store_true", help="JSON lines, for the window")
    a = ap.parse_args()

    def emit(obj):
        print(json.dumps(obj), flush=True)

    try:
        if a.cmd == "whoami":
            who = whoami()
            if a.json:
                emit({"done": who})
            else:
                print(f"logged in as {who['name']} (token role: {who['role'] or 'unknown'})")
            return 0
        if a.cmd == "board":
            got = fetch_board(os.path.abspath(os.path.expanduser(a.base)))
            if a.json:
                emit({"done": got})
            else:
                print(f"bounty board of {got['updated']}: {got['bounties']} bounties")
            return 0
        if a.cmd == "login":
            def code(phase, text, **extra):
                if a.json:
                    emit(dict(extra, phase=phase, text=text))
                else:
                    print(f"Open {extra['url']} and approve the code {extra['code']}. Waiting...", flush=True)
            who = login(code)
            if a.json:
                emit({"done": who})
            else:
                print(f"logged in as {who['name']}")
            return 0
        if a.json:
            def progress(phase, text, fraction=None, **extra):
                emit(dict(extra, phase=phase, text=text, fraction=fraction))

            def log(text):
                emit({"log": text})
        else:
            last = {}

            def progress(phase, text, fraction=None, **extra):
                if extra.get("pr_url"):
                    print(f"pull request: {extra['pr_url']}", flush=True)
                line = text if fraction is None else f"{fraction * 100:5.1f}% {text}"
                if (phase, text) != last.get("key") or fraction in (0.0, 1.0):
                    print(line, file=sys.stderr, flush=True)
                last["key"] = (phase, text)

            def log(text):
                print(text, flush=True)
        if hasattr(os, "setpriority"):
            try:
                os.setpriority(os.PRIO_PROCESS, 0, 19)   # validation reads and hashes everything: stay out of VR's way
            except OSError:
                pass
        result = upload(takes.Store(a.base), a.session, dry_run=a.dry_run, again=a.again, progress=progress, log=log)
        if a.json:
            emit({"done": result})
        elif result["dry_run"]:
            print(f"dry run done: {result['files']} files would go to datasets/{result['repo']}/{result['path_in_repo']}")
        else:
            print(f"uploaded: {result['pr_url']}")
        return 0
    except HubError as e:
        if a.json:
            emit({"error": e.as_dict()})
        else:
            print(f"error ({e.kind}): {e.text}", file=sys.stderr)
            for line in e.errors:
                print(f"  {line}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
