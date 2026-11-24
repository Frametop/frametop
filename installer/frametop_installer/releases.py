"""The two versions the Frametop Installer offers, from GitHub: the newest stable release (from
main) and the newest experimental one, the way get.sh --release picks them. For each it reads
the release's frametop-release.json (about 1 KB, an asset beside Frametop.zip), so it can say
whether the release was tested on this SteamOS build, and which parts it can leave out, before
anything big downloads.

The SteamOS check follows install-release.sh's check_release: Frametop's newest table
(pack/steamos.json on main) when it can be fetched, else the release's own. install-release.sh
still checks again when it installs, and stays the one that refuses a broken build.

It also says when a channel's newest release is newer than a version (newer), for the version
page's line about the installed Frametop (installed_note) and for the installer's own update
(selfupdate.py), which picks its release here too.
"""
import json
import re
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path

from . import options

SLUG = "Frametop/frametop"
API = f"https://api.github.com/repos/{SLUG}"
RAW = f"https://raw.githubusercontent.com/{SLUG}"
TABLE_URL = f"{RAW}/main/pack/steamos.json"
UA = "frametop-installer"
VERSION = re.compile(r"[0-9A-Za-z][0-9A-Za-z.+_-]{0,63}")
COMMIT = re.compile(r"[0-9a-f]{7,40}")
_RELEASED = re.compile(r"([0-9]+)\.([0-9]+)\.([0-9]+)(?:-exp\.([0-9]+))?")


