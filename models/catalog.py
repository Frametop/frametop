"""The model catalogs: which hand and eye models there are, their notes and benchmarks, and the
versions someone picked, downloaded and checked.

Two catalogs, one per kind, in the repo and on GitHub:
  hands  hands/models.json         ft-hands' palm detector + hand landmark model pairs
  eyes   gaze/tracker/models.json  the eye tracker: SteamVR's, our classic pupil finder, our eye nets

An install reads its branch's catalog from GitHub (main for a stable release, experimental for an
experimental one or a clone on that branch: models.sh's rule), so a new version and its notes reach
installs on a push, without a release. Offline, or when GitHub's copy isn't valid, the copy that came
with the install is used. A catalog only names versions: the files come from the kind's own Hugging
Face repo (HF_REPO here, never from the catalog), at the commit the entry pins, and every file must
match the sha256 it lists.

A catalog (JSON):
  {"catalog": 1, "kind": "hands", "latest": "v13b-p1", "default": "latest", "baseline": "stock",
   "metrics": [{"key", "label", "unit", "better": "higher"|"lower", "note"}],
   "models": [{"name", "title", "kind", "status", "date", "summary", "notes", "cost", "bench": {key: n},
               "format": 1, "rev": "<40 hex>", "files": {"name": "<sha256>"}}]}
  kind    custom (ours, trained on Frametop data), stock (off the shelf, unchanged), valve (SteamVR's),
          classic (our hand-written tracker, no model)
  status  recommended, testing, older, builtin
  latest  the custom version "Latest" follows: it changes as new versions ship
  default what an unset setting uses ("latest", or a builtin's name)
  format  what that version needs of the runtime (FORMATS here); newer ones show, but can't be used
Builtin entries (no rev or files) ship with Frametop or come from SteamVR.

Downloaded versions live in the kind's store (hands/models/versions/NAME, gaze/tracker/models/NAME),
with PIN.json, the entry they were fetched by. hands' "latest" is hands/models/student, which
hands/models.sh keeps on its own pin (models.pin); the catalog's latest must match it (the tests check).

Standard library only: the input settings, the gaze service (on the host) and ft-eyes (in the box)
import it. CLI: models/ft-models.
"""
import fcntl
import hashlib
import json
import os
import re
import shutil
import time
import urllib.error
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
GITHUB = "Frametop/frametop"
CACHE = Path(os.environ.get("XDG_CACHE_HOME") or Path.home() / ".cache") / "frametop" / "models"
CATALOG_VERSION = 1           # the catalog layout this code reads
MAX_CATALOG = 128 * 1024      # bytes
MAX_FILE = 200 * 1024 * 1024  # bytes, per model file
NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}")
SHA = re.compile(r"[0-9a-f]{64}")
REV = re.compile(r"[0-9a-f]{40}")
KINDS_OF = ("custom", "stock", "valve", "classic")
STATUSES = ("recommended", "testing", "older", "builtin")
TEXT_MAX = {"title": 80, "date": 20, "summary": 400, "notes": 4000, "cost": 300}
METRIC_TEXT_MAX = {"key": 40, "label": 120, "unit": 20, "note": 300}

# Per kind: the catalog, the Hugging Face repo files come from, where versions go, the files a
# version may have (and must), files at the repo's top, and the newest version format we can run.
SPECS = {
    "hands": {
        "catalog": "hands/models.json",
        "hf": "DeeJanuz/frametop-hands-models",
        "store": "hands/models/versions",
        "files": {"NOTICE", "hand.ncnn.bin", "hand.ncnn.param", "palm.ncnn.bin", "palm.ncnn.param"},
        "required": {"hand.ncnn.bin", "hand.ncnn.param", "palm.ncnn.bin", "palm.ncnn.param"},
        "top": {"LICENSE", "README.md"},
        "format": 1,
        "setting": "HANDS_MODEL",
        "update": "HANDS_MODELS_UPDATE",
    },
    "eyes": {
        "catalog": "gaze/tracker/models.json",
        "hf": "DeeJanuz/frametop-eyes-models",
        "store": "gaze/tracker/models",
        "files": {"NOTICE", "model.json", "model.ncnn.bin", "model.ncnn.param"},
        "required": {"model.json", "model.ncnn.bin", "model.ncnn.param"},
        "top": {"LICENSE", "README.md"},
        "format": 1,
        "setting": "EYES_MODEL",
        "update": "EYES_MODELS_UPDATE",
    },
}


