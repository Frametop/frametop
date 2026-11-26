"""Verify the linked resident loader entry and position-independent operator image."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "src"), str(ROOT / "tools")]
from framenpu.resident_landmark import CODE, CODE_LAYOUTS, SEAL, STATE, load_linked
from check_resident_cache import verify_relocations, verify_vectors

UNCACHED, CACHED = 0xF0000000, 0x87000000


def _constants(text):
    return {int(m[1], 16) for m in re.finditer(r"immext\(#0x([0-9a-f]+)\)", text)} | {
        int(m[1], 16) & 0xFFFFFFFF for m in re.finditer(r"##(-?0x[0-9a-f]+)", text)
    }


def verify_loader(disassembly, symbols):
    """Vectors as in the cache entry; dispatch enters only a sealed image."""
    verify_vectors(disassembly, symbols)
    dispatch = disassembly.split("<resident_dispatch>:", 1)[1].split(">:", 1)[0]
    constants = _constants(dispatch)
    required = {UNCACHED + STATE + 124, SEAL, UNCACHED + CODE, CACHED + CODE}
    if not required <= constants:
        raise ValueError("loader seal/alias constants differ")
    if (
        "cmp.eq(r1,#0x20)" not in dispatch
        or "<frame_job>" not in dispatch
        or dispatch.count("jumpr") != 1
        or "mux(p0,r2,r1)" not in dispatch
    ):
        raise ValueError("loader dispatch contract differs")
    if re.search(r"resident_(stage|publish|cache_cleanup)", disassembly):
        raise ValueError("loader must not publish or clean the code slot")


def verify_image(disassembly, symbols, code_bytes=CODE_LAYOUTS["resident_graph_loader_link_v1"]):
    """Entry at offset zero, direct calls only and no self-referencing addresses."""
    base = UNCACHED + CODE
    if symbols.get("rg_batch") != base:
        raise ValueError("linked entry is not at image offset zero")
    if any(not base <= address < base + code_bytes for address in symbols.values()):
        raise ValueError("linked symbol lies outside the image reservation")
    for line in disassembly.splitlines():
        if re.search(r"\b(callr|icinva|dcinva|dccleaninva|dccleana|dczeroa|rte|trap1)\b", line):
            raise ValueError("linked image contains an indirect call or privileged cache/return op")
        if "jumpr" in line and not re.search(r"jumpr(:n?t)? r31\b", line):
            raise ValueError("linked image contains an indirect jump")
    for value in _constants(disassembly):
        if any(a + CODE <= value < a + CODE + code_bytes for a in (UNCACHED, CACHED)):
            raise ValueError("linked image embeds its own absolute address")


def _symbols(elf):
    listing = subprocess.check_output(["llvm-nm", "-n", str(elf)], text=True)
    return {
        m[2]: int(m[0], 16)
        for line in listing.splitlines()
        if len(m := line.split()) == 3 and re.fullmatch("[0-9a-f]+", m[0]) and m[1] not in "Aa"
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jobs", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    loader, image, _ = load_linked(args.jobs)
    proof = {}
    for name, code, check in (
        ("loader", loader.code, verify_loader),
        (
            "linked",
            image,
            lambda text, symbols: verify_image(text, symbols, CODE_LAYOUTS[loader.name]),
        ),
    ):
        folder = args.jobs / name
        elf = folder / "kernel.elf"
        if code != (folder / "kernel.bin").read_bytes():
            raise ValueError("linked build output differs from its container")
        disassembly = subprocess.check_output(["llvm-objdump", "-d", str(elf)], text=True)
        (folder / "disassembly.txt").write_text(disassembly)
        check(disassembly, _symbols(elf))
        verify_relocations((folder / f"{name}-relocations.txt").read_text())
        sections = subprocess.check_output(["llvm-size", "-A", str(elf)], text=True)
        if any(
            row.split()[0] in (".data", ".bss") and int(row.split()[1])
            for row in sections.splitlines()
            if row.startswith(".")
        ):
            raise ValueError("linked build has writable data")
        proof[name] = {
            "bytes": len(code),
            "code_sha256": hashlib.sha256(code).hexdigest(),
            "elf_sha256": hashlib.sha256(elf.read_bytes()).hexdigest(),
            "all_C_code_relocations_pc_relative": True,
        }
    args.output.write_text(json.dumps(proof, indent=2) + "\n")
    print("Verified loader vectors/dispatch and the position-independent linked image")


if __name__ == "__main__":
    main()
