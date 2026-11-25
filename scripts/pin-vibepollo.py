#!/usr/bin/env python3
"""Pin the hosts' Vibepollo to a release of Frametop's fork (Frametop/frametop-vibepollo).

host/vibepollo-pin.json is the one place the pin lives: the release tag, the Arch package and
the SHA-256s of everything the host kits download. The kits can't read it when they run (get.sh
is piped from curl, the Windows kit runs on its own), so each keeps its own copy of the values,
and this script writes them in from the pin:

  host/linux/get.sh                       TAG, PACKAGE and both SHA-256s
  host/windows/frametop-host-setup.ps1    the release URL and each file's SHA-256; the build it
                                          replaces goes on $OlderBuildShas
  README.md, host/*/README.md             every mention of the pinned tag and package (links,
                                          steps): a sentence there about an earlier build has
                                          to name that build, not the pinned one

  scripts/pin-vibepollo.py pin [TAG] [--base VERSION]
                                       pin TAG, or the fork's latest release (the GitHub API,
                                       each file's .sha256 asset, and Vibepollo's Linux installer
                                       at the tag, hashed here); then apply. The Windows base
                                       comes from the release notes, or --base
  scripts/pin-vibepollo.py apply       write the pin into the files (after editing it by hand)
  scripts/pin-vibepollo.py check       exit 1 if a file doesn't match the pin; changes nothing

The installers people run download the kit from `main` (FrametopHostSetup.exe and the Pages
one-liner both do), not from this branch. So a new pin reaches them only once host/ and the
README section are on main too: land the same change there.

Windows: Frametop's files go into a stock Vibepollo install (windows.base, the kit's
$VibepolloVersion), which the release notes name. A release built for another base needs the
kit changed by hand first (its installer URL and hash, the stock files' hashes, $EarlierShas),
so pin and apply refuse when the two don't match, and change nothing.
"""
import argparse
import hashlib
import json
import re
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPO = "Frametop/frametop-vibepollo"
PIN = "host/vibepollo-pin.json"
GET_SH = "host/linux/get.sh"
KIT = "host/windows/frametop-host-setup.ps1"
DOCS = ("README.md", "host/linux/README.md", "host/windows/README.md")
# The Windows host setup as a program (the fork's packaging/windows/frametop-host-setup), on
# the fork's releases after frametop-2.0.4. It holds no pins (it runs the kit from main), so
# the pages link the latest release's copy.
SETUP_EXE = "FrametopHostSetup.exe"
SETUP_EXE_URL = f"https://github.com/{REPO}/releases/latest/download/{SETUP_EXE}"
WINDOWS_FILES = ("sunshine.exe", "tools\\sunshine_wgc_capture.exe", "tools\\frametop_clipboard.exe")
SHA = re.compile(r"[0-9a-f]{64}")


class PinError(Exception):
    pass


def http_get(url):
    request = urllib.request.Request(url, headers={"User-Agent": "frametop-pin-vibepollo"})
    with urllib.request.urlopen(request, timeout=60) as response:
        return response.read()


def read_pin(root):
    return validate(json.loads((root / PIN).read_text()))


def validate(pin):
    """PIN, if every value is one the kits can take as it is (get.sh is piped to bash)."""
    if not re.fullmatch(r"frametop-[0-9][0-9A-Za-z.-]*", pin["tag"]):
        raise PinError(f"{PIN}: tag {pin['tag']!r} isn't a frametop-* release tag")
    if not re.fullmatch(r"vibepollo-[0-9A-Za-z.-]+-x86_64\.pkg\.tar\.zst", pin["linux"]["package"]):
        raise PinError(f"{PIN}: {pin['linux']['package']!r} isn't an Arch package name")
    for key in ("package_sha256", "installer_sha256"):
        if not SHA.fullmatch(pin["linux"][key]):
            raise PinError(f"{PIN}: linux.{key} isn't a lowercase SHA-256")
    if sorted(pin["windows"]["files"]) != sorted(WINDOWS_FILES):
        raise PinError(f"{PIN}: windows.files must be exactly {', '.join(WINDOWS_FILES)}")
    for path, sha in pin["windows"]["files"].items():
        if not SHA.fullmatch(sha):
            raise PinError(f"{PIN}: windows.files[{path!r}] isn't a lowercase SHA-256")
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+)+", pin["windows"]["base"] or ""):
        raise PinError(f"{PIN}: windows.base {pin['windows']['base']!r} isn't a Vibepollo version")
    return pin


