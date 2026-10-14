"""What the recorders' hub.py share: Hugging Face errors explained for people, the login (whoami
and the browser's device code), upload records kept in a session's or round's session.json, and
the upload itself (a pull request opened first, the export's folder into it, then marked open).
hands/rec/hub.py and gaze/rec/hub.py each add their dataset, their checks and their text.

huggingface_hub is imported only when it's needed (_hf), so the recorders' windows can import
this. Nothing here prints or keeps a token: huggingface_hub saves and reads its own."""
import datetime
import hashlib
import json
import os
import re

REPO_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*/[A-Za-z0-9][A-Za-z0-9._-]*$")
TOKENS_URL = "https://huggingface.co/settings/tokens"
# Quiet huggingface_hub: no progress bars on stderr, no telemetry or update hints.
HF_ENV = {"HF_HUB_DISABLE_PROGRESS_BARS": "1", "HF_HUB_DISABLE_TELEMETRY": "1", "HF_HUB_DISABLE_UPDATE_CHECK": "1"}


class HubError(Exception):
    """What went wrong, for people: kind (below), text, a link to open, validation errors.
    Kinds: missing (no huggingface_hub), login, read-token, terms, not-found, permission,
    network, hub, invalid, duplicate, closed, no-export."""

    def __init__(self, kind, text, link="", errors=None, extra=None):
        super().__init__(text)
        self.kind, self.text, self.link = kind, text, link
        self.errors = errors or []
        self.extra = extra or {}

    def as_dict(self):
        return dict(self.extra, kind=self.kind, text=self.text, link=self.link, errors=self.errors)


def dataset_id(env, default):
    """default, or the environment variable env when it's set (a test repo)."""
    repo = os.environ.get(env, "").strip() or default
    if not REPO_RE.match(repo):
        raise HubError("not-found", f"{env}={repo!r} isn't a dataset id like owner/name")
    return repo


def dataset_url(repo):
    return "https://huggingface.co/datasets/" + repo


def is_draft(path):
    try:
        with open(path, encoding="utf-8") as f:
            return "DRAFT" in f.readline()
    except OSError:
        return False


def now_iso():
    return datetime.datetime.now().astimezone().isoformat(timespec="seconds")


def export_sha(export_dir):
    """The export's identity: the SHA256 of its SHA256SUMS ("" if there's none)."""
    try:
        with open(os.path.join(export_dir, "SHA256SUMS"), "rb") as f:
            return hashlib.sha256(f.read()).hexdigest()
    except OSError:
        return ""


# ---------------------------------------------------------------- records in session.json