class CatalogError(ValueError):
    pass


def _check_text(d, limits, where):
    for key, n in limits.items():
        v = d.get(key, "")
        if not isinstance(v, str) or len(v) > n:
            raise CatalogError(f"{where}: {key} must be text of at most {n} characters")


def validate(cat, kind):
    """Check a catalog strictly; return it, or raise CatalogError. Anything unexpected fails the
    whole catalog: GitHub's copy is then ignored for the one that came with the install."""
    spec = SPECS[kind]
    if not isinstance(cat, dict):
        raise CatalogError("not an object")
    if cat.get("catalog") != CATALOG_VERSION:
        raise CatalogError(f"catalog version {cat.get('catalog')!r}, this Frametop reads {CATALOG_VERSION}")
    if cat.get("kind") != kind:
        raise CatalogError(f"a catalog of {cat.get('kind')!r}, not {kind}")
    metrics = cat.get("metrics", [])
    if not isinstance(metrics, list) or len(metrics) > 40:
        raise CatalogError("metrics must be a list (at most 40)")
    keys = set()
    for m in metrics:
        if not isinstance(m, dict):
            raise CatalogError("a metric isn't an object")
        _check_text(m, METRIC_TEXT_MAX, "metric")
        if not m.get("key") or not m.get("label") or m.get("better", "higher") not in ("higher", "lower"):
            raise CatalogError(f"metric {m.get('key')!r}: needs key, label, better higher|lower")
        keys.add(m["key"])
    models = cat.get("models")
    if not isinstance(models, list) or not models or len(models) > 200:
        raise CatalogError("models must be a non-empty list (at most 200)")
    names = set()
    for e in models:
        if not isinstance(e, dict):
            raise CatalogError("a model isn't an object")
        name = e.get("name")
        if not isinstance(name, str) or not NAME.fullmatch(name) or name in ("latest",):
            raise CatalogError(f"bad model name {name!r}")
        if name in names:
            raise CatalogError(f"{name} listed twice")
        names.add(name)
        _check_text(e, TEXT_MAX, name)
        if e.get("kind") not in KINDS_OF or e.get("status") not in STATUSES:
            raise CatalogError(f"{name}: kind must be one of {KINDS_OF}, status one of {STATUSES}")
        bench = e.get("bench", {})
        if not isinstance(bench, dict) or any(k not in keys or not (v is None or isinstance(v, (int, float)))
                                              or isinstance(v, bool) for k, v in bench.items()):
            raise CatalogError(f"{name}: bench must map listed metrics to numbers")
        if e["status"] == "builtin":
            if "rev" in e or "files" in e:
                raise CatalogError(f"{name}: a builtin has no rev or files")
            continue
        if not isinstance(e.get("format"), int) or e["format"] < 1:
            raise CatalogError(f"{name}: format must be a positive number")
        if not isinstance(e.get("rev"), str) or not REV.fullmatch(e["rev"]):
            raise CatalogError(f"{name}: rev must be a full commit hash")
        files = e.get("files")
        if not isinstance(files, dict) or any(not isinstance(v, str) or not SHA.fullmatch(v) for v in files.values()):
            raise CatalogError(f"{name}: files must map names to sha256s")
        allowed = spec["files"] | spec["top"]
        if set(files) - allowed or spec["required"] - set(files):
            raise CatalogError(f"{name}: files must be among {sorted(allowed)} and include {sorted(spec['required'])}")
    for key in ("latest", "baseline"):
        v = cat.get(key)
        if v is not None and v not in names:
            raise CatalogError(f"{key} {v!r} isn't listed")
    latest = cat.get("latest")
    if latest is not None and next(e for e in models if e["name"] == latest)["status"] == "builtin":
        raise CatalogError("latest must be a downloadable version")
    default = cat.get("default", "latest")
    if default != "latest" and default not in names:
        raise CatalogError(f"default {default!r} isn't listed")
    if default == "latest" and latest is None:
        raise CatalogError("default is latest, but there's no latest")
    return cat


