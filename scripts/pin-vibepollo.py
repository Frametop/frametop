#!/usr/bin/env python3
"""Pin the hosts' Vibepollo to a release of Frametop's fork (Frametop/frametop-vibepollo).

host/vibepollo-pin.json is the one place the pin lives: the release tag, the Arch package and
the SHA-256s of everything the host kits download. The kits can't read it when they run (get.sh
is piped from curl, the Windows kit runs on its own), so each keeps its own copy of the values,
and this script writes them in from the pin:

  host/linux/get.sh                       TAG, PACKAGE and both SHA-256s
  host/windows/frametop-host-setup.ps1    the release URL and each file's SHA-256; the build it
                                          replaces goes on $OlderBuildShas
  README.md, host/*/README.md             the tag and package names (links, the Windows exe)

  scripts/pin-vibepollo.py pin [TAG]   pin TAG, or the fork's latest release (the GitHub API,
                                       each file's .sha256 asset, and Vibepollo's Linux installer
                                       at the tag, hashed here); then apply
  scripts/pin-vibepollo.py apply       write the pin into the files (after editing it by hand)
  scripts/pin-vibepollo.py check       exit 1 if a file doesn't match the pin; changes nothing

Windows: Frametop's files go into a stock Vibepollo install (windows.base, the kit's
$VibepolloVersion). A release built for another base needs the kit changed by hand first
(its installer URL and hash, the stock files' hashes, $EarlierShas), so pin and apply refuse
when the two don't match.
"""
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
# The Windows host setup as a program, on each release since frametop-2.0.4 (the fork's
# packaging/windows/frametop-host-setup). It holds no pins: it runs the kit from main.
SETUP_EXE = "FrametopHostSetup.exe"
WINDOWS_FILES = ("sunshine.exe", "tools\\sunshine_wgc_capture.exe", "tools\\frametop_clipboard.exe")
SHA = re.compile(r"[0-9a-f]{64}")


class PinError(Exception):
    pass


def http_get(url):
    request = urllib.request.Request(url, headers={"User-Agent": "frametop-pin-vibepollo"})
    with urllib.request.urlopen(request, timeout=60) as response:
        return response.read()


def read_pin(root):
    pin = json.loads((root / PIN).read_text())
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


def pin_from_release(fetch, tag=None):
    """The pin for TAG (or the latest release), from the release's assets. FETCH(url) -> bytes."""
    api = f"https://api.github.com/repos/{REPO}/releases/" + (f"tags/{tag}" if tag else "latest")
    release = json.loads(fetch(api))
    tag = release["tag_name"]
    if not tag.startswith("frametop-"):
        raise PinError(f"{REPO}'s release {tag} isn't a frametop-* release")
    if release.get("draft") or release.get("prerelease"):
        raise PinError(f"{tag} is a draft or a prerelease")
    assets = {a["name"]: a["browser_download_url"] for a in release["assets"]}
    packages = [n for n in assets if re.fullmatch(r"vibepollo-[0-9A-Za-z.-]+-x86_64\.pkg\.tar\.zst", n) and "-debug-" not in n]
    if len(packages) != 1:
        raise PinError(f"{tag}: expected one Arch package, found {len(packages)}")
    installer = fetch(f"https://raw.githubusercontent.com/{REPO}/{tag}/scripts/linux_install.sh")
    pin = {
        "tag": tag,
        "linux": {
            "package": packages[0],
            "package_sha256": sha_asset(fetch, assets, packages[0]),
            "installer_sha256": hashlib.sha256(installer).hexdigest(),
        },
        "windows": {
            "base": None,  # kept from the current pin: a release doesn't say
            "files": {p: sha_asset(fetch, assets, p.split("\\")[-1]) for p in WINDOWS_FILES},
        },
    }
    warnings = []
    if SETUP_EXE not in assets:
        warnings.append(f"{tag} has no {SETUP_EXE}: the pages' Windows download link won't work until it's uploaded")
    return pin, warnings


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


def current(root):
    """The values the files hold now: what apply replaces."""
    sh = (root / GET_SH).read_text()
    kit = (root / KIT).read_text()
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
        "older": [s.lower() for s in re.findall(r'"([0-9A-F]{64})"', one(kit, r"^\$OlderBuildShas = @\(((?:.|\n)*?)\)$", KIT))],
    }


