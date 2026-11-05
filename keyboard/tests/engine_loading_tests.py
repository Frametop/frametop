#!/usr/bin/env python3
"""Run a copied test executable without modules, then with broken dependencies.

All fixtures live in a temporary directory. Installed engines are never moved.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

binary = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="full-keyboard-engines-") as directory:
    root = Path(directory)
    (root / "bin").mkdir()
    probe = root / "bin/language-tests"
    shutil.copy2(binary, probe)
    subprocess.run([str(probe), "--without-engines"], check=True)
    modules = binary.parent / "engines"
    if modules.is_dir():
        shutil.copytree(modules, probe.parent / "engines")
        # Force dlopen's dependency resolution to fail without altering the OS.
        libraries = root / "unavailable"
        libraries.mkdir()
        for name in ("libpyzy-1.0.so.0", "libhangul.so.1"):
            (libraries / name).write_bytes(b"unavailable library test fixture")
        subprocess.run([str(probe), "--without-engines"], check=True,
                       env=dict(os.environ, LD_LIBRARY_PATH=str(libraries)))
        print("Missing modules and unavailable runtime dependencies passed.")
    else:
        print("Missing modules passed; runtime dependency case needs engine modules.")
