"""Build the pinned resident graph modules without opening a device."""

import argparse
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu.compiler import BUILD_FLAGS, SDK, Toolchain, _run, build_job
from framenpu import Job
from framenpu.resident_landmark import JOB_KINDS, CODE, CODE_LAYOUTS

TESTS = ROOT / "tests"
LINK_SOURCES = (
    [SDK / "hmx.S", SDK / "link.ld", SDK / "include/framenpu.h", SDK / "include/fnpu_readback.h"]
    + [
        TESTS / name
        for name in (
            "resident_graph_entry.S",
            "resident_link.ld",
            "resident_graph_link.c",
            "resident_graph.h",
            "resident_graph_gather.h",
            "resident_dw_masks.h",
            "resident_dw_tile.h",
            "resident_fp16.h",
            "landmark_common.h",
        )
    ]
    + [TESTS / f"resident_graph_{name}.c" for name in JOB_KINDS.values()]
)


def _pc_relative(tools, obj, folder, label):
    relocations = _run(["llvm-readobj", "--relocations", str(obj)])
    if any("R_HEX" in line and "PCREL" not in line for line in relocations.splitlines()):
        raise ValueError(f"{label} requires review of an absolute relocation")
    (folder / f"{label}-relocations.txt").write_text(relocations)


def build_linked(output, tools=None, profile_dw=0, dw_reuse=0, dw_row_tile=False, conv_fast=False):
    """Build the slot loader and the one linked operator image; opens no device."""
    if dw_row_tile and (dw_reuse != 6 or profile_dw):
        raise ValueError("row tiling requires --dw-reuse 6 and ordinary graph timing")
    if conv_fast and not dw_row_tile:
        raise ValueError("the fast 1x1 path requires --dw-row-tile (wide reservation)")
    if dw_reuse & 5 == 5:
        raise ValueError("select scalar or broadcast coefficient caching")
    if dw_reuse and profile_dw:
        raise ValueError("reuse builds require ordinary graph timing, not component timer modes")
    tools = tools or Toolchain.discover()
    output.mkdir(parents=True, exist_ok=True)
    flags = [*BUILD_FLAGS, "-fno-jump-tables", "-I", str(SDK / "include")]

    def compile_(folder, index, source, *defines):
        obj = folder / f"{index:02d}-{source.name}.o"
        _run([tools.clang, *flags, *defines, "-c", str(source), "-o", str(obj)])
        return obj

    # Row tiling needs the wide reservation; loader and image must agree on it.
    name = "resident_graph_loader_link_wide_v1" if dw_row_tile else "resident_graph_loader_link_v1"
    code_bytes = CODE_LAYOUTS[name]
    wide = ["-DRG_CODE_WIDE"] if dw_row_tile else []

    def link(folder, script, objects, *options):
        elf, binary = folder / "kernel.elf", folder / "kernel.bin"
        _run([tools.linker, "-T", str(script), *options, *map(str, objects), "-o", str(elf)])
        _run([tools.objcopy, "-O", "binary", "--only-section=.payload", str(elf), str(binary)])
        return binary.read_bytes()

    folder = output / "loader"
    folder.mkdir(parents=True, exist_ok=True)
    objects = [
        compile_(folder, 0, TESTS / "resident_graph_entry.S", "-DRG_LINK_ENTRY"),
        compile_(folder, 1, SDK / "hmx.S"),
        compile_(folder, 2, TESTS / "resident_graph_admin.c", "-DRG_LOADER", *wide),
    ]
    _pc_relative(tools, objects[2], folder, "loader")
    loader = Job(link(folder, SDK / "link.ld", objects), name, True, True)
    loader.save(output / "resident_graph_loader.fjob")

    folder = output / "linked"
    folder.mkdir(parents=True, exist_ok=True)
    objects = [
        compile_(
            folder,
            0,
            TESTS / "resident_graph_link.c",
            *([f"-DRG_DW_PROFILE={profile_dw}"] if profile_dw else []),
            *([f"-DRG_DW_REUSE={dw_reuse}"] if dw_reuse else []),
            *(["-DRG_DW_TILE"] if dw_row_tile else []),
            *wide,
            *(["-DRG_CONV_FAST"] if conv_fast else []),
        ),
        compile_(folder, 1, SDK / "hmx.S"),
    ]
    _pc_relative(tools, objects[0], folder, "linked")
    image = link(
        folder, TESTS / "resident_link.ld", objects, f"--defsym=rg_code_bytes={code_bytes:#x}"
    )
    if not 0 < len(image) <= code_bytes or len(image) % 4:
        raise ValueError("linked resident image size differs from its reservation")
    (output / "resident_graph_linked.bin").write_bytes(image)
    record = {
        "format": "framenpu-resident-link-build",
        "profile_dw": profile_dw,
        "dw_reuse": dw_reuse,
        "dw_row_tile": dw_row_tile,
        "conv_fast": conv_fast,
        "version": 1,
        "code_offset": CODE,
        "code_reservation_bytes": code_bytes,
        "loader": {"bytes": len(loader.code), "sha256": hashlib.sha256(loader.code).hexdigest()},
        "linked": {"bytes": len(image), "sha256": hashlib.sha256(image).hexdigest()},
        "flags": [*BUILD_FLAGS, "-fno-jump-tables"],
        "toolchain": tools.versions(),
        "sources": {
            str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in LINK_SOURCES
        },
        "c_relocations_pc_relative": ["loader", "linked"],
    }
    (output / "link.json").write_text(json.dumps(record, indent=2) + "\n")
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--cached-entry", action="store_true")
    parser.add_argument("--linked", action="store_true")
    parser.add_argument("--profile-dw", type=int, choices=range(1, 13))
    parser.add_argument("--dw-reuse", type=int, choices=(0, 1, 2, 3, 4, 6), default=0)
    parser.add_argument("--dw-row-tile", action="store_true")
    parser.add_argument("--conv-fast", action="store_true")
    args = parser.parse_args()
    if args.dw_row_tile and not args.linked:
        parser.error("--dw-row-tile requires --linked")
    if args.dw_reuse and not args.linked:
        parser.error("--dw-reuse requires --linked")
    if args.profile_dw and not args.linked:
        parser.error("--profile-dw requires --linked")
    if args.linked:
        if args.cached_entry:
            parser.error("--linked builds its own loader entry")
        print(
            json.dumps(
                build_linked(
                    args.output,
                    profile_dw=args.profile_dw or 0,
                    dw_reuse=args.dw_reuse,
                    dw_row_tile=args.dw_row_tile,
                    conv_fast=args.conv_fast,
                ),
                indent=2,
            )
        )
        return
    args.output.mkdir(parents=True, exist_ok=True)
    record = {}
    tools = Toolchain.discover() if args.cached_entry else None
    for name in JOB_KINDS.values():
        source = ROOT / "tests" / f"resident_graph_{name}.c"
        output = args.output / f"resident_graph_{name}.fjob"
        if args.cached_entry:
            folder = args.output / name
            folder.mkdir(parents=True, exist_ok=True)
            objects = []
            for i, src in enumerate((ROOT / "tests/resident_graph_entry.S", SDK / "hmx.S", source)):
                obj = folder / f"{i:02d}-{src.name}.o"
                _run(
                    [
                        tools.clang,
                        *BUILD_FLAGS,
                        "-fno-jump-tables",
                        "-I",
                        str(SDK / "include"),
                        "-c",
                        str(src),
                        "-o",
                        str(obj),
                    ]
                )
                objects.append(str(obj))
                if i == 2:
                    relocations = _run(["llvm-readobj", "--relocations", str(obj)])
                    for line in relocations.splitlines():
                        if "R_HEX" in line and "PCREL" not in line:
                            raise ValueError(
                                "cached C module requires review of an absolute relocation"
                            )
                    (folder / "relocations.txt").write_text(relocations)
            elf, binary = folder / "kernel.elf", folder / "kernel.bin"
            _run([tools.linker, "-T", str(SDK / "link.ld"), *objects, "-o", str(elf)])
            _run([tools.objcopy, "-O", "binary", "--only-section=.payload", str(elf), str(binary)])
            job = Job(binary.read_bytes(), f"resident_graph_{name}_cache_v1", True, True)
            job.save(output)
        else:
            job = build_job(source, output, requires_hmx=True, build_dir=args.output / name)
        record[name] = {"bytes": len(job.code), "sha256": hashlib.sha256(job.code).hexdigest()}
    (args.output / "jobs.json").write_text(json.dumps(record, indent=2) + "\n")
    if args.cached_entry:
        proof = {
            "format": "framenpu-resident-cache-build",
            "version": 1,
            "flags": [*BUILD_FLAGS, "-fno-jump-tables"],
            "toolchain": tools.versions(),
            "entry_sha256": hashlib.sha256(
                (ROOT / "tests/resident_graph_entry.S").read_bytes()
            ).hexdigest(),
            "sources": {
                str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in (
                    SDK / "entry.S",
                    SDK / "hmx.S",
                    SDK / "link.ld",
                    SDK / "include/framenpu.h",
                )
            },
            "module_relocations_verified": list(record),
            "modules": record,
        }
        (args.output / "cache-build.json").write_text(json.dumps(proof, indent=2) + "\n")
    print(json.dumps(record, indent=2))


if __name__ == "__main__":
    main()
