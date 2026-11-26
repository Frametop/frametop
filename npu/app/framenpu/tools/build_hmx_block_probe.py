"""Build the fused HMX spatial-block prototype variants; opens no device.

Each variant directory holds probe.fjob, build.json (source pins, macros,
geometry) and disassembly.txt. Variants per geometry: main (8-vector batched
copies, phase timers), copy1 (serial copies), notime (no phase timers, to
measure instrumentation cost) and, where stage-2 weights can stay resident,
noreuse (reload them every block) and nohmx (timing-only: same data movement
with no HMX instruction, separating VTCM work from waits on an in-flight load).
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(ROOT / "tests"))
from framenpu import Job
from framenpu.compiler import BUILD_FLAGS, SDK, Toolchain, _run
from hmx_block_layout import GEOMETRIES

SOURCES = (
    ROOT / "tests/resident_graph_entry.S",
    SDK / "hmx.S",
    ROOT / "tests/hmx_block_probe.c",
)
RESIDENT_SOURCES = SOURCES[:2] + (ROOT / "tests/hmx_resident_block_probe.c",)
PINNED = (
    SOURCES
    + RESIDENT_SOURCES[2:]
    + (
        ROOT / "tests/landmark_common.h",
        ROOT / "tests/hmx_block_layout.py",
        SDK / "include/framenpu.h",
        SDK / "include/fnpu_readback.h",
        SDK / "link.ld",
    )
)
VARIANTS = {
    "main": {"FB_BATCH": 8, "FB_TIMING": 1, "FB_REUSE": 1, "FB_NOHMX": 0},
    "copy1": {"FB_BATCH": 1, "FB_TIMING": 1, "FB_REUSE": 1, "FB_NOHMX": 0},
    "notime": {"FB_BATCH": 8, "FB_TIMING": 0, "FB_REUSE": 1, "FB_NOHMX": 0},
    "noreuse": {"FB_BATCH": 8, "FB_TIMING": 1, "FB_REUSE": 0, "FB_NOHMX": 0},
    "nohmx": {"FB_BATCH": 8, "FB_TIMING": 1, "FB_REUSE": 1, "FB_NOHMX": 1},
    # v2: borrows the FeatureNN worker-0/1 VTCM scratch span (docs/VTCM_MAP.md)
    "res1": {
        "FB_BATCH": 8,
        "FB_TIMING": 1,
        "FB_REUSE": 1,
        "FB_NOHMX": 0,
        "FR_PAIR": 0,
        "FR_RESIDENT": 1,
    },
    "res2": {
        "FB_BATCH": 8,
        "FB_TIMING": 1,
        "FB_REUSE": 1,
        "FB_NOHMX": 0,
        "FR_PAIR": 1,
        "FR_RESIDENT": 1,
    },
    "res2_notime": {
        "FB_BATCH": 8,
        "FB_TIMING": 0,
        "FB_REUSE": 1,
        "FB_NOHMX": 0,
        "FR_PAIR": 1,
        "FR_RESIDENT": 1,
    },
}


def variants():
    for g in GEOMETRIES:
        for name, flags in VARIANTS.items():
            if name == "noreuse" and g.nm * g.no != 1:
                continue
            yield g, name, flags


def build(output, g, variant, flags):
    output.mkdir(parents=True, exist_ok=True)
    t = Toolchain.discover()
    macros = {**g.macros(), **flags}
    defines = [f"-D{k}={v}" for k, v in macros.items()]
    objects = []
    for i, p in enumerate(RESIDENT_SOURCES if flags.get("FR_RESIDENT") else SOURCES):
        obj = output / f"{i:02d}-{p.name}.o"
        _run(
            [
                t.clang,
                *BUILD_FLAGS,
                "-fno-jump-tables",
                *defines,
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
    job = Job(binary.read_bytes(), f"hmx_block_{g.name}_{variant}_v1", True, True)
    job.save(output / "probe.fjob")
    text = _run(["llvm-objdump", "-d", str(elf)])
    (output / "disassembly.txt").write_text(text)
    stack_vectors = len(re.findall(r"vmemu?\((r29|r30)", text))
    if stack_vectors:
        raise ValueError(f"{g.name}/{variant}: {stack_vectors} vector stack accesses")
    record = {
        "format": "framenpu-hmx-block-probe-build",
        "geometry": g.name,
        "variant": variant,
        "macros": macros,
        "bytes": len(job.code),
        "sha256": hashlib.sha256(job.code).hexdigest(),
        "input_bytes": g.input_bytes,
        "output_bytes": g.output_bytes,
        "vector_stack_accesses": stack_vectors,
        "flags": [*BUILD_FLAGS, "-fno-jump-tables", *defines],
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
    summary = {}
    for g, variant, flags in variants():
        r = build(args.output / g.name / variant, g, variant, flags)
        summary[f"{g.name}/{variant}"] = {"bytes": r["bytes"], "sha256": r["sha256"]}
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
