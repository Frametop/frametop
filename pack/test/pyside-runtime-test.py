#!/usr/bin/env python3
"""Check the GUI bindings under both Python paths used by the image and distrobox."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys

REPO = Path(__file__).resolve().parents[2]
HELPER = REPO / "pack/pyside-runtime.py"


def probe():
    spec = importlib.util.spec_from_file_location("pyside_runtime", HELPER)
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    manifest = Path("/") / helper.MANIFEST
    if manifest.exists():
        # The helper verifies hashes, ELF dependencies, advertised modules, versions,
        # required Qt modules and the four real backend imports together.
        subprocess.run([sys.executable, str(HELPER), "--verify", str(manifest),
                        "--repo", str(REPO)], check=True, timeout=90)
    else:
        # Full Fedora SDK/source-development containers need no subset manifest.
        helper.imports(REPO)
    print(f"{sys.executable}: Frametop GUI bindings passed", flush=True)


def main():
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
    env.pop("PYTHONPATH", None)
    env.pop("PYTHONHOME", None)
    seen = set()
    for python in (Path(sys.executable), Path("/usr/bin/python3"),
                   Path("/opt/frametop/venv/bin/python")):
        if not python.is_file():
            continue
        # Keep system and venv paths distinct even when their binaries are symlinks
        # to the same executable. Only aliases within one bin directory are skipped.
        directory = python.parent.resolve()
        if directory in seen:
            continue
        seen.add(directory)
        subprocess.run([str(python), str(Path(__file__).resolve()), "--probe"],
                       env=env, check=True, timeout=120)


if __name__ == "__main__":
    if sys.argv[1:] == ["--probe"]:
        probe()
    elif len(sys.argv) == 1:
        main()
    else:
        raise SystemExit("usage: pyside-runtime-test.py")
