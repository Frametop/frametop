"""Build isolated cached-entry IEEE arithmetic experiments; opens no device."""

import argparse
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu import Job
from framenpu.compiler import BUILD_FLAGS, SDK, Toolchain, _run

DW_CASES = ("convdwclip_0", "convdwclip_1", "convdwclip_8", "convdwclip_12")


def build(source, output, *, defines=()):
    output.mkdir(parents=True, exist_ok=True)
    tools = Toolchain.discover()
    objects = []
    flags = [*BUILD_FLAGS, "-mhvx-ieee-fp", "-fno-jump-tables", *defines]
    for i, path in enumerate((ROOT / "tests/resident_graph_entry.S", source)):
        obj = output / f"{i:02d}-{path.name}.o"
        _run(
            [
                tools.clang,
                *flags,
                "-I",
                str(SDK / "include"),
                "-I",
                str(ROOT / "tests"),
                "-c",
                str(path),
                "-o",
                str(obj),
            ]
        )
        objects.append(str(obj))
    relocations = _run(["llvm-readobj", "--relocations", objects[-1]])
    if any("R_HEX" in line and "PCREL" not in line for line in relocations.splitlines()):
        raise ValueError("probe has an unreviewed absolute relocation")
    elf, binary = output / "kernel.elf", output / "kernel.bin"
    _run([tools.linker, "-T", str(SDK / "link.ld"), *objects, "-o", str(elf)])
    _run([tools.objcopy, "-O", "binary", "--only-section=.payload", str(elf), str(binary)])
    (output / "disassembly.txt").write_text(_run(["llvm-objdump", "-d", str(elf)]))
    job = Job(binary.read_bytes(), source.stem + "_cache_v1", True, True)
    job.save(output / "probe.fjob")
    sources = [
        source,
        ROOT / "tests/resident_graph_entry.S",
        ROOT / "tests/landmark_common.h",
        SDK / "include/framenpu.h",
        SDK / "include/fnpu_readback.h",
        SDK / "link.ld",
    ]
    record = {
        "format": "framenpu-ieee-probe-build",
        "bytes": len(job.code),
        "sha256": hashlib.sha256(job.code).hexdigest(),
        "flags": flags,
        "toolchain": tools.versions(),
        "sources": {
            str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources
        },
    }
    (output / "build.json").write_text(json.dumps(record, indent=2) + "\n")
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument(
        "--depthwise",
        action="store_true",
        help="build three supported depthwise arithmetic variants",
    )
    args = parser.parse_args()
    if args.depthwise:
        result = {
            name: {
                mode: build(
                    ROOT / "tests/native_depthwise_probe.c",
                    args.output / name / str(mode),
                    defines=(f"-DND_MODE={mode}", f"-DND_CASE={case}"),
                )
                for mode in range(3)
            }
            for case, name in enumerate(DW_CASES)
        }
    else:
        result = build(ROOT / "tests/ieee_arithmetic_probe.c", args.output)
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
