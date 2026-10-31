#!/usr/bin/env python3
"""Offline test of install-release.sh's install, release after release (docs/charts G7): which
release is "current", which folders stay, and a reinstall over fast-installed files. podman is a
shell function that copies a made-up tree, /etc/os-release a made-up one, and each tree's
install.sh is install.sh's own "current" block (DEE-286). Everything happens in a temp folder:
no podman, no install, no network.

  pack/test/install-release-test.py
"""
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.join(HERE, "..", "..")
TMP = tempfile.mkdtemp(prefix="ft-install-release-test-")
RELEASES = os.path.join(TMP, "releases")
OS_RELEASE = os.path.join(TMP, "os-release")
Path(OS_RELEASE).write_text('ID=steamos\nVARIANT_ID=vr\nBUILD_ID=20260922.6101926\nVERSION_ID=3.8\n')
SCRIPT = (Path(REPO, "pack", "install-release.sh").read_text().rsplit('\nmain "$@"', 1)[0]
          .replace("/etc/os-release", OS_RELEASE))

# install.sh's block that makes its own release current, as each made-up tree's install.sh
INSTALL = Path(REPO, "install.sh").read_text()
start = INSTALL.index("# A release's install.sh is also how you go back to it")
CURRENT_BLOCK = INSTALL[start:INSTALL.index('step "Done"', start)]

# podman: "image inspect" names the loaded image, "cp" copies the made-up tree, the rest succeed
PODMAN = r'''
podman() {
  case "$1 $2" in
    "image inspect") echo "$FAKE_ID" ;;
    *) [ "$1" = cp ] && cp -a "$FAKE_TREE/." "$3" ;;
  esac
  return 0
}
'''
failures = []


def check(label, got, want):
    ok = got == want
    print(("ok    " if ok else "FAIL  ") + label + ("" if ok else f": got {got!r}, want {want!r}"), flush=True)
    if not ok:
        failures.append(label)


def oci_archive(path, config):
    def digest(b):
        return hashlib.sha256(b).hexdigest()
    manifest = json.dumps({"schemaVersion": 2, "config": {"digest": "sha256:" + digest(config)}, "layers": []}).encode()
    index = json.dumps({"schemaVersion": 2, "manifests": [{"digest": "sha256:" + digest(manifest)}]}).encode()
    with tarfile.open(path, "w") as tar:
        for name, data in (("index.json", index), (f"blobs/sha256/{digest(manifest)}", manifest),
                           (f"blobs/sha256/{digest(config)}", config)):
            info = tarfile.TarInfo(name)
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
    return digest(config)


def make_zip(version):
    """An unpacked Frametop.zip for VERSION, and the tree its image holds."""
    unpacked = os.path.join(TMP, f"zip-{version}")
    tree = os.path.join(TMP, f"tree-{version}")
    os.makedirs(unpacked)
    os.makedirs(tree)
    image = os.path.join(unpacked, "frametop-image.tar")
    image_id = oci_archive(image, json.dumps({"architecture": "arm64", "v": version}).encode())
    table = os.path.join(TMP, "steamos.json")
    Path(table).write_text(json.dumps({"tested": [{"build": "20260922.6101926", "version": version}]}))
    out = subprocess.run([sys.executable, os.path.join(REPO, "pack", "release-info.py"), "--image-file", image,
                          "--version", version, "--commit", "1234567", "--steamos", table],
                         capture_output=True, text=True, check=True).stdout
    Path(unpacked, "frametop-release.json").write_text(out)
    Path(tree, "install.sh").write_text('#!/usr/bin/env bash\nset -euo pipefail\nroot=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)\n'
                                        'FRAME_RELEASE=1 FRAME_LOCAL=1\necho "install.sh of $root"\n' + CURRENT_BLOCK)
    os.chmod(os.path.join(tree, "install.sh"), 0o755)
    return unpacked, tree, image_id


ZIPS = {v: make_zip(v) for v in ("1.0", "2.0", "3.0")}


def install(version):
    unpacked, tree, image_id = ZIPS[version]
    env = dict(os.environ, FAKE_ID=image_id, FAKE_TREE=tree, FRAMETOP_STEAMOS_TABLE="none")
    r = subprocess.run(["bash", "-c", SCRIPT + PODMAN + 'main "$@"', "install-release.sh", "--yes", "--dir", RELEASES],
                       cwd=unpacked, env=env, capture_output=True, text=True, stdin=subprocess.DEVNULL, check=False)
    return r.returncode, r.stdout + r.stderr


def current():
    link = os.path.join(RELEASES, "current")
    return os.readlink(link) if os.path.islink(link) else None


def folders():
    return sorted(d for d in os.listdir(RELEASES) if d != "current")


code, out = install("1.0")
check("the first release installs", (code, current(), folders()), (0, "1.0", ["1.0"]))
code, out = install("2.0")
check("the next one is current, and the one before stays", (code, current(), folders()), (0, "2.0", ["1.0", "2.0"]))
code, out = install("3.0")
check("two releases on, the oldest goes; the one current before stays, though install.sh "
      "moved current first (DEE-286)", (code, current(), folders()), (0, "3.0", ["2.0", "3.0"]))

# Going back: the older release's install.sh makes it current, so fast-install and get.sh follow.
r = subprocess.run([os.path.join(RELEASES, "2.0", "install.sh")], capture_output=True, text=True, check=False)
check("running the release before's install.sh makes it current (DEE-286)", (r.returncode, current()), (0, "2.0"))
r = subprocess.run(["bash", os.path.join(RELEASES, "current", "install.sh")], capture_output=True, text=True,
                   check=False)
check("...also through the current link", (r.returncode, current()), (0, "2.0"))
code, out = install("3.0")
check("installing 3.0 again makes it current, and 2.0 stays", (code, current(), folders()), (0, "3.0", ["2.0", "3.0"]))

# A reinstall over fast-installed files is refused, and nothing changes (DEE-289).
os.makedirs(os.path.join(RELEASES, "3.0", ".fast-install"))
Path(RELEASES, "3.0", ".fast-install", "state.json").write_text("{}")
code, out = install("3.0")
check("a reinstall over fast-installed files is refused (DEE-289)", (code, "fast-install undo" in out), (1, True))
check("...and leaves the releases as they were", (current(), folders()), ("3.0", ["2.0", "3.0"]))
os.rename(os.path.join(RELEASES, "3.0", ".fast-install"), os.path.join(TMP, "undone"))
code, out = install("3.0")
check("after fast-install undo, it installs again", (code, current()), (0, "3.0"))

print("FAILED: " + ", ".join(failures) if failures else "all passed", flush=True)
shutil.rmtree(TMP, ignore_errors=True)
sys.exit(1 if failures else 0)
