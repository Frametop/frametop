"""Build the bounded cached HMX model-cost probe; opens no device."""

import argparse
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu import Job
from framenpu.compiler import BUILD_FLAGS, SDK, Toolchain, _run


def build(output, mode):
    output.mkdir(parents=True, exist_ok=True)
    t = Toolchain.discover()
    objects = []
    sources = [
        ROOT / "tests/resident_graph_entry.S",
        SDK / "hmx.S",
        ROOT / "tests/hmx_model_probe.c",
    ]
    for i, p in enumerate(sources):
        obj = output / f"{i:02d}-{p.name}.o"
        _run(
            [
                t.clang,
                *BUILD_FLAGS,
                "-fno-jump-tables",
                f"-DMODEL_MODE={mode}",
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
    job = Job(binary.read_bytes(), "hmx_model_probe_cache_v1", True, True)
    job.save(output / "probe.fjob")
    (output / "disassembly.txt").write_text(_run(["llvm-objdump", "-d", str(elf)]))
    sources += [
        ROOT / "tests/landmark_common.h",
        SDK / "include/framenpu.h",
        SDK / "include/fnpu_readback.h",
        SDK / "link.ld",
    ]
    r = {
        "format": "framenpu-hmx-model-probe-build",
        "bytes": len(job.code),
        "sha256": hashlib.sha256(job.code).hexdigest(),
        "flags": [*BUILD_FLAGS, "-fno-jump-tables", f"-DMODEL_MODE={mode}"],
        "toolchain": t.versions(),
        "sources": {
            str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources
        },
    }
    (output / "build.json").write_text(json.dumps(r, indent=2) + "\n")
    return r


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--output", required=True, type=Path)
    args = p.parse_args()
    print(json.dumps({mode: build(args.output / str(mode), mode) for mode in range(4)}, indent=2))


if __name__ == "__main__":
    main()
