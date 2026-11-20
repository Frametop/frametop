#!/usr/bin/env python3
"""Fetch Full Keyboard for Steam Frame (TaiKeid's) into keyboard/build/release, standard library only.

Frametop runs Full Keyboard's own release package, unmodified, as a hosted keyboard
(keyboard/FRAMETOP.md). Every build takes the newest stable release, not a pinned one
(Daenon, 2026-11-20: "on future builds, we actually just install the latest version of the
keyboard rather than pinning a specific version"). That is a deliberate exception to the
root AGENTS.md's "Pin every input". What makes it safe enough:

- Only TaiKeid's repository, over HTTPS, and only a published, stable release (no draft or
  prerelease) whose tag is a plain version no older than MINIMUM, the first one Frametop's host
  was tested with.
- The package must match its line in the release's SHA256SUMS and GitHub's own digest of the
  asset; it unpacks with tarfile's "data" filter (no links out, no device files, no absolute
  paths); its program must say that version and speak hosted protocol 1 (checked where it can
  run: on the Frame, with SteamVR's libopenvr_api; a release image's build can't run it, and
  ft-kbhost logs the version the keyboard reports).
- What went in is written to RELEASE in it (version, sha256, where from), so a build says which
  keyboard it carries. FULLKB_VERSION=x.y.z takes that exact release instead, for going back
  to a known-good one while a new release has a problem.

  keyboard/fetch.py [--dest DIR] [--force]

A folder that already holds the chosen release is kept. GITHUB_TOKEN, when set, is sent to
api.github.com only (CI's rate limit). Exits 1 with a reason when anything doesn't check out,
leaving what was there before.
"""
import argparse
import hashlib
import http.client
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.parse
import urllib.request
from pathlib import Path

REPO = "TaiKeid/steam-frame-full-keyboard"
API = f"https://api.github.com/repos/{REPO}/releases"
MINIMUM = (0, 19, 1)   # the first release ft-kbhost was tested with (hosted protocol 1)
PROTOCOL = 1           # the hosted protocol ft-kbhost speaks
VERSION = re.compile(r"^(\d{1,4})\.(\d{1,4})\.(\d{1,4})$")
# Where a release download may go: GitHub, and the storage its asset links redirect to.
HOSTS = {"api.github.com", "github.com", "objects.githubusercontent.com",
         "release-assets.githubusercontent.com"}
MAX_PACKAGE = 400 * 1024 * 1024
HERE = Path(__file__).resolve().parent


class FetchError(Exception):
    pass