# --- where and which ---

def store(kind, tree=REPO):
    return Path(tree) / SPECS[kind]["store"]


def channel(tree=REPO):
    """The branch whose catalog an install follows: main or experimental, or None (any other branch:
    only the copy in the checkout)."""
    tree = Path(tree)
    rel = tree / ".frametop-release"
    if rel.exists():
        ch = ""
        for line in rel.read_text().splitlines():
            if line.startswith("CHANNEL="):
                ch = line.split("=", 1)[1].strip()
        return "experimental" if ch == "experimental" else "main"
    git = tree / ".git"
    try:
        if git.is_file():   # a worktree: "gitdir: PATH"
            git = Path(git.read_text().split(":", 1)[1].strip())
        head = (git / "HEAD").read_text().strip()
    except (OSError, IndexError):
        return None
    branch = head.removeprefix("ref: refs/heads/") if head.startswith("ref: refs/heads/") else None
    return branch if branch in ("main", "experimental") else None


def read_conf(path=None):
    path = Path(path) if path else Path.home() / ".config" / "frametop.conf"
    conf = {}
    try:
        for line in path.read_text().splitlines():
            line = line.split("#", 1)[0].strip()
            if "=" in line:
                k, v = line.split("=", 1)
                conf[k.strip()] = v.strip().strip("\"'")
    except OSError:
        pass
    return conf


def bundled(kind, tree=REPO):
    path = Path(tree) / SPECS[kind]["catalog"]
    return validate(json.loads(path.read_text()), kind)


def cached(kind):
    """GitHub's copy from the last refresh, if it's still valid: (catalog, branch, fetched unix)."""
    try:
        d = json.loads((CACHE / f"{kind}.json").read_text())
        return validate(d["catalog"], kind), d["branch"], float(d["fetched"])
    except (OSError, ValueError, KeyError, TypeError):
        return None


def load(kind, tree=REPO):
    """The catalog to show: GitHub's copy for this install's branch (from the last refresh), else the
    one in the checkout. Returns (catalog, source), source "github BRANCH" or "this install"."""
    ch = channel(tree)
    c = cached(kind)
    if c and ch and c[1] == ch:
        return c[0], f"github {ch}"
    return bundled(kind, tree), "this install"


def refresh(kind, tree=REPO, timeout=15):
    """Fetch the branch's catalog from GitHub into the cache. Returns (ok, message)."""
    ch = channel(tree)
    if not ch:
        return False, "this checkout isn't on main or experimental: only its own catalog"
    url = f"https://raw.githubusercontent.com/{GITHUB}/{ch}/{SPECS[kind]['catalog']}"
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            data = r.read(MAX_CATALOG + 1)
    except (urllib.error.URLError, OSError) as e:
        return False, f"couldn't reach GitHub ({getattr(e, 'reason', e)})"
    if len(data) > MAX_CATALOG:
        return False, "GitHub's catalog is too big; keeping this one"
    try:
        cat = validate(json.loads(data), kind)
    except (ValueError, CatalogError) as e:
        return False, f"GitHub's catalog isn't valid for this Frametop ({e}); keeping this one"
    CACHE.mkdir(parents=True, exist_ok=True)
    tmp = CACHE / f".{kind}.json.tmp"
    tmp.write_text(json.dumps({"branch": ch, "fetched": time.time(), "catalog": cat}))
    os.replace(tmp, CACHE / f"{kind}.json")
    return True, f"up to date with {ch}"


def entry(cat, name):
    return next((e for e in cat["models"] if e["name"] == name), None)


def installed(kind, name, tree=REPO):
    """A downloaded version's folder, if it has every file it needs and its pin, else None."""
    if not NAME.fullmatch(name or ""):
        return None
    d = store(kind, tree) / name
    if (d / "PIN.json").is_file() and all((d / f).is_file() for f in SPECS[kind]["required"]):
        return d
    return None


def installed_names(kind, tree=REPO):
    s = store(kind, tree)
    try:
        return sorted(p.name for p in s.iterdir() if not p.name.startswith(".") and installed(kind, p.name, tree))
    except OSError:
        return []