def write_pin(root, pin):
    (root / PIN).write_text(json.dumps(pin, indent=2) + "\n")


# --- the release ----------------------------------------------------------------------------

def sha_asset(fetch, assets, name):
    """The SHA-256 in NAME.sha256 (sha256sum's '<hex>  <name>'), which the fork's CI publishes."""
    if name not in assets or f"{name}.sha256" not in assets:
        raise PinError(f"the release has no {name} with its .sha256")
    fields = fetch(assets[f"{name}.sha256"]).decode().split()
    if len(fields) != 2 or not SHA.fullmatch(fields[0].lower()) or fields[1].lstrip("*") != name:
        raise PinError(f"{name}.sha256 isn't '<sha256>  {name}'")
    return fields[0].lower()


# The fork's release notes say which stock Vibepollo the Windows files go into
# (.github/frametop-release-notes.md: "... go into a Vibepollo 2.0.3 install").
BASE_IN_NOTES = re.compile(r"go into a Vibepollo ([0-9]+(?:\.[0-9]+)+) install")


def pin_from_release(fetch, tag=None, base=None):
    """The pin for TAG (or the latest release), from the release's assets and notes; BASE
    overrides the Windows base the notes give. FETCH(url) -> bytes."""
    tag_asked = tag is not None
    api = f"https://api.github.com/repos/{REPO}/releases/" + (f"tags/{tag}" if tag_asked else "latest")
    release = json.loads(fetch(api))
    tag = release["tag_name"]
    if not re.fullmatch(r"frametop-[0-9][0-9A-Za-z.-]*", tag):
        raise PinError(f"{REPO}'s release {tag!r} isn't a frametop-* release")
    latest = json.loads(fetch(f"https://api.github.com/repos/{REPO}/releases/latest")) if tag_asked else release
    if release.get("draft") or release.get("prerelease"):
        raise PinError(f"{tag} is a draft or a prerelease")
    assets = {a["name"]: a["browser_download_url"] for a in release["assets"]}
    packages = [n for n in assets if re.fullmatch(r"vibepollo-[0-9A-Za-z.-]+-x86_64\.pkg\.tar\.zst", n) and "-debug-" not in n]
    if len(packages) != 1:
        raise PinError(f"{tag}: expected one Arch package, found {len(packages)}")
    if base is None:
        found = BASE_IN_NOTES.findall(release.get("body") or "")
        if len(set(found)) != 1:
            raise PinError(f"{tag}'s notes don't say which Vibepollo its Windows files go into; give it with --base VERSION")
        base = found[0]
    installer = fetch(f"https://raw.githubusercontent.com/{REPO}/{tag}/scripts/linux_install.sh")
    pin = {
        "tag": tag,
        "linux": {
            "package": packages[0],
            "package_sha256": sha_asset(fetch, assets, packages[0]),
            "installer_sha256": hashlib.sha256(installer).hexdigest(),
        },
        "windows": {
            "base": base,
            "files": {p: sha_asset(fetch, assets, p.split("\\")[-1]) for p in WINDOWS_FILES},
        },
    }
    warnings = []
    # The pages link the latest release's copy, whichever release is pinned.
    if SETUP_EXE not in {a["name"] for a in latest["assets"]}:
        warnings.append(f"the latest release ({latest['tag_name']}) has no {SETUP_EXE}: "
                        "the pages' Windows download link won't work until one has")
    return validate(pin), warnings


# --- the files ------------------------------------------------------------------------------

def sub_once(text, pattern, repl, where):
    new, n = re.subn(pattern, repl, text, flags=re.M)
    if n != 1:
        raise PinError(f"{where}: expected one match for {pattern!r}, found {n}")
    return new