def fetch(url, timeout=20):
    req = urllib.request.Request(url, headers={"User-Agent": UA, "Accept": "application/vnd.github+json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def memo(get=fetch):
    """get, keeping each URL's answer: the look for a newer installer and the lookup of the two
    releases read the same lists, and GitHub counts requests. An error isn't kept."""
    kept = {}

    def cached(url):
        if url not in kept:
            kept[url] = get(url)
        return kept[url]
    return cached


def version_key(version):
    """((major, minor, patch), the experimental build's number or None) of a release's version,
    or None for a version no release has (a local build's)."""
    m = _RELEASED.fullmatch(str(version))
    if not m:
        return None
    return (int(m[1]), int(m[2]), int(m[3])), (int(m[4]) if m[4] else None)


def newer(newest, have):
    """Is newest, the newest release of a channel, newer than version have? Yes, unless they're
    the same, either isn't a release's version, or the numbers say newest is the older one: a
    lower X.Y.Z, or the same X.Y.Z and a lower experimental build. An experimental build and
    the stable release of the same X.Y.Z can't be told apart by number (0.3.0-exp.23 came before
    0.3.0, 0.3.0-exp.24 after it), so there the channel's newest is taken as the newer."""
    a, b = version_key(newest), version_key(have)
    if a is None or b is None or a == b:
        return False
    if a[0] != b[0]:
        return a[0] > b[0]
    if a[1] is not None and b[1] is not None:
        return a[1] > b[1]
    return True


def fetch_json(url, get=fetch):
    return json.loads(get(url))


@dataclass
class Release:
    channel: str             # "stable" or "experimental": what you picked
    version: str
    tag: str
    commit: str
    zip_size: int
    status: str              # tested, untested or broken, on this SteamOS build
    note: str
    supported: tuple = field(default_factory=tuple)
    prerelease: bool = False


def os_release(paths=("/run/host/os-release", "/etc/os-release")):
    """The host's os-release: in a Flatpak, /etc/os-release is the runtime's; /run/host has the host's."""
    for p in paths:
        try:
            text = Path(p).read_text()
        except OSError:
            continue
        out = {}
        for line in text.splitlines():
            k, sep, v = line.partition("=")
            if sep:
                out[k.strip()] = v.strip().strip('"')
        return out
    return {}


def is_frame(osr):
    return osr.get("ID") == "steamos" and osr.get("VARIANT_ID") == "vr"


def _base(v):
    """0.3.0-exp.1 -> (0, 3, 0): the release it leads up to (as install-release.sh's base)."""
    return tuple(int(p) for p in re.findall(r"\d+", str(v).split("-")[0].split("+")[0]))


def steamos_status(release_json, table, build):
    """(status, note) for this release on this SteamOS build, as install-release.sh decides it."""
    version = release_json.get("version", "")
    if not (isinstance(table, dict) and isinstance(table.get("tested"), list)):
        table = release_json.get("steamos") if isinstance(release_json.get("steamos"), dict) else {}

    def entries(kind):
        listed = table.get(kind)
        return [e for e in listed if isinstance(e, dict)] if isinstance(listed, list) else []

    broken = [b for b in entries("broken") if b.get("build") == build
              and (not b.get("from") or _base(version) >= _base(b["from"]))
              and (not b.get("fixed_in") or _base(version) < _base(b["fixed_in"]))]
    if broken:
        note = " ".join(str(broken[0].get("reason", "it doesn't work on this SteamOS build")).split())
        if broken[0].get("fixed_in"):
            note += f" (fixed in Frametop {broken[0]['fixed_in']})"
        return "broken", note
    if any(t.get("build") == build for t in entries("tested")):
        return "tested", ""
    versions = list(dict.fromkeys(str(e.get("version", "?")) for e in entries("tested")))[-3:]
    if not versions:
        return "untested", "not tested on any SteamOS build yet"
    return "untested", "tested on SteamOS " + ", ".join(versions)


def asset(rel, name):
    for a in rel.get("assets", []) or []:
        if isinstance(a, dict) and a.get("name") == name:
            return a
    return None


def pick(channel, get=fetch, having="Frametop.zip"):
    """The GitHub release for a channel, as get.sh picks it: stable is GitHub's latest (the
    newest that isn't a prerelease), experimental the newest of any with a Frametop.zip (or
    with the file named in having)."""
    if channel == "stable":
        rel = fetch_json(f"{API}/releases/latest", get)
        return rel if asset(rel, having) and not rel.get("draft") else None
    for rel in fetch_json(f"{API}/releases?per_page=30", get):  # newest first
        if isinstance(rel, dict) and not rel.get("draft") and asset(rel, having):
            return rel
    return None


def lookup(channel, build, table=None, get=fetch):
    """The Release for a channel, or raises LookupError with what's wrong, in words."""
    try:
        rel = pick(channel, get)
    except (OSError, ValueError) as e:
        raise LookupError(f"couldn't reach GitHub ({e})") from e
    if not rel:
        raise LookupError(f"there's no {channel} release of Frametop on GitHub yet")
    info = asset(rel, "frametop-release.json")
    if not info:
        raise LookupError(f"{rel.get('tag_name', 'the release')} has no frametop-release.json")
    try:
        rj = fetch_json(info["browser_download_url"], get)
    except (OSError, ValueError, KeyError) as e:
        raise LookupError(f"couldn't read {rel.get('tag_name')}'s frametop-release.json ({e})") from e
    version, commit = rj.get("version", ""), rj.get("commit", "")
    if not (isinstance(version, str) and VERSION.fullmatch(version)
            and isinstance(commit, str) and COMMIT.fullmatch(commit)):
        raise LookupError(f"{rel.get('tag_name')}'s frametop-release.json isn't usable")
    if rel.get("tag_name") != f"v{version}":
        raise LookupError(f"{rel.get('tag_name')}'s frametop-release.json names version {version}")
    script = None
    if not isinstance(rj.get("install_options"), list):  # a release from before they were listed
        try:
            script = get(f"{RAW}/{commit}/pack/install-release.sh").decode("utf-8", "replace")
        except OSError:
            script = None
    status, note = steamos_status(rj, table, build)
    return Release(channel=channel, version=version, tag=rel["tag_name"], commit=commit,
                   zip_size=int(asset(rel, "Frametop.zip").get("size") or 0), status=status, note=note,
                   supported=options.supported_options(rj, script), prerelease=bool(rel.get("prerelease")))


def newest_table(get=fetch):
    try:
        table = fetch_json(TABLE_URL, get)
        return table if isinstance(table, dict) else None
    except Exception:  # offline, cut short (http.client.IncompleteRead), or not JSON: no table
        return None


def installed_note(version, channel, found):
    """(the version page's title, its line about the installed Frametop, is it news) for the
    Frametop installed now, against the newest release of its channel in found (lookup_both's)."""
    newest = found.get(channel)
    name = f"Frametop {version}" + (f" ({channel})" if channel else "")
    if isinstance(newest, Release) and newer(newest.version, version):
        return ("Update Frametop", f"Frametop {newest.version} is out. You have {version}. Updating replaces "
                                   "it and keeps your settings.", True)
    if isinstance(newest, Release) and newest.version == version:
        return ("Install Frametop", f"You have {name}, the newest {channel} release. Installing it again, or "
                                    "the other version, keeps your settings.", False)
    return ("Install Frametop", f"Installed now: {name}. Installing replaces it with the version you pick, "
                                "and keeps your settings.", False)


def lookup_both(build, get=fetch):
    """{"stable": Release or LookupError, "experimental": ...}."""
    table = newest_table(get)
    out = {}
    for channel in ("stable", "experimental"):
        try:
            out[channel] = lookup(channel, build, table, get)
        except LookupError as e:
            out[channel] = e
        except Exception as e:  # a GitHub answer shaped some other way: say so, don't crash
            out[channel] = LookupError(f"GitHub's answer wasn't what was expected ({e.__class__.__name__}: {e})")
    return out