class _OnlyGitHub(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        check_url(newurl)
        new = super().redirect_request(req, fp, code, msg, headers, newurl)
        if new is not None and urllib.parse.urlsplit(newurl).hostname != "api.github.com":
            new.remove_header("Authorization")
        return new


def check_url(url):
    parts = urllib.parse.urlsplit(url)
    if parts.scheme != "https" or parts.hostname not in HOSTS:
        raise FetchError(f"refusing to download from {parts.scheme}://{parts.hostname}")


def get(url, limit, accept=None):
    check_url(url)
    request = urllib.request.Request(url, headers={"User-Agent": "frametop-keyboard-fetch"})
    if accept:
        request.add_header("Accept", accept)
    token = os.environ.get("GITHUB_TOKEN")
    if token and urllib.parse.urlsplit(url).hostname == "api.github.com":
        request.add_header("Authorization", f"Bearer {token}")
    opener = urllib.request.build_opener(_OnlyGitHub)
    try:
        with opener.open(request, timeout=60) as response:
            data = response.read(limit + 1)
    except (OSError, http.client.HTTPException) as error:   # (a cut-short read is the latter)
        raise FetchError(f"download failed: {url}: {error}") from None
    if len(data) > limit:
        raise FetchError(f"{url} is larger than {limit} bytes")
    return data


def version_of(tag):
    match = VERSION.match(tag or "")
    return tuple(int(n) for n in match.groups()) if match else None


def choose(release):
    """The package and checksum assets of one release (GitHub's JSON), after its checks."""
    tag = release.get("tag_name", "")
    version = version_of(tag)
    if version is None:
        raise FetchError(f"release tag {tag!r} isn't a plain version")
    if release.get("draft") or release.get("prerelease"):
        raise FetchError(f"{tag} is a draft or a prerelease")
    if version < MINIMUM:
        raise FetchError(f"{tag} is older than {'.'.join(map(str, MINIMUM))}, which Frametop needs")
    assets = {a.get("name"): a for a in release.get("assets", [])}
    package = assets.get(f"framekeyboard-{tag}-aarch64.tar.gz")
    sums = assets.get("SHA256SUMS")
    if not package or not sums:
        raise FetchError(f"{tag} has no aarch64 package or no SHA256SUMS")
    return tag, package, sums


def expected_sha(sums_text, name, digest):
    """The package's sha256: its SHA256SUMS line, which GitHub's digest (when given) must match."""
    listed = None
    for line in sums_text.splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[1].lstrip("*") == name and re.fullmatch(r"[0-9a-f]{64}", parts[0]):
            listed = parts[0]
    if listed is None:
        raise FetchError(f"SHA256SUMS has no line for {name}")
    if digest and digest != f"sha256:{listed}":
        raise FetchError(f"GitHub's digest of {name} doesn't match SHA256SUMS")
    return listed


def unpack(data, tag, into):
    """The package's folder, unpacked under `into` with tarfile's data filter."""
    top = f"framekeyboard-{tag}-aarch64"
    with tempfile.TemporaryFile() as raw:
        raw.write(data)
        raw.seek(0)
        try:
            with tarfile.open(fileobj=raw, mode="r:gz") as archive:
                for member in archive.getmembers():
                    if member.name != top and not member.name.startswith(top + "/"):
                        raise FetchError(f"package entry {member.name!r} is outside {top}/")
                archive.extractall(into, filter="data")
        except (tarfile.TarError, EOFError, OSError) as error:   # FilterError: a link out, a device
            raise FetchError(f"the package didn't unpack: {error}") from None
    folder = Path(into) / top
    if not (folder / "bin" / "framekeyboard").is_file() or not (folder / "share" / "framekeyboard").is_dir():
        raise FetchError("the package has no bin/framekeyboard or share/framekeyboard")
    return folder


OPENVR = Path("/opt/steamvr/bin/linuxarm64/libopenvr_api.so")   # its RUNPATH: SteamVR's


def check_program(folder, tag):
    """Its program says this version and speaks PROTOCOL, where it can run (the Frame's host)."""
    if os.uname().machine != "aarch64" or not OPENVR.is_file():
        print(f"(not running it here to check it: no {OPENVR})")
        return
    program = str(folder / "bin" / "framekeyboard")

    def say(flag):
        try:
            return subprocess.run([program, flag], capture_output=True, text=True, timeout=30).stdout.split()
        except (OSError, subprocess.SubprocessError) as error:
            raise FetchError(f"its program didn't run: {error}") from None
    said = say("--version")
    if said[-1:] != [tag]:
        raise FetchError(f"the program says {' '.join(said)!r}, not {tag}")
    protocols = say("--protocols")
    if str(PROTOCOL) not in protocols:
        raise FetchError(f"{tag} doesn't speak hosted protocol {PROTOCOL} (it says {protocols})")


def installed(dest):
    try:
        return json.loads((dest / "RELEASE").read_text())
    except (OSError, ValueError):
        return {}


def fetch(dest, force=False, exact=None):
    url = f"{API}/tags/{exact}" if exact else f"{API}/latest"
    if exact and version_of(exact) is None:
        raise FetchError(f"FULLKB_VERSION={exact!r} isn't a plain version")
    try:
        release = json.loads(get(url, 2 * 1024 * 1024, "application/vnd.github+json"))
    except ValueError:
        raise FetchError(f"{url} didn't answer with a release") from None
    if not isinstance(release, dict):
        raise FetchError(f"{url} didn't answer with a release")
    tag, package, sums = choose(release)
    sums_text = get(sums["browser_download_url"], 64 * 1024).decode(errors="replace")
    sha = expected_sha(sums_text, package["name"], package.get("digest"))
    have = installed(dest)
    if not force and have.get("version") == tag and have.get("sha256") == sha \
            and (dest / "bin" / "framekeyboard").is_file():
        print(f"Full Keyboard {tag} is already in {dest}")
        return tag
    data = get(package["browser_download_url"], MAX_PACKAGE)
    if hashlib.sha256(data).hexdigest() != sha:
        raise FetchError(f"{package['name']} doesn't match its sha256")
    dest.parent.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix=".fetch-", dir=dest.parent))
    try:
        folder = unpack(data, tag, work)
        check_program(folder, tag)
        (folder / "RELEASE").write_text(json.dumps(
            {"version": tag, "sha256": sha, "package": package["name"], "from": package["browser_download_url"]},
            indent=1) + "\n")
        old = work / "old"
        if dest.exists():
            dest.rename(old)   # removed with work below
        try:
            folder.rename(dest)
        except OSError:
            if old.exists():
                old.rename(dest)   # what was there stays
            raise
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print(f"Full Keyboard {tag} in {dest} (sha256 {sha})")
    return tag


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--dest", type=Path, default=HERE / "build" / "release")
    parser.add_argument("--force", action="store_true", help="download even if --dest holds that release")
    args = parser.parse_args(argv)
    try:
        fetch(args.dest.resolve(), args.force, os.environ.get("FULLKB_VERSION") or None)
    except FetchError as error:
        print(f"keyboard/fetch.py: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
