"""Build the resident teacher-tail kernel (tests/teacher_resident_probe.c) into
<output>/rs and the stride-2 depthwise kernel it hands off to
(tests/teacher_dw_probe.c) into <output>/dw; opens no device.

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

KERNELS = {
    "rs": (
        ROOT / "tests/resident_graph_entry.S",
        SDK / "hmx.S",
        ROOT / "tests/teacher_resident_probe.c",
    ),
    "dw": (ROOT / "tests/resident_graph_entry.S", ROOT / "tests/teacher_dw_probe.c"),
}
COMMON = (
    ROOT / "tests/landmark_common.h",
    ROOT / "tests/teacher_tail_layout.py",
    ROOT / "tests/teacher_resident_layout.py",
    ROOT / "tests/hmx_block_layout.py",
    SDK / "include/framenpu.h",
    SDK / "include/fnpu_readback.h",
    SDK / "link.ld",
)


def build_one(output, name, SOURCES):
    PINNED = SOURCES + COMMON
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
    job = Job(binary.read_bytes(), f"teacher_{name}_v1", True, True)
    job.save(output / "probe.fjob")
    record = {
        "format": "framenpu-teacher-tail-build",
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


def build(output):
    return {name: build_one(output / name, name, src) for name, src in KERNELS.items()}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--output", required=True, type=Path)
    args = p.parse_args()
    r = build(args.output)
    print(
        json.dumps(
            {k: {"bytes": v["bytes"], "sha256": v["sha256"]} for k, v in r.items()}, indent=2
        )
    )


if __name__ == "__main__":
    main()
