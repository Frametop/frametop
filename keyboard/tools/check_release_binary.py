#!/usr/bin/env python3
"""Reject unstripped, wrong-architecture, or build-path-leaking release binaries."""
import os
import re
import subprocess
import sys
from pathlib import Path


def validate(binary, forbidden_roots):
    environment = dict(os.environ, LC_ALL="C")
    header = subprocess.check_output(["readelf", "-h", str(binary)], text=True, env=environment)
    if not re.search(r"Machine:\s+AArch64\b", header):
        raise ValueError("release executable is not AArch64")
    sections = subprocess.check_output(["readelf", "-SW", str(binary)], text=True, env=environment)
    if re.search(r"\.(?:z?debug\S*|symtab)\s", sections):
        raise ValueError("release executable still contains debug or static symbol sections")
    content = binary.read_bytes()
    for root in forbidden_roots:
        path = str(Path(root).resolve())
        if path != "/" and path.encode() in content:
            raise ValueError(f"release executable contains a build-machine path: {path}")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit("usage: check_release_binary.py BINARY BUILD_ROOT [OTHER_ROOT ...]")
    try:
        validate(Path(sys.argv[1]), [*sys.argv[2:], Path.home()])
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
    print("Release executable: AArch64, stripped, no supplied build/home paths.")
