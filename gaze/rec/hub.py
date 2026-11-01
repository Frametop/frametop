#!/usr/bin/env python3
"""Upload an eye recorder export to the eye dataset on Hugging Face.

As the hand recorder's hub.py (the shared part is recorder/hub.py): the window runs it as a child
process, it uses huggingface_hub's saved login (`hub.py login` is the browser's device code), and
nobody types a token anywhere. An upload:
  1. checks the export with validate.py (against recipients.txt's keys), and stops on errors;
  2. stops if this export (same SHA256SUMS) was uploaded before, unless --again;
  3. stops unless CONSENT.md and UPLOAD.md are here and neither is a draft. FT_EYEREC_ALLOW_UPLOAD=1
     opens it for a rehearsal, and only to another dataset than the real one (FT_EYEREC_DATASET):
     the real one takes nothing before the texts are reviewed; and it stops for an export made under
     a draft consent text (agree to the current one, then export again);
  4. checks the login and access to the dataset, opens the pull request first, uploads the folder
     to contributions/<contributor>/<round> in it, and records it under "uploads" in session.json.
--dry-run does all of it except the network calls and the records, and says what it would upload.

The images and health answers are encrypted already (takes.py's export): nothing here can read
them, and the dataset's readers need one of its keys.

`hub.py board` fetches the bounty board (bounties.json) from the dataset into BASE/board-cache.json,
with the saved login when there is one (the dataset is gated): the window's board then uses it
when it's newer than the one shipped here (recorder/bounties.py checks what it may say).

usage: hub.py [--base DIR] whoami [--json]
       hub.py [--base DIR] login [--json]
       hub.py [--base DIR] board [--json]
       hub.py [--base DIR] upload ROUND [--dry-run] [--again] [--json]
With --json, each line of output is one JSON object, as the hand recorder's hub.py.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
from gaze.rec import takes  # noqa: E402  (next to this file)
import recorder.files  # noqa: E402  (Cancelled, read_json, human_bytes, free_bytes)
from recorder import bounties  # noqa: E402
from recorder import hub as rhub  # noqa: E402
from recorder.common import write_json  # noqa: E402
from recorder.hub import HubError, export_sha, is_draft, now_iso  # noqa: E402,F401

HF_DATASET = "DeeJanuz/frametop-eyes"
DATASET_ENV = "FT_EYEREC_DATASET"
ALLOW_ENV = "FT_EYEREC_ALLOW_UPLOAD"
CONSENT_PATH = os.path.join(HERE, "CONSENT.md")
UPLOAD_PATH = os.path.join(HERE, "UPLOAD.md")


def dataset_id():
    """HF_DATASET, or FT_EYEREC_DATASET when set."""
    return rhub.dataset_id(DATASET_ENV, HF_DATASET)


def texts_ready():
    return all(os.path.isfile(p) and not is_draft(p) for p in (CONSENT_PATH, UPLOAD_PATH))


def upload_allowed(repo):
    """Real uploads: once the texts are reviewed, or for a rehearsal against another dataset."""
    return texts_ready() or (os.environ.get(ALLOW_ENV) == "1" and repo != HF_DATASET)


def meta_path(store, rnd):
    return os.path.join(store.round_dir(rnd), "session.json")


def describe(summary, report, sha, nbytes):
    """(commit message, commit description) from validate's summary of the manifest."""
    s = summary
    message = f"Eyes: {s.get('bounty')} round {s.get('round')} from {s.get('contributor')}"
    lines = ["Contribution to the Frametop eye dataset, uploaded from the eye recorder. The eye images "
             "and any health answers are encrypted to the dataset's keys.", "",
             f"- Round: {s.get('round')}",
             f"- Bounty: {s.get('bounty')}",
             f"- Contributor: {s.get('contributor')}",
             f"- Takes: {s.get('takes')} ({s.get('minutes')} minutes, {s.get('frames')} frames)",
             f"- Glasses: {s.get('glasses') or 'not given'}",
             f"- Health answers: {'included, encrypted' if s.get('health') else 'none'}",
             f"- Consent version: {s.get('consent_version')}",
             f"- Tool: {s.get('tool')}",
             f"- Size: {recorder.files.human_bytes(nbytes)}",
             f"- SHA256 of SHA256SUMS: {sha}", "",
             "Checked with gaze/rec/validate.py: no errors" + (f", {len(report.warnings)} warnings:"
                                                                if report.warnings else ".")]
    lines += [f"- {w}" for w in report.warnings]
    return message, "\n".join(lines) + "\n"


def _hf():
    return rhub.hf_module("run setup/dev-container.sh (or: pip install --user huggingface_hub)")


