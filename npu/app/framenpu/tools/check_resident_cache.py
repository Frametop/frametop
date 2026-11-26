"""Verify linked resident cache entries, forwarding targets and code relocations."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu import Job
from framenpu.resident_landmark import JOB_KINDS


def verify_vectors(disassembly, symbols):
    instructions = {
        int(m[1], 16): m[2]
        for line in disassembly.splitlines()
        if (m := re.match(r"^([0-9a-f]+):\s+(.*)$", line))
    }
    if symbols["vectors"] % 256:
        raise ValueError("vector table is not 256-byte aligned")
    for index in range(64):
        label = "fault" if index in (2, 4, 6, 8) else "forward" + str(index)
        if "<" + label + ">" not in instructions[symbols["vectors"] + index * 4]:
            raise ValueError("exception vector target differs")
        if label != "fault":
            row = instructions[symbols[label]]
            if f"r0 = #0x{index:x} ; jump" not in row or "<resident_forward_common>" not in row:
                raise ValueError("forwarded vector index differs")
    common = disassembly.split("<resident_forward_common>:", 1)[1].split("<fault>:", 1)[0]
    if (
        "r0 = asl(r0,#0x2)" not in common
        or "immext(#0x87000000)" not in common
        or "jumpr r0" not in common
        or re.search(r"\br(?:[1-9]|[12][0-9]|3[01])\s*=", common)
    ):
        raise ValueError("firmware forwarding target/register contract differs")


def verify_entry(disassembly, symbols):
    verify_vectors(disassembly, symbols)
    dispatch = disassembly.split("<resident_dispatch>:", 1)[1].split("<frame_job>:", 1)[0]
    target = symbols["frame_job"] - 0x69000000
    constants = [
        int(m[1], 16) & 0xFFFFFFFF for m in re.finditer(r"r1 = ##(-?0x[0-9a-f]+)", dispatch)
    ]
    if target not in constants:
        raise ValueError("cached frame_job tail target differs")
    return target


def verify_relocations(text):
    if any("R_HEX" in row and "PCREL" not in row for row in text.splitlines()):
        raise ValueError("absolute C code/table relocation requires review")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jobs", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    manifest = json.loads((args.jobs / "jobs.json").read_text())
    if set(manifest) != set(JOB_KINDS.values()):
        parser.error("resident module inventory differs")
    proof = {}
    for name in JOB_KINDS.values():
        folder = args.jobs / name
        job = Job.load(args.jobs / f"resident_graph_{name}.fjob")
        if (
            job.name != f"resident_graph_{name}_cache_v1"
            or len(job.code) != manifest[name]["bytes"]
            or hashlib.sha256(job.code).hexdigest() != manifest[name]["sha256"]
            or job.code != (folder / "kernel.bin").read_bytes()
        ):
            raise ValueError("cache module code/contract pin differs")
        elf = folder / "kernel.elf"
        listing = subprocess.check_output(["llvm-nm", "-n", str(elf)], text=True)
        symbols = {
            m[2]: int(m[0], 16)
            for line in listing.splitlines()
            if len(m := line.split()) == 3 and re.fullmatch("[0-9a-f]+", m[0])
        }
        disassembly = subprocess.check_output(["llvm-objdump", "-d", str(elf)], text=True)
        (folder / "disassembly.txt").write_text(disassembly)
        target = verify_entry(disassembly, symbols)
        relocations = subprocess.check_output(
            ["llvm-readobj", "--relocations", str(next(folder.glob("02-*.o")))], text=True
        )
        verify_relocations(relocations)
        proof[name] = {
            "guarded_vectors": [2, 4, 6, 8],
            "forwarded_vectors": 60,
            "vector_table_offset": symbols["vectors"] - 0xF0A92000,
            "cached_target": target,
            "all_C_code_relocations_pc_relative": True,
            "elf_sha256": hashlib.sha256(elf.read_bytes()).hexdigest(),
            "code_sha256": manifest[name]["sha256"],
        }
    args.output.write_text(json.dumps(proof, indent=2) + "\n")
    print(
        "Verified all 11 vector tables, forwarding targets, cached tail targets and C relocations"
    )


if __name__ == "__main__":
    main()
