"""Build the HVX FP16 depthwise probe v2 (tests/hvx_dw2_probe.c); opens no device.

Writes probe.fjob, build.json (source pins, size) and disassembly.txt. The
build fails on non-PC-relative relocations or vector stack accesses.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu import Job
from framenpu.compiler import BUILD_FLAGS, SDK, Toolchain, _run

SOURCES = (
    ROOT / "tests/resident_graph_entry.S",
    ROOT / "tests/hvx_dw2_probe.c",
)
PINNED = SOURCES + (
    ROOT / "tests/landmark_common.h",
    ROOT / "tests/hvx_dw_layout.py",
    ROOT / "tests/hmx_block_layout.py",
    SDK / "include/framenpu.h",
    SDK / "include/fnpu_readback.h",
    SDK / "link.ld",
)


def build(output):
    output.mkdir(parents=True, exist_ok=True)
    t = Toolchain.discover()
    objects = []
    for i, p in enumerate(SOURCES):
        obj = output / f"{i:02d}-{p.name}.o"
        _run(
            [
                t.clang,
                *BUILD_FLAGS,
                "-fno-jump-tables",
                "-I",
                str(SDK / "include"),
                "-I",
                str(ROOT / "tests"),
                "-c",
                str(p),
                "-o",
                str(obj),
            ]
        )
        objects.append(str(obj))
    rel = _run(["llvm-readobj", "--relocations", objects[-1]])
    if any("R_HEX" in line and "PCREL" not in line for line in rel.splitlines()):
        raise ValueError("unreviewed relocation")
    elf, binary = output / "kernel.elf", output / "kernel.bin"
    _run([t.linker, "-T", str(SDK / "link.ld"), *objects, "-o", str(elf)])
    _run([t.objcopy, "-O", "binary", "--only-section=.payload", str(elf), str(binary)])
    text = _run(["llvm-objdump", "-d", str(elf)])
    (output / "disassembly.txt").write_text(text)
    stack_vectors = len(re.findall(r"vmemu?\((r29|r30)", text))
    if stack_vectors:
        raise ValueError(f"{stack_vectors} vector stack accesses")
    job = Job(binary.read_bytes(), "hvx_dw_v2", True, True)
    job.save(output / "probe.fjob")
    record = {
        "format": "framenpu-hvx-dw2-probe-build",
        "bytes": len(job.code),
        "sha256": hashlib.sha256(job.code).hexdigest(),
        "flags": [*BUILD_FLAGS, "-fno-jump-tables"],
        "toolchain": t.versions(),
        "sources": {
            str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in PINNED
        },
    }
    (output / "build.json").write_text(json.dumps(record, indent=2) + "\n")
    return record


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--output", required=True, type=Path)
    args = p.parse_args()
    r = build(args.output)
    print(json.dumps({"bytes": r["bytes"], "sha256": r["sha256"]}, indent=2))


if __name__ == "__main__":
    main()