def one(text, pattern, where):
    found = re.findall(pattern, text, flags=re.M)
    if len(found) != 1:
        raise PinError(f"{where}: expected one match for {pattern!r}, found {len(found)}")
    return found[0]


# $OlderBuildShas = @() or a list closed by a line that is just ")".
OLDER = r"^\$OlderBuildShas = @\((?:\)|\n((?:.*\n)*?)\))$"


def read_texts(root):
    return {p: (root / p).read_text() for p in (GET_SH, KIT) + DOCS}


def current(texts):
    """The values the kits hold: what a new pin replaces."""
    sh, kit = texts[GET_SH], texts[KIT]
    files = {}
    for path in WINDOWS_FILES:
        files[path] = one(kit, r'^\s*@\{ Path = "' + re.escape(path) + r'"; Sha = "([0-9A-F]{64})"', KIT).lower()
    return {
        "tag": one(sh, r"^TAG=(\S+)$", GET_SH),
        "kit_tag": one(kit, r'^\$BuildUrl = "https://github\.com/' + re.escape(REPO) + r'/releases/download/([^"]+)"$', KIT),
        "linux": {
            "package": one(sh, r"^PACKAGE=(\S+)$", GET_SH),
            "package_sha256": one(sh, r"^PACKAGE_SHA256=(\S+)$", GET_SH),
            "installer_sha256": one(sh, r"^INSTALLER_SHA256=(\S+)$", GET_SH),
        },
        "windows": {"base": one(kit, r'^\$VibepolloVersion = "([^"]+)"$', KIT), "files": files},
        "older": [s.lower() for s in re.findall(r'"([0-9A-F]{64})"', one(kit, OLDER, KIT))],
    }


def tag_pattern(tag):
    """TAG as a whole name: frametop-2.0.4, not frametop-2.0.40, frametop-2.0.4.1 or frametop-2.0.4-1."""
    return r"(?<![0-9A-Za-z.-])" + re.escape(tag) + r"(?![.-]?[0-9A-Za-z])"


def package_pattern(package):
    """PACKAGE as a whole name; its .sha256 file's name has it too."""
    return r"(?<![0-9A-Za-z.-])" + re.escape(package) + r"(?![0-9A-Za-z_-])"


def problems_in(pin, texts):
    now = current(texts)
    problems = []
    if now["tag"] != pin["tag"] or now["kit_tag"] != pin["tag"]:
        problems.append(f"the kits pin {now['tag']} (Linux) and {now['kit_tag']} (Windows), the pin says {pin['tag']}")
    if now["linux"] != pin["linux"]:
        problems.append(f"{GET_SH} doesn't match the pin's linux values")
    if now["windows"]["base"] != pin["windows"]["base"]:
        problems.append(f"{KIT} goes into Vibepollo {now['windows']['base']}, the pin says {pin['windows']['base']}")
    if now["windows"]["files"] != pin["windows"]["files"]:
        problems.append(f"{KIT}'s file SHA-256s don't match the pin's")
    for doc in DOCS:
        if not re.search(tag_pattern(pin["tag"]), texts[doc]):
            problems.append(f"{doc} doesn't name {pin['tag']}")
    for doc in ("README.md", "host/windows/README.md"):
        if SETUP_EXE_URL not in texts[doc]:
            problems.append(f"{doc} doesn't link {SETUP_EXE_URL}")
    return problems


def check(root):
    return problems_in(read_pin(root), read_texts(root))


