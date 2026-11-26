#!/usr/bin/env python3
"""Build the five memory-access cost probe images (cache-v1 entry); opens no device."""

import argparse
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu import Job
from framenpu.compiler import BUILD_FLAGS, SDK, Toolchain, _run

NAMES = ("reads", "stores", "taps", "stack", "unit")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    tools = Toolchain.discover()
    record = {}
    for index, name in enumerate(NAMES):
        folder = args.output / name
        folder.mkdir(parents=True, exist_ok=True)
        objects = []
        for i, source in enumerate(
            (ROOT / "tests/resident_graph_entry.S", ROOT / "tests/resident_access_probe.c")
        ):
            obj = folder / f"{i:02d}-{source.name}.o"
            _run(
                [
                    tools.clang,
                    *BUILD_FLAGS,
                    "-fno-jump-tables",
                    f"-DAP_SET={index}",
                    "-I",
                    str(SDK / "include"),
                    "-I",
                    str(ROOT / "tests"),
                    "-c",
                    str(source),
                    "-o",
                    str(obj),
                ]
            )
            objects.append(str(obj))
        relocations = _run(["llvm-readobj", "--relocations", objects[1]])
        if any("R_HEX" in line and "PCREL" not in line for line in relocations.splitlines()):
            raise ValueError("probe requires review of an absolute relocation")
        elf, binary = folder / "kernel.elf", folder / "kernel.bin"
        _run([tools.linker, "-T", str(SDK / "link.ld"), *objects, "-o", str(elf)])
        _run([tools.objcopy, "-O", "binary", "--only-section=.payload", str(elf), str(binary)])
        job = Job(binary.read_bytes(), f"resident_access_probe_{name}_cache_v1", True, True)
        job.save(args.output / f"access_{name}.fjob")
        record[name] = {"bytes": len(job.code), "sha256": hashlib.sha256(job.code).hexdigest()}
    (args.output / "jobs.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps(record, indent=2))


if __name__ == "__main__":
    main()