def _read(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def uploads(meta_path):
    return [u for u in _read(meta_path).get("uploads") or [] if isinstance(u, dict)]


def previous_upload(meta_path, sha):
    """The latest finished upload of this same export, or None. (Records from before
    2026-10-03 have no status: they were written only when an upload finished.)"""
    same = [u for u in uploads(meta_path) if sha and u.get("export_sha") == sha and u.get("status", "done") == "done"]
    return same[-1] if same else None


def unfinished_upload(meta_path, sha, repo):
    """The latest upload of this same export to repo that opened its pull request and didn't
    finish, or None."""
    same = [u for u in uploads(meta_path) if sha and u.get("export_sha") == sha and u.get("repo") == repo
            and u.get("status") == "started" and u.get("pr_num")]
    return same[-1] if same else None


def record_upload(meta_path, record, write_json):
    """Add record to the file's "uploads". The file keeps its time: an upload doesn't change the
    recordings, so the export mustn't count as out of date because of it (the stores compare
    times)."""
    try:
        st = os.stat(meta_path)
    except OSError:
        st = None
    meta = _read(meta_path)
    records = meta.setdefault("uploads", [])
    same = [i for i, u in enumerate(records) if isinstance(u, dict) and record.get("pr_num")
            and u.get("pr_num") == record["pr_num"] and u.get("repo") == record.get("repo")]
    if same:
        records[same[-1]] = record   # the same pull request: started, then done
    else:
        records.append(record)
    write_json(meta_path, meta)
    if st:
        os.utime(meta_path, ns=(st.st_atime_ns, st.st_mtime_ns))


# ---------------------------------------------------------------- talking to the Hub

def hf_module(missing_hint):
    os.environ.update({k: v for k, v in HF_ENV.items() if k not in os.environ})
    try:
        import huggingface_hub
    except ImportError:
        raise HubError("missing", "huggingface_hub isn't installed: " + missing_hint) from None
    return huggingface_hub


def explain(e, repo):
    """A huggingface_hub (or network) exception as a HubError."""
    if isinstance(e, HubError):
        return e
    try:
        from huggingface_hub import errors as hf
    except ImportError:
        hf = None
    url = dataset_url(repo)
    if hf is not None:
        if isinstance(e, hf.LocalTokenNotFoundError):
            return HubError("login", "You're not logged in to Hugging Face. Press Log in.")
        if isinstance(e, hf.GatedRepoError):
            return HubError("terms", f"Accept the dataset's terms first: open {url}, read them and accept them, "
                                     "then try again.", url)
        if isinstance(e, hf.RepositoryNotFoundError):
            return HubError("not-found", f"The dataset {repo} wasn't found, or it's private and your account has "
                                         "no access to it.", url)
        if isinstance(e, hf.HfHubHTTPError):
            status = getattr(getattr(e, "response", None), "status_code", None)
            first = str(e).strip().splitlines()[0] if str(e).strip() else type(e).__name__
            if status == 401:
                return HubError("login", "Hugging Face didn't accept your login (it may have expired or been "
                                         "revoked). Log in again.")
            if status == 403:
                return HubError("permission", "Your login isn't allowed to open a pull request. Log in again, "
                                              "and allow everything the Hugging Face page asks for.")
            return HubError("hub", f"Hugging Face refused the upload ({status or 'no status'}): {first}")
    try:
        import httpx
        net = (httpx.TransportError, OSError)
    except ImportError:
        net = (OSError,)
    if isinstance(e, net):
        return HubError("network", f"Couldn't reach huggingface.co ({type(e).__name__}: {e}). Check the network "
                                   "and try again: files already sent usually aren't sent twice.")
    return HubError("hub", f"{type(e).__name__}: {e}")


def whoami(hf, repo):
    """{"name", "role"} for the saved login; HubError if there's none or it doesn't work.
    role: "write", "read", "fineGrained" or ""."""
    try:
        info = hf.HfApi().whoami()
    except Exception as e:
        raise explain(e, repo) from None
    token = ((info.get("auth") or {}).get("accessToken") or {})
    return {"name": info.get("name", ""), "role": token.get("role", "")}


def login(repo, progress, whoami_fn, update_hint):
    """The browser login: progress("code", text, url=, code=, expires_in=) once the link is ready,
    then wait (up to the code's expiry: 5 minutes on 2026-10-03) for the person to approve it, and
    save the token. Returns whoami_fn(). Uses huggingface_hub's own device code helpers (1.x)."""
    tell = progress or (lambda phase, text, **extra: None)
    try:
        from huggingface_hub._login import _save_oauth_token
        from huggingface_hub.errors import DeviceCodeError
        from huggingface_hub.utils._oauth_device import poll_device_token, request_device_code
    except ImportError:
        raise HubError("missing", "This huggingface_hub has no browser login: " + update_hint) from None
    try:
        info = request_device_code()
        tell("code", "Approve the login in your browser", url=info["verification_uri_complete"],
             code=info["user_code"], expires_in=info["expires_in"])
        _save_oauth_token(poll_device_token(info))
    except DeviceCodeError as e:
        raise HubError("login", f"The login didn't go through: {e}. Press Log in to try again.") from None
    except Exception as e:
        raise explain(e, repo) from None
    return whoami_fn()


def push(hf, repo, export, path_in_repo, message, description, meta_path, sha, nfiles, nbytes, tell, write_json,
         human_bytes):
    """The upload: the login and access checks, the pull request (opened first, so its link shows
    while the files go; a retry of this export goes on in its own if it's still open), the
    folder, then the pull request marked open. Records it in meta_path's "uploads" as it goes.
    Returns (pr, who, record)."""
    api = hf.HfApi()
    tell("login", "Checking your Hugging Face login", None)
    try:
        who = api.whoami()
    except Exception as e:
        raise explain(e, repo) from None
    role = ((who.get("auth") or {}).get("accessToken") or {}).get("role", "")
    if role == "read":
        raise HubError("read-token", "Your saved login is a read-only token, so it can't open a pull request. "
                                     "Log in again.")
    tell("access", f"Checking access to {repo}", None)
    try:
        api.auth_check(repo, repo_type="dataset")
    except Exception as e:
        raise explain(e, repo) from None
    tell("open", "Opening your pull request", None)
    pr = None
    before = unfinished_upload(meta_path, sha, repo)
    if before:
        try:
            d = api.get_discussion_details(repo, int(before["pr_num"]), repo_type="dataset")
            if d.is_pull_request and d.status in ("draft", "open"):
                pr = d
        except Exception:
            pr = None
    if pr is None:
        try:
            pr = api.create_pull_request(repo, message, description=description, repo_type="dataset")
        except Exception as e:
            raise explain(e, repo) from None
    pr_url = getattr(pr, "url", "") or f"{dataset_url(repo)}/discussions/{pr.num}"
    record = {"repo": repo, "pr_url": pr_url, "pr_num": pr.num, "started": now_iso(), "export_sha": sha,
              "status": "started"}
    record_upload(meta_path, record, write_json)
    tell("opened", f"Pull request #{pr.num} is open. Uploading {nfiles} files ({human_bytes(nbytes)})",
         None, pr_url=pr_url)
    try:
        api.upload_folder(repo_id=repo, repo_type="dataset", folder_path=export, path_in_repo=path_in_repo,
                          revision=f"refs/pr/{pr.num}", commit_message=message, commit_description=description)
    except Exception as e:
        raise explain(e, repo) from None
    try:   # a pull request opened through the API stays a draft until it's marked open
        if api.get_discussion_details(repo, pr.num, repo_type="dataset").status == "draft":
            api.change_discussion_status(repo, pr.num, "open", repo_type="dataset")
    except Exception:
        pass   # the maintainer can open it
    record = dict(record, uploaded=now_iso(), status="done")
    record_upload(meta_path, record, write_json)
    return pr, who, record