def whoami():
    return rhub.whoami(_hf(), dataset_id())


def login(progress=None):
    _hf()
    return rhub.login(dataset_id(), progress, lambda: whoami(), "update the dev container (setup/dev-container.sh).")


def board_url(repo):
    return f"{rhub.dataset_url(repo)}/resolve/main/bounties.json"


def fetch_board(base):
    """The dataset's bounties.json into base/board-cache.json (session.load_board reads it).
    Returns {"updated", "bounties"}; raises HubError."""
    repo = dataset_id()
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
    return {"updated": board["updated"], "bounties": len(board["bounties"])}


def upload(store, rnd, dry_run=False, again=False, progress=None, log=None):
    """Upload exports/<round> (the steps in this file's docstring). Returns the result; raises HubError."""
    tell = progress or (lambda phase, text, fraction=None, **extra: None)
    say = log or (lambda text: None)
    from gaze.rec import validate
    repo = dataset_id()
    try:
        export = store.export_dir(rnd)
    except ValueError as e:
        raise HubError("no-export", str(e)) from None
    if not os.path.isfile(os.path.join(export, "manifest.json")):
        raise HubError("no-export", f"No export of round {rnd}: export it first")

    tell("check", "Checking the export", 0.0)
    report = validate.validate(export, progress=lambda f, text: tell("check", text, f))
    if not report.ok:
        raise HubError("invalid", f"The export has {len(report.errors)} problems, so it can't be uploaded. Export "
                                  "the round again; if that doesn't help, report it.", errors=report.errors)
    summary = report.summary
    sha = export_sha(export)
    before = rhub.previous_upload(meta_path(store, rnd), sha)
    if before and not again:
        raise HubError("duplicate", f"This export was uploaded already, on {before.get('uploaded', '?')}: "
                                    f"{before.get('pr_url', '')}", before.get("pr_url", ""), extra={"previous": before})
    if not dry_run and not upload_allowed(repo):
        raise HubError("closed", "Eye contributions aren't open yet: the consent and upload texts are waiting for a "
                                 f"legal review. (A rehearsal: {ALLOW_ENV}=1 with {DATASET_ENV} set to a test dataset.)")
    if repo == HF_DATASET and str(summary.get("consent_version") or "").endswith("draft"):
        raise HubError("draft-consent", "This export was made when you'd agreed to a draft of the consent text. "
                                        "Agree to the current one on the Welcome page, then export the round again.")
    path_in_repo = f"contributions/{summary.get('contributor', '')}/{rnd}"
    message, description = describe(summary, report, sha, report.bytes)
    files = []
    for root, _, names in os.walk(export):
        files += [os.path.relpath(os.path.join(root, n), export) for n in names]
    plan = {"repo": repo, "repo_type": "dataset", "folder_path": export, "path_in_repo": path_in_repo,
            "create_pr": True, "commit_message": message, "commit_description": description,
            "files": len(files), "bytes": report.bytes, "warnings": report.warnings}

    if dry_run:
        say(f"dry run: would check the login (whoami) and access to {repo} (auth_check)")
        say(f"dry run: would upload_folder {len(files)} files, {recorder.files.human_bytes(report.bytes)}, "
            f"from {export} to datasets/{repo}/{path_in_repo}, as a pull request")
        for rel in sorted(files):
            say(f"  {rel}  {recorder.files.human_bytes(os.path.getsize(os.path.join(export, rel)))}")
        say(f"dry run: commit message: {message}")
        say("dry run: commit description:\n" + description.rstrip())
        record = {"repo": repo, "pr_url": "", "uploaded": now_iso(), "export_sha": sha, "status": "done"}
        say(f"dry run: would record in session.json's uploads: {json.dumps(record)}")
        tell("done", "Dry run: nothing was uploaded", 1.0)
        return dict(plan, dry_run=True, pr_url="", record=record)

    pr, who, record = rhub.push(_hf(), repo, export, path_in_repo, message, description, meta_path(store, rnd), sha,
                                len(files), report.bytes, tell, write_json, recorder.files.human_bytes)
    tell("done", "Uploaded", 1.0)
    return dict(plan, dry_run=False, pr_url=record["pr_url"], pr_num=pr.num, user=who.get("name", ""), record=record)


def main():
    return rhub.cli("Upload an eye recorder export to the eye dataset on Hugging Face.", "round", takes.DEFAULT_BASE, takes.Store,
                    whoami=whoami, login=login, fetch_board=fetch_board, upload=upload)


if __name__ == "__main__":
    os.umask(0o077)   # recordings, exports and logs are private: folders 0700, files 0600
    sys.exit(main())