def name_pattern(name):
    """NAME as a whole word: frametop-2.0.4 but not frametop-2.0.40 or frametop-2.0.4.1."""
    return r"(?<![0-9A-Za-z.-])" + re.escape(name) + r"(?!\.?[0-9A-Za-z])"


def check(root):
    pin = read_pin(root)
    now = current(root)
    problems = []
    if now["tag"] != pin["tag"] or now["kit_tag"] != pin["tag"]:
        problems.append(f"the kits pin {now['tag']} (Linux) and {now['kit_tag']} (Windows), the pin says {pin['tag']}")
    if now["linux"] != pin["linux"]:
        problems.append(f"{GET_SH} doesn't match the pin's linux values")
    if now["windows"]["base"] != pin["windows"]["base"]:
        problems.append(f"{KIT} goes into Vibepollo {now['windows']['base']}, the pin says {pin['windows']['base']}")
    if now["windows"]["files"] != pin["windows"]["files"]:
        problems.append(f"{KIT}'s file SHA-256s don't match the pin's")
    exe = f"https://github.com/{REPO}/releases/download/{pin['tag']}/{SETUP_EXE}"
    for doc in ("README.md", "host/windows/README.md"):
        if exe not in (root / doc).read_text():
            problems.append(f"{doc} doesn't link {exe}")
    return problems


def apply(root):
    pin = read_pin(root)
    now = current(root)
    if now["windows"]["base"] != pin["windows"]["base"]:
        raise PinError(
            f"the pin's Windows files go into Vibepollo {pin['windows']['base']}, but {KIT} installs "
            f"{now['windows']['base']}: change the kit's base by hand first (see this script's docstring)")
    if now["tag"] != now["kit_tag"]:
        raise PinError(f"{GET_SH} pins {now['tag']} but {KIT} pins {now['kit_tag']}: fix that by hand first")
    old_tag, new_tag = now["tag"], pin["tag"]
    old_pkg, new_pkg = now["linux"]["package"], pin["linux"]["package"]

    texts = {p: (root / p).read_text() for p in (GET_SH, KIT) + DOCS}
    # Names first: every mention of the pinned tag and package (links, steps) moves along, so a
    # doc that means an earlier build names another one.
    for p in texts:
        if old_tag != new_tag:
            texts[p] = re.sub(name_pattern(old_tag), new_tag, texts[p])
        if old_pkg != new_pkg:
            texts[p] = re.sub(name_pattern(old_pkg), new_pkg, texts[p])
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
        kit = sub_once(kit, r"^\$OlderBuildShas = @\(((?:.|\n)*?)\)$",
                       lambda m: "$OlderBuildShas = @(\n" + m.group(1).strip("\n") + ("\n" if m.group(1).strip() else "") + entry + ")",
                       KIT)
    texts[KIT] = kit

    changed = [p for p, t in texts.items() if t != (root / p).read_text()]
    for p in changed:
        (root / p).write_text(texts[p])
    problems = check(root)
    if problems:
        raise PinError("after applying: " + "; ".join(problems))
    return changed


def pin(root, tag=None, fetch=http_get):
    old = read_pin(root)
    new, warnings = pin_from_release(fetch, tag)
    new["windows"]["base"] = old["windows"]["base"]
    if new == old:
        return new, apply(root), warnings
    write_pin(root, new)
    try:
        return new, apply(root), warnings
    except PinError:
        write_pin(root, old)  # apply changed nothing; leave the pin as it was too
        raise


def main(argv):
    if len(argv) < 2 or argv[1] not in ("pin", "apply", "check") or len(argv) > (3 if argv[1] == "pin" else 2):
        print("usage: pin-vibepollo.py pin [TAG] | apply | check", file=sys.stderr)
        return 2
    try:
        if argv[1] == "check":
            problems = check(ROOT)
            for p in problems:
                print(f"pin-vibepollo: {p}", file=sys.stderr)
            if not problems:
                print(f"pin-vibepollo: every file pins {read_pin(ROOT)['tag']}")
            return 1 if problems else 0
        if argv[1] == "apply":
            changed = apply(ROOT)
            tag = read_pin(ROOT)["tag"]
        else:
            new, changed, warnings = pin(ROOT, argv[2] if len(argv) == 3 else None)
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