def render(pin, texts):
    """TEXTS with PIN written in: a new dict; refuses (PinError) rather than half-writing."""
    now = current(texts)
    if now["windows"]["base"] != pin["windows"]["base"]:
        raise PinError(
            f"the pin's Windows files go into Vibepollo {pin['windows']['base']}, but {KIT} installs "
            f"{now['windows']['base']}: change the kit's base by hand first (see this script's docstring)")
    if now["tag"] != now["kit_tag"]:
        raise PinError(f"{GET_SH} pins {now['tag']} but {KIT} pins {now['kit_tag']}: fix that by hand first")
    old_tag, new_tag = now["tag"], pin["tag"]
    old_pkg, new_pkg = now["linux"]["package"], pin["linux"]["package"]

    texts = dict(texts)
    # Names first: every mention of the pinned tag and package (links, steps) moves along, so a
    # sentence in these files that means an earlier build has to name another one.
    for p in texts:
        if old_tag != new_tag:
            texts[p] = re.sub(tag_pattern(old_tag), new_tag, texts[p])
        if old_pkg != new_pkg:
            texts[p] = re.sub(package_pattern(old_pkg), new_pkg, texts[p])
    sh = texts[GET_SH]
    sh = sub_once(sh, r"^TAG=\S+$", f"TAG={new_tag}", GET_SH)
    sh = sub_once(sh, r"^PACKAGE=\S+$", f"PACKAGE={new_pkg}", GET_SH)
    sh = sub_once(sh, r"^PACKAGE_SHA256=\S+$", f"PACKAGE_SHA256={pin['linux']['package_sha256']}", GET_SH)
    sh = sub_once(sh, r"^INSTALLER_SHA256=\S+$", f"INSTALLER_SHA256={pin['linux']['installer_sha256']}", GET_SH)
    texts[GET_SH] = sh

    kit = texts[KIT]
    for path, sha in pin["windows"]["files"].items():
        kit = sub_once(kit, r'^(\s*@\{ Path = "' + re.escape(path) + r'"; Sha = ")[0-9A-F]{64}"',
                       lambda m, sha=sha: m.group(1) + sha.upper() + '"', KIT)
    # The build this one replaces is still a Frametop build on the same base: the kit has to
    # know its sunshine.exe to update over it.
    old_exe, new_exe = now["windows"]["files"]["sunshine.exe"], pin["windows"]["files"]["sunshine.exe"]
    if old_exe != new_exe and old_exe not in now["older"]:
        entry = f'    "{old_exe.upper()}"  # {old_tag}\n'
        kit = sub_once(kit, OLDER, lambda m: "$OlderBuildShas = @(\n" + (m.group(1) or "") + entry + ")", KIT)
    texts[KIT] = kit

    problems = problems_in(pin, texts)
    if problems:
        raise PinError("the files wouldn't match the pin: " + "; ".join(problems))
    return texts


def write_texts(root, before, after):
    changed = [p for p in after if after[p] != before[p]]
    for p in changed:
        (root / p).write_text(after[p])
    return changed


def apply(root):
    texts = read_texts(root)
    return write_texts(root, texts, render(read_pin(root), texts))


def pin(root, tag=None, base=None, fetch=http_get):
    old = read_pin(root)
    new, warnings = pin_from_release(fetch, tag, base)
    texts = read_texts(root)
    after = render(new, texts)  # refuses before anything is written
    if new != old:
        write_pin(root, new)
    return new, write_texts(root, texts, after), warnings


def main(argv):
    parser = argparse.ArgumentParser(prog="pin-vibepollo.py", description="Pin the hosts' Vibepollo (see the docstring).")
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("pin", help="pin TAG, or the fork's latest release, then apply")
    p.add_argument("tag", nargs="?")
    p.add_argument("--base", help="the Vibepollo the Windows files go into, if the release notes don't say")
    sub.add_parser("apply", help="write the pin into the files")
    sub.add_parser("check", help="exit 1 if a file doesn't match the pin")
    try:
        args = parser.parse_args(argv[1:])
    except SystemExit as e:
        return e.code
    try:
        if args.command == "check":
            problems = check(ROOT)
            for problem in problems:
                print(f"pin-vibepollo: {problem}", file=sys.stderr)
            if not problems:
                print(f"pin-vibepollo: every file pins {read_pin(ROOT)['tag']}")
            return 1 if problems else 0
        if args.command == "apply":
            changed = apply(ROOT)
            tag = read_pin(ROOT)["tag"]
        else:
            new, changed, warnings = pin(ROOT, args.tag, args.base)
            tag = new["tag"]
            for w in warnings:
                print(f"pin-vibepollo: warning: {w}", file=sys.stderr)
        print(f"pin-vibepollo: pinned {tag}" + (f"; changed {', '.join(changed)}" if changed else "; nothing to change"))
        return 0
    except PinError as e:
        print(f"pin-vibepollo: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
