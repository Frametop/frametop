"""A newer Frametop Installer: looked for on GitHub when the window opens, and installed over
this one.

The installer ships as a Flatpak bundle on each release (FrametopInstaller.flatpak). A bundle
has no repository behind it, so neither flatpak update nor Discover ever finds a newer one. So
the installer looks itself: it follows the channel it came from (an experimental release's
follows the newest release of any kind, a stable release's follows GitHub's latest), and a
release whose version is newer than its own (VERSION, written by flatpak/build.sh) is an update.

Updating downloads that release's bundle into ~/.cache/frametop-installer/update, checks it
against the release's SHA256SUMS, and has the host's flatpak install it where this one is
installed. That's the trust an install of Frametop already has: the same releases, over HTTPS.
For every user of the headset (Discover's choice), flatpak asks for the password, as it did
when the installer was installed; for this user alone it doesn't. The running window keeps the
old files until it closes, so it opens the new version and quits.
"""
import hashlib
import os
import subprocess
import tempfile
import urllib.request
from dataclasses import dataclass
from pathlib import Path

from . import host, releases

APP_ID = "io.github.frametop.Installer"
BUNDLE = "FrametopInstaller.flatpak"
SUMS = "SHA256SUMS"
DOWNLOADS = f"https://github.com/{releases.SLUG}/releases/download/"
FOLDER = host.CACHE / "update"
MAX_BUNDLE = 50_000_000     # the bundle is about 30 kB: anything near this isn't it
MAX_SUMS = 100_000


class UpdateError(Exception):
    """Why the update didn't happen, in words for the window."""


@dataclass(frozen=True)
class Update:
    version: str
    url: str         # the release's FrametopInstaller.flatpak
    sums_url: str    # its SHA256SUMS


def own_version(path=None):
    """This installer's version, or "" when it isn't a built one (run from the repo)."""
    try:
        text = Path(path or host.DATA / "VERSION").read_text()
    except OSError:
        return ""
    for line in text.splitlines():
        key, _, value = line.partition("=")
        if key == "VERSION":
            return value.strip()
    return ""


def installation(info="/.flatpak-info", home=host.HOME):
    """Where flatpak has this app: "user", "system", or "" outside a Flatpak."""
    try:
        text = Path(info).read_text()
    except OSError:
        return ""
    for line in text.splitlines():
        key, _, value = line.partition("=")
        if key == "app-path":
            return "user" if Path(value).is_relative_to(home) else "system"
    return "system"


def check(own, get=releases.fetch):
    """The Update newer than version own, or None. Raises OSError or ValueError when GitHub
    can't be read."""
    if releases.version_key(own) is None:
        return None     # a local build: nothing to compare with
    channel = "experimental" if "-" in own else "stable"
    rel = releases.pick(channel, get, having=BUNDLE)
    if not rel:
        return None
    tag = str(rel.get("tag_name", ""))
    version = tag[1:] if tag.startswith("v") else ""
    if not releases.newer(version, own):
        return None
    urls = [(releases.asset(rel, name) or {}).get("browser_download_url") for name in (BUNDLE, SUMS)]
    if not all(isinstance(u, str) and u.startswith(DOWNLOADS) for u in urls):
        return None     # no checksums to check it against, or files from somewhere else
    return Update(version, urls[0], urls[1])


def fetch_file(url, limit, timeout=60):
    req = urllib.request.Request(url, headers={"User-Agent": releases.UA, "Accept": "application/octet-stream"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        data = r.read(limit + 1)
    if len(data) > limit:
        raise UpdateError("the download is far bigger than the installer is")
    return data


def listed_sum(sums, name=BUNDLE):
    """name's SHA-256 in a SHA256SUMS file's text, or ""."""
    for line in sums.splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[1].lstrip("*") == name and len(parts[0]) == 64:
            return parts[0].lower()
    return ""


def download(update, folder=None, get=fetch_file):
    """Download the update's bundle and check it. Returns its path; raises UpdateError."""
    folder = Path(folder or FOLDER)
    try:
        want = listed_sum(get(update.sums_url, MAX_SUMS).decode("utf-8", "replace"))
        if not want:
            raise UpdateError(f"Frametop {update.version}'s SHA256SUMS doesn't list the installer")
        data = get(update.url, MAX_BUNDLE)
    except OSError as e:
        raise UpdateError(f"couldn't download it ({e})") from e
    if hashlib.sha256(data).hexdigest() != want:
        raise UpdateError("the download doesn't match its checksum")
    try:
        folder.mkdir(parents=True, exist_ok=True)
        fd, tmp = tempfile.mkstemp(dir=folder, prefix=f".{BUNDLE}.")
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        os.replace(tmp, folder / BUNDLE)
    except OSError as e:
        raise UpdateError(f"couldn't save it in {folder} ({e})") from e
    return folder / BUNDLE


def install_args(path, where):
    """The host's flatpak command that installs the bundle over this app. No --noninteractive:
    with it flatpak may not ask for the password a system-wide install needs."""
    return ["flatpak", "install", f"--{where}", "--bundle", "--or-update", "--assumeyes", str(path)]


def install(path, where, run=host.run):
    """Install the downloaded bundle where this app is installed. Raises UpdateError."""
    code, out, err = run(install_args(path, where), timeout=600)
    if code != 0:
        said = (err.strip() or out.strip()).decode("utf-8", "replace").splitlines()
        raise UpdateError(said[-1] if said else f"flatpak install failed ({code})")


def reopen(argv=(), popen=subprocess.Popen):
    """Start the version installed now, a moment from now: by then this window has quit and
    given up the app's name, which a second window would otherwise hand its start to. Returns
    False when it couldn't."""
    try:
        popen(["flatpak-spawn", "--latest-version", "sh", "-c", 'sleep 1; exec frametop-installer "$@"',
               "sh", *argv], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
              start_new_session=True)
        return True
    except OSError:
        return False
