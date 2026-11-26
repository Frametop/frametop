#!/usr/bin/env python3
"""Build the FrameNPU host kernels (src/framenpu/host/fnpu_host.c) as a freestanding
shared library for ctypes.

    python tools/build_host_native.py            # the bundled aarch64 library + manifest
    python tools/build_host_native.py --target x86_64 --output DIR   # a local test build

The bundled build writes src/framenpu/data/host_aarch64.so and host_aarch64.json
(bytes, sha256, source sha256, flags, toolchain). The library has no libc or Python
dependency, so one build serves every Python on the Frame. Needs clang and ld.lld.
"""
import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PKG = ROOT / "src/framenpu"
SOURCE = "host/fnpu_host.c"
FLAGS = (
    "-O2",
    "-std=c11",
    "-fPIC",
    "-shared",
    "-nostdlib",
    "-ffreestanding",
    "-ffp-contract=off",
    "-fno-stack-protector",
    "-fno-unwind-tables",
    "-fno-asynchronous-unwind-tables",
    "-fvisibility=hidden",
    "-Wall",
    "-Werror",
    "-fuse-ld=lld",
    "-Wl,--build-id=none",
    "-Wl,-z,now",
    "-Wl,-z,relro",
    "-Wl,-z,noexecstack",
    "-Wl,--no-undefined",
    "-Wl,-soname,libfnpu_host.so",
)
EXPORTS = (
    "fnpu_abi", "fnpu_read32", "fnpu_nonfinite32", "fnpu_to_f16", "fnpu_from_f16", "fnpu_stem_columns",
    "fnpu_grid_pack", "fnpu_grid_unpack", "fnpu_dense", "fnpu_conv1x1", "fnpu_avgpool",
    "fnpu_hint", "fnpu_conv1x1_f16",
)


def _tool(cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout


def build(target, out):
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    src = PKG / SOURCE
    _tool(["clang", f"--target={target}-linux-gnu", *FLAGS, "-o", str(out), str(src)])
    syms = _tool(["llvm-nm", "-D", "--defined-only", str(out)]).split()
    undefined = _tool(["llvm-nm", "-D", "--undefined-only", str(out)]).split()
    if undefined:
        raise SystemExit(f"library imports symbols: {undefined}")
    missing = [e for e in EXPORTS if e not in syms]
    if missing:
        raise SystemExit(f"library lacks exports: {missing}")
    data = out.read_bytes()
    return {
        "format": "framenpu-host-native",
        "abi": int(re.search(r"fnpu_abi\(void\) \{ return (\d+); \}", src.read_text())[1]),
        "target": f"{target}-linux-gnu",
        "bytes": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "source_sha256": {SOURCE: hashlib.sha256(src.read_bytes()).hexdigest()},
        "compiler_flags": list(FLAGS),
        "exports": list(EXPORTS),
        "toolchain": {
            "clang": _tool(["clang", "--version"]).splitlines()[0].strip(),
            "linker": _tool(["ld.lld", "--version"]).strip(),
        },
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--target", default="aarch64", choices=("aarch64", "x86_64"))
    ap.add_argument("--output", help="directory for a local (non-bundled) build")
    a = ap.parse_args()
    if a.output is None:
        if a.target != "aarch64":
            raise SystemExit("only the aarch64 library is bundled; pass --output")
        lib = PKG / "data/host_aarch64.so"
        record = build(a.target, lib)
        (PKG / "data/host_aarch64.json").write_text(json.dumps(record, indent=2) + "\n")
    else:
        lib = Path(a.output) / f"host_{a.target}.so"
        record = build(a.target, lib)
        (Path(a.output) / f"host_{a.target}.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps({"library": str(lib.relative_to(ROOT) if lib.is_relative_to(ROOT) else lib),
                      "bytes": record["bytes"], "sha256": record["sha256"]}))


if __name__ == "__main__":
    sys.exit(main())