def usable(kind, e):
    """Can this Frametop run the entry: builtins, and versions whose format it knows."""
    return e["status"] == "builtin" or e["format"] <= SPECS[kind]["format"]


def setting(kind, conf=None):
    """The kind's setting from frametop.conf (FT_ in the environment wins), or the catalog's default."""
    key = SPECS[kind]["setting"]
    v = os.environ.get("FT_" + key)
    if v is None:
        v = (conf if conf is not None else read_conf()).get(key, "")
    return v.strip()


# --- downloading ---

def _sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def verify(kind, name, tree=REPO):
    """Check a downloaded version's files against its pin: [] or the names that don't match."""
    d = installed(kind, name, tree)
    if not d:
        return ["(not downloaded)"]
    pin = json.loads((d / "PIN.json").read_text())
    return [f for f, sha in pin["files"].items() if not (d / f).is_file() or _sha(d / f) != sha]


def fetch(kind, name, tree=REPO, cat=None, progress=None, timeout=30):
    """Download a version into the store, every file checked, then swapped in whole. Returns the
    folder. Raises CatalogError (or OSError) when it can't."""
    spec = SPECS[kind]
    cat = cat or load(kind, tree)[0]
    e = entry(cat, name)
    if e is None:
        raise CatalogError(f"{name} isn't in the catalog")
    if e["status"] == "builtin":
        raise CatalogError(f"{name} comes with Frametop: nothing to download")
    if not usable(kind, e):
        raise CatalogError(f"{name} needs a newer Frametop")
    s = store(kind, tree)
    s.mkdir(parents=True, exist_ok=True)
    with open(s / ".lock", "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        d = s / name
        if installed(kind, name, tree) and json.loads((d / "PIN.json").read_text()) == e and not verify(kind, name, tree):
            return d
        new = s / f".{name}.new"
        new.mkdir(exist_ok=True)
        for f, sha in sorted(e["files"].items()):
            path = new / f
            if path.is_file() and _sha(path) == sha:
                continue
            sub = f if f in spec["top"] else f"{name}/{f}"
            url = f"https://huggingface.co/{spec['hf']}/resolve/{e['rev']}/{sub}"
            if progress:
                progress(f"downloading {f}")
            part = new / (f + ".part")
            try:
                with urllib.request.urlopen(url, timeout=timeout) as r, open(part, "wb") as out:
                    total = 0
                    while block := r.read(1 << 20):
                        total += len(block)
                        if total > MAX_FILE:
                            raise CatalogError(f"{f} is bigger than {MAX_FILE >> 20} MB")
                        out.write(block)
            except (urllib.error.URLError, OSError) as err:
                part.unlink(missing_ok=True)
                raise CatalogError(f"couldn't download {f} ({getattr(err, 'reason', err)})") from err
            except CatalogError:
                part.unlink(missing_ok=True)
                raise
            if _sha(part) != sha:
                part.unlink()
                raise CatalogError(f"{f}'s sha256 doesn't match the catalog; not kept")
            os.replace(part, path)
        (new / "PIN.json").write_text(json.dumps(e, indent=1))
        old = s / f".{name}.old"
        shutil.rmtree(old, ignore_errors=True)
        if d.exists():
            os.replace(d, old)
        os.replace(new, d)
        shutil.rmtree(old, ignore_errors=True)
    return d


def remove(kind, name, tree=REPO):
    d = installed(kind, name, tree)
    if d:
        shutil.rmtree(d)
    return bool(d)


# --- what a setting means ---

def resolve(kind, value=None, tree=REPO, cat=None):
    """What the kind's setting runs now: {"setting", "name", "folder", "why"}. name is the version
    (a catalog name, "latest"'s target), folder its downloaded folder or None (a builtin, or not
    downloaded: then the runtime falls back and "why" says so). A value with a "/" is a folder of
    one's own (for development), used as is."""
    cat = cat or load(kind, tree)[0]
    value = value if value is not None else setting(kind)
    if not value:
        value = cat.get("default", "latest")
    if "/" in value:
        return {"setting": value, "name": None, "folder": os.path.expanduser(value), "why": "a folder of your own"}
    if value.lower() in ("off", "none"):
        value = "classic" if kind == "eyes" else "stock"
    if value == "latest":
        name = cat.get("latest")
        if kind == "hands":   # hands/models.sh keeps latest in hands/models/student
            return {"setting": "latest", "name": name, "folder": None, "why": "latest (hands/models/student)"}
        folder = installed(kind, name, tree) if name else None
        if folder:
            return {"setting": "latest", "name": name, "folder": str(folder), "why": "latest"}
        # Not fetched yet: the newest custom one downloaded, while the auto update fetches it.
        have = [e["name"] for e in cat["models"] if e["kind"] == "custom" and installed(kind, e["name"], tree)]
        if have:
            return {"setting": "latest", "name": have[0], "folder": str(installed(kind, have[0], tree)),
                    "why": f"latest is {name}, not downloaded yet: using {have[0]}"}
        return {"setting": "latest", "name": None, "folder": None, "why": f"latest is {name}, not downloaded yet"}
    e = entry(cat, value)
    if e is not None and e["status"] == "builtin":
        return {"setting": value, "name": value, "folder": None, "why": e.get("title") or value}
    folder = installed(kind, value, tree)
    if folder:
        return {"setting": value, "name": value, "folder": str(folder), "why": value}
    return {"setting": value, "name": None, "folder": None, "why": f"{value} isn't downloaded"}


def auto(kind, tree=REPO, every=15 * 60):
    """What the services run when they start: refresh the catalog (at most every 15 minutes), then
    fetch what the setting needs if it's missing. Not with KIND_MODELS_UPDATE=0. Returns a message."""
    conf = read_conf()
    upd = os.environ.get("FT_" + SPECS[kind]["update"], conf.get(SPECS[kind]["update"], "1"))
    if upd.strip() == "0":
        return f"{kind} models: not updating ({SPECS[kind]['update']}=0)"
    c = cached(kind)
    msg = ""
    if not c or time.time() - c[2] > every:
        msg = refresh(kind, tree)[1]
    cat = load(kind, tree)[0]
    value = setting(kind, conf) or cat.get("default", "latest")
    want = cat.get("latest") if value == "latest" else value
    if kind == "hands" and value == "latest":
        return f"hands models: latest is hands/models.sh's ({msg})" if msg else "hands models: latest is hands/models.sh's"
    e = entry(cat, want) if want and NAME.fullmatch(want) else None
    if e is None or e["status"] == "builtin" or not usable(kind, e):
        return f"{kind} models: nothing to fetch for {value!r}" + (f" ({msg})" if msg else "")
    have = installed(kind, want, tree)
    if have and json.loads((have / "PIN.json").read_text()) == e:
        return f"{kind} models: {want} is here" + (f" ({msg})" if msg else "")
    fetch(kind, want, tree, cat)
    return f"{kind} models: fetched {want}"


def view(kind, tree=REPO):
    """Everything the input settings show for a kind: the catalog with each entry's state."""
    cat, source = load(kind, tree)
    c = cached(kind)
    value = setting(kind) or cat.get("default", "latest")
    have = set(installed_names(kind, tree))
    models = []
    for e in cat["models"]:
        m = {k: e.get(k, "") for k in ("name", "title", "kind", "status", "date", "summary", "notes", "cost")}
        m["bench"] = e.get("bench", {})
        m["builtin"] = e["status"] == "builtin"
        m["installed"] = m["builtin"] or e["name"] in have
        m["usable"] = usable(kind, e)
        m["size_mb"] = None
        models.append(m)
    listed = {e["name"] for e in cat["models"]}
    for name in sorted(have - listed):   # downloaded once, no longer listed: still usable
        models.append({"name": name, "title": name, "kind": "custom", "status": "older", "date": "",
                       "summary": "No longer in the catalog; downloaded here.", "notes": "", "cost": "",
                       "bench": {}, "builtin": False, "installed": True, "usable": True, "size_mb": None})
    return {"kind": kind, "latest": cat.get("latest"), "default": cat.get("default", "latest"),
            "baseline": cat.get("baseline"), "metrics": cat.get("metrics", []), "models": models,
            "setting": value, "resolved": resolve(kind, value, tree, cat), "source": source,
            "checked": c[2] if c else None}
