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
  7. marks the record {"repo", "pr_url", "pr_num", "uploaded", "export_sha", "status": "done"}.
--dry-run does all of it except the network calls (4 to 6) and recording (5, 7), and says what
it would upload.

The dataset is HF_DATASET; FT_HANDREC_DATASET overrides it (a test repo, for rehearsals).

usage: hub.py [--base DIR] whoami [--json]
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
from recorder import hub as rhub  # noqa: E402
from recorder.hub import HF_ENV, REPO_RE, TOKENS_URL, HubError, explain, export_sha, is_draft, now_iso  # noqa: E402,F401

# The dataset contributions go to (DESIGN.md "Licensing and consent").
HF_DATASET = "DeeJanuz/frametop-hands"
DATASET_ENV = "FT_HANDREC_DATASET"
ALLOW_ENV = "FT_HANDREC_ALLOW_UPLOAD"
CONSENT_PATH = os.path.join(HERE, "CONSENT.md")
UPLOAD_PATH = os.path.join(HERE, "UPLOAD.md")


def dataset_id():
    """HF_DATASET, or FT_HANDREC_DATASET when set."""
    return rhub.dataset_id(DATASET_ENV, HF_DATASET)


def dataset_url(repo=None):
    return rhub.dataset_url(repo or dataset_id())


def texts_draft():
    return is_draft(CONSENT_PATH) or is_draft(UPLOAD_PATH)


def upload_allowed():
    """Real uploads: once the texts aren't drafts, or for the maintainer's rehearsal against a
    test repo (FT_HANDREC_ALLOW_UPLOAD=1)."""
    return not texts_draft() or os.environ.get(ALLOW_ENV) == "1"


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

def describe(summary, report, sha, nbytes):
    """(commit message, commit description) from validate's summary of the manifest."""
    s = summary
    objects = ", ".join(s.get("objects") or []) or "none"
    if s.get("own_objects"):
        objects += f" (+{s['own_objects']} of their own)"
    message = f"Hands: session {s.get('session')} from {s.get('contributor')}"
    lines = ["Contribution to the Frametop hand dataset, uploaded from the hand recorder.", "",
             f"- Session: {s.get('session')}",
             f"- Contributor: {s.get('contributor')}",
             f"- Takes: {s.get('takes')} ({s.get('minutes')} minutes, {s.get('sets')} frame sets)",
             f"- Lighting: {s.get('lighting') or 'not given'}",
             f"- Objects: {objects}",
             f"- Controllers: {s.get('controllers') or 'not given'}",
             f"- Consent version: {s.get('consent_version')}",
             f"- Tool: {s.get('tool')}",
             f"- Size: {takes.human_bytes(nbytes)}",
             f"- SHA256 of SHA256SUMS: {sha}", "",
             "Checked with hands/rec/validate.py: no errors" + (f", {len(report.warnings)} warnings:"
                                                                 if report.warnings else ".")]
    lines += [f"- {w}" for w in report.warnings]
    return message, "\n".join(lines) + "\n"


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
    repo = dataset_id()
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
    contributor = summary.get("contributor", "")
    sha = export_sha(export)
    before = previous_upload(store, session, sha)
    if before and not again:
        raise HubError("duplicate", f"This export was uploaded already, on {before.get('uploaded', '?')}: "
                                    f"{before.get('pr_url', '')}", before.get("pr_url", ""), extra={"previous": before})
    if not dry_run and not upload_allowed():
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
