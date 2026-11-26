"""Build the teacher linked loader and operator image; opens no device.

<output>/rs and <output>/dw are the ordinary cached-entry kernels from
tools/build_teacher_resident.py (the qualification reference). <output>/loader
is the 4 KiB slot loader (tests/resident_graph_entry.S with RG_LINK_ENTRY and
TL_LINK_ENTRY, plus tests/teacher_link_admin.c). <output>/linked is the
operator image for the reservation at 0x1ff0000: tests/teacher_link.c at
offset zero, then tests/teacher_resident_probe.c and tests/teacher_dw_probe.c
compiled as tl_rs and tl_dw. --stride2 enables the resident stride-2 extension
in tl_rs only; --dw-blocks 4 restores four-block coefficient sharing there.
--s2-even (Mercury, with --stride2 only) keeps the even stride-2 phase.
--dw-fast (stride2, 2 dw blocks) unrolls the 3x3 depthwise taps, inlines the
post and holds the overflow flags in a register per op (same arithmetic).
--replay (stride2) runs up to 3 crops per request from one crop's descriptors
(request words 3, 4: crop count, crop stride).
--pw-group N (with --pw-fast) runs the 1x1 HMX tiles of N column blocks
before their posts; 1x1 scratch grows to N * (nk + no) * 16 vectors.
--pw-fast (stride2 only) posts each 1x1 HMX tile with hoisted fields and
register-held overflow flags (same arithmetic).
--pw-profile adds 1x1 phase tick totals in meta words 3..11 (timing only).
--mheads (stride2, no heads/stem) adds kinds 7 (regrid) and 8 (3x3 HMX) for
the Mercury heatmap head.
--s2-direct (with --stride2 only) computes stride-2 outputs directly (no
stride-1 temporary or scalar decimation); results are bit-identical.
The two 4 KiB qualification kernels remain unchanged.

Checks: PC-relative C relocations only, no vector stack accesses, the loader's
vector table and forwarding stubs (as the cache entry), its dispatch constants
and that it never publishes or cleans the slot, the image entry at offset zero,
no indirect calls or jumps other than returns, no cache or exception-return
instructions, no embedded address of the image and no writable data.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "src"), str(ROOT / "tools")]
from framenpu import Job
from framenpu.compiler import BUILD_FLAGS, SDK, Toolchain, _run
import build_teacher_resident as BR
from check_resident_cache import verify_relocations, verify_vectors
from check_resident_link import _constants, _symbols, verify_image

TESTS = ROOT / "tests"
CODE, CODE_BYTES, SEAL_AT = 0x1FF0000, 0x4000, 0x1FF3F80
SEAL = 0x544C4E4B
UNCACHED, CACHED = 0xF0000000, 0x87000000
LOADER_NAME = "teacher_link_loader_v1"
SOURCES = (
    TESTS / "resident_graph_entry.S",
    TESTS / "teacher_link_admin.c",
    TESTS / "teacher_link.c",
    TESTS / "teacher_link.ld",
    TESTS / "teacher_resident_probe.c",
    TESTS / "teacher_dw_probe.c",
    TESTS / "teacher_heads.c",
    TESTS / "teacher_heads_check.h",
    TESTS / "teacher_heads_layout.py",
    TESTS / "teacher_stem_check.h",
    TESTS / "teacher_stem_impl.h",
    TESTS / "teacher_stem_vector.h",
    TESTS / "teacher_stem_layout.py",
    TESTS / "teacher_stem_profile.h",
    TESTS / "teacher_stem_profile.py",
    TESTS / "landmark_common.h",
    TESTS / "teacher_tail_layout.py",
    TESTS / "teacher_resident_layout.py",
    TESTS / "hmx_block_layout.py",
    SDK / "hmx.S",
    SDK / "link.ld",
    SDK / "include/framenpu.h",
    SDK / "include/fnpu_readback.h",
)


def _sha(data):
    return hashlib.sha256(data).hexdigest()


def verify_loader(text, symbols, seal_at=SEAL_AT):
    verify_vectors(text, symbols)
    dispatch = text.split("<resident_dispatch>:", 1)[1].split(">:", 1)[0]
    if not {UNCACHED + seal_at, SEAL, UNCACHED + CODE, CACHED + CODE} <= _constants(dispatch):
        raise ValueError("loader seal/alias constants differ")
    if (
        "<frame_job>" not in dispatch
        or dispatch.count("jumpr") != 1
        or "mux(p0,r2,r1)" not in dispatch
    ):
        raise ValueError("loader dispatch contract differs")
    if re.search(r"resident_(stage|publish|cache_cleanup)", text):
        raise ValueError("loader must not publish or clean the code slot")


def build(
    output,
    *,
    stride2=False,
    dw_blocks=2,
    heads=False,
    stem=False,
    stem_profile=False,
    stem_vector=False,
    stem_gray=False,
    s2_even=False,
    s2_direct=False,
    pw_profile=False,
    pw_fast=False,
    pw_group=1,
    dw_fast=False,
    replay=False,
    mheads=False,
    resident=False,
    res_slots=False,
    cbias=False,
    win_wide=False,
    s2_rev=False,
    multi=False,
    stream=False,
    loader_stamp=False,
    pw_ablate=0,
    clock_probe=False,
    omit_fp=False,
    hmx_inline=False,
    pool=False,
    pool_solo=False,
    pool_diag=None,
    diag_pad=0,
    code_pad=0,
    pool_sp_pad=None,
    pool_prof=False,
    pw_pipe=False,
    tiles=False,
    pw_deep=False,
    pw_lg=0,
    post_batch=False,
    code_kib=16,
):
    if code_kib not in (16, 20):
        raise ValueError("--code-kib is 16 or 20")
    code_bytes = code_kib << 10
    seal_at = CODE + code_bytes - 128
    if post_batch and not (pw_pipe and pool and s2_direct and dw_fast):
        raise ValueError("--post-batch is an option of the pooled --pw-pipe build")
    if pw_lg == 3 and code_kib != 20:
        raise ValueError("--pw-lg 3 needs the 20 KiB reservation (--code-kib 20)")
    if pw_deep and not (tiles and pool):
        raise ValueError("--pw-deep is an option of the pooled --tiles build")
    if tiles and not (resident and pw_pipe and dw_fast):
        raise ValueError("--tiles is an option of the pipelined resident image")
    if pw_ablate and not (0 < pw_ablate < 32 and pw_group > 1 and not pw_profile):
        raise ValueError("--pw-ablate is a timing diagnostic of the unprofiled grouped build (1..15)")
    if res_slots and not resident:
        raise ValueError("--res-slots is an option of --resident")
    if cbias and not res_slots:
        raise ValueError("--cbias is an option of the --res-slots image")
    if win_wide and not res_slots:
        raise ValueError("--win-wide is an option of the --res-slots image")
    if s2_rev and not post_batch:
        raise ValueError("--s2-rev is an option of the --post-batch build")
    if multi and not (res_slots and pool and replay) or multi and pool_diag:
        raise ValueError("--multi is an option of the pooled --res-slots replay build")
    if stream and not multi:
        raise ValueError("--stream is an option of the --multi build")
    if resident and not (replay and mheads):
        raise ValueError("resident parameters are an option of the replay + mheads image")
    if mheads and (not stride2 or heads or stem):
        raise ValueError("the Mercury heatmap head is a stride2 build without heads/stem")
    if replay and (not stride2 or heads or stem):
        raise ValueError("crop replay is a stride2 build without heads/stem")
    if dw_fast and (not stride2 or heads or stem or dw_blocks != 2):
        raise ValueError("the fast depthwise is the stride2, 2-block build")
    if pw_group != 1 and (not pw_fast or pw_group not in (2, 4, 8)):
        raise ValueError("--pw-group is 2, 4 or 8, on a --pw-fast build")
    if pw_fast and (not stride2 or heads or stem):
        raise ValueError("the fast 1x1 post is stride2-only")
    if pw_profile and (not stride2 or heads or stem):
        raise ValueError("the 1x1 profile is a stride2-only instrument")
    if s2_direct and (not stride2 or heads or stem or dw_blocks != 2):
        raise ValueError("direct stride 2 is the Mercury-style build: stride2 only")
    if s2_even and (not stride2 or heads or stem or dw_blocks != 2):
        raise ValueError("even-phase stride 2 is the Mercury build: stride2 only")
    if stem_gray and (not stem or stem_profile):
        raise ValueError("grayscale requires the stem build without profiling")
    if stem_vector and not stem:
        raise ValueError("vector stitching requires the stem linked build")
    if stem_profile and not stem:
        raise ValueError("stem profiling requires the stem linked build")
    if stem and not heads:
        raise ValueError("resident stem requires the heads linked build")
    if heads and not stride2:
        raise ValueError("resident heads require the stride-2 linked build")
    if dw_blocks not in (2, 4) or (dw_blocks == 4 and not stride2):
        raise ValueError("four-block depthwise requires the stride-2 linked build")
    if pw_pipe and not (resident and mheads and pw_group > 1):
        raise ValueError("--pw-pipe is an option of the grouped resident heads build")
    if pool_diag not in (None, "wrap", "loader", "tail") or (pool_diag and pool):
        raise ValueError("--pool-diag is wrap or loader, without --pool")
    if pool_sp_pad is None:
        # rs_job's frame in the --pool build is 8 bytes smaller than the plain
        # build's frame_job; the 1x1 post is ~60 us/crop slower at any other depth
        pool_sp_pad = 8 if pool else 0
        if multi:  # rs_job(c, seg) keeps three more words: its frame is the plain depth already
            pool_sp_pad = 0
    if pool_solo and not pool:
        raise ValueError("--pool-solo is a diagnostic of the --pool build")
    tools = Toolchain.discover()
    record = {"format": "framenpu-teacher-link-build", "reference": BR.build(output)}

    def compile_(folder, index, source, *defines):
        obj = folder / f"{index:02d}-{source.name}.o"
        _run(
            [
                tools.clang,
                *BUILD_FLAGS,
                "-fno-jump-tables",
                *(["-fomit-frame-pointer"] if omit_fp and folder.name != "loader" else []),
                *defines,
                "-I",
                str(SDK / "include"),
                "-I",
                str(TESTS),
                "-c",
                str(source),
                "-o",
                str(obj),
            ]
        )
        if source.suffix == ".c":
            rel = _run(["llvm-readobj", "--relocations", str(obj)])
            verify_relocations(rel)
            (folder / f"{index:02d}-relocations.txt").write_text(rel)
        return obj

    def link(folder, script, objects, check):
        elf, binary = folder / "kernel.elf", folder / "kernel.bin"
        _run([tools.linker, "-T", str(script), *map(str, objects), "-o", str(elf)])
        _run([tools.objcopy, "-O", "binary", "--only-section=.payload", str(elf), str(binary)])
        text = subprocess.check_output(["llvm-objdump", "-d", str(elf)], text=True)
        (folder / "disassembly.txt").write_text(text)
        if re.search(r"vmemu?\((r29|r30)", text):
            raise ValueError(f"{folder.name}: vector stack accesses")
        sections = subprocess.check_output(["llvm-size", "-A", str(elf)], text=True)
        if any(
            r.split()[0] in (".data", ".bss", ".sdata", ".sbss") and int(r.split()[1])
            for r in sections.splitlines()
            if r.startswith(".")
        ):
            raise ValueError(f"{folder.name}: writable data")
        check(text, _symbols(elf))
        return binary.read_bytes()

    folder = output / "loader"
    folder.mkdir(parents=True, exist_ok=True)
    objects = [
        compile_(folder, 0, TESTS / "resident_graph_entry.S", "-DRG_LINK_ENTRY", "-DTL_LINK_ENTRY",
                 *(("-DRG_STAMP_RESTORED",) if loader_stamp else ()),
                 *(("-DRG_POOL",) if pool or pool_diag == "loader" else ()),
                 *((f"-DTL_SEAL_VA={UNCACHED + seal_at:#x}",) if code_kib != 16 else ())),
        compile_(folder, 1, SDK / "hmx.S"),
        compile_(folder, 2, TESTS / "teacher_link_admin.c",
                 *((f"-DTL_CODE_BYTES={code_bytes:#x}u",) if code_kib != 16 else ())),
    ]
    code = link(folder, SDK / "link.ld", objects, lambda t, s: verify_loader(t, s, seal_at))
    if len(code) > 4096:
        raise ValueError("loader exceeds the 4 KiB slot")
    loader = Job(code, LOADER_NAME, True, True)
    loader.save(output / "teacher_link_loader.fjob")

    folder = output / "linked"
    folder.mkdir(parents=True, exist_ok=True)
    objects = [
        compile_(folder, 0, TESTS / "teacher_link.c", *(("-DRS_POOL",) if pool or pool_diag == "loader" else ()),
                 *((f"-DTL_DIAG_CODE_PAD={code_pad}",) if code_pad else ())),
        compile_(
            folder,
            1,
            TESTS / "teacher_resident_probe.c",
            "-Dframe_job=tl_rs",
            *(["-DRS_STRIDE2"] if stride2 else []),
            *(["-DRS_DW_BLOCKS=4"] if dw_blocks == 4 else []),
            *(["-DRS_HEADS"] if heads else []),
            *(["-DRS_STEM"] if stem else []),
            *(["-DRS_STEM_PROFILE"] if stem_profile else []),
            *(["-DRS_STEM_VECTOR"] if stem_vector else []),
            *(["-DRS_STEM_GRAY"] if stem_gray else []),
            *(["-DRS_S2_EVEN"] if s2_even else []),
            *(["-DRS_S2_DIRECT"] if s2_direct else []),
            *(["-DRS_PW_PROFILE"] if pw_profile else []),
            *(["-DRS_PW_FAST"] if pw_fast else []),
            *([f"-DRS_PW_GROUP={pw_group}"] if pw_group > 1 else []),
            *(["-DRS_DW_FAST"] if dw_fast else []),
            *(["-DRS_REPLAY"] if replay else []),
            *(["-DRS_MHEADS"] if mheads else []),
            *(["-DRS_RESIDENT"] if resident else []),
            *(["-DRS_RES_SLOTS"] if res_slots else []),
            *(["-DRS_CBIAS"] if cbias else []),
            *(["-DRS_WIN_WIDE"] if win_wide else []),
            *(["-DRS_S2_REV"] if s2_rev else []),
            *(["-DRS_MULTI"] if multi else []),
            *(["-DRS_STREAM"] if stream else []),
            *([f"-DRS_PW_ABLATE={pw_ablate}"] if pw_ablate else []),
            *(["-DRS_CLOCK_PROBE"] if clock_probe else []),
            *(["-DRS_PW_PIPE"] if pw_pipe else []),
            *(["-DRS_HMX_INLINE"] if hmx_inline else []),
            *(["-DRS_POOL"] if pool else []),
            *(["-DRS_POOL_SOLO"] if pool_solo else []),
            *([f"-DRS_POOL_SP_PAD={pool_sp_pad}"] if pool_sp_pad else []),
            *(["-DRS_POOL_PROF"] if pool_prof else []),
            *(["-DRS_TILES"] if tiles else []),
            *(["-DRS_PW_DEEP"] if pw_deep else []),
            *([f"-DRS_PW_LG={pw_lg}u"] if pw_lg else []),
            *(["-DRS_POST_BATCH"] if post_batch else []),
            *({"wrap": ["-DRS_DIAG_WRAP"], "loader": ["-DRS_DIAG_LOADER"], "tail": ["-DRS_DIAG_TAIL"]}.get(pool_diag, [])),
            *([f"-DRS_DIAG_PAD={diag_pad}"] if diag_pad else []),
        ),
        compile_(folder, 2, TESTS / "teacher_dw_probe.c", "-Dframe_job=tl_dw"),
        compile_(folder, 3, SDK / "hmx.S"),
    ]
    if heads:
        objects.append(compile_(folder, 4, TESTS / "teacher_heads.c"))

    def check_image(text, symbols):
        if symbols.get("tl_entry") != UNCACHED + CODE:
            raise ValueError("image entry is not at offset zero")
        verify_image(text, symbols | {"rg_batch": UNCACHED + CODE}, seal_at - CODE)

    script = TESTS / "teacher_link.ld"
    if code_kib != 16:  # the same script with the payload bound moved below the new seal vector
        ld = script.read_text()
        if ld.count("<= 0x3f80,") != 1:
            raise ValueError("teacher_link.ld payload bound differs")
        script = folder / "teacher_link.ld"
        script.write_text(ld.replace("<= 0x3f80,", f"<= {seal_at - CODE:#x},"))
    image = link(folder, script, objects, check_image)
    (output / "teacher_link.bin").write_bytes(image)
    record |= {
        "stride2": stride2,
        "dw_blocks": dw_blocks,
        "heads": heads,
        "stem": stem,
        "stem_profile": stem_profile,
        "stem_vector": stem_vector,
        "stem_gray": stem_gray,
        "s2_even": s2_even,
        "s2_direct": s2_direct,
        "pw_profile": pw_profile,
        "pw_fast": pw_fast,
        "pw_group": pw_group,
        "dw_fast": dw_fast,
        "replay": replay,
        "mheads": mheads,
        "resident": resident,
        **({"res_slots": 4} if res_slots else {}),
        **({"cbias": True} if cbias else {}),
        **({"win_hi": "0x1ff0000"} if win_wide else {}),
        **({"s2_rev": True} if s2_rev else {}),
        **({"multi": 2} if multi else {}),
        **({"stream": True} if stream else {}),
        **({"pw_ablate": pw_ablate} if pw_ablate else {}),
        **({"clock_probe": True} if clock_probe else {}),
        **({"pw_pipe": True} if pw_pipe else {}),
        **({"hmx_inline": True} if hmx_inline else {}),
        **({"pool": True} if pool else {}),
        **({"pool_solo": True} if pool_solo else {}),
        **({"pool_diag": pool_diag} if pool_diag else {}),
        **({"diag_pad": diag_pad} if diag_pad else {}),
        **({"code_pad": code_pad} if code_pad else {}),
        **({"pool_sp_pad": pool_sp_pad} if pool_sp_pad else {}),
        **({"clock_probe": "pool"} if pool_prof else {}),
        **({"tiles": True} if tiles else {}),
        **({"pw_deep": True} if pw_deep else {}),
        **({"pw_lg": pw_lg} if pw_lg else {}),
        **({"post_batch": True} if post_batch else {}),
        "loader": {"bytes": len(loader.code), "sha256": _sha(loader.code), "name": LOADER_NAME},
        "linked": {"bytes": len(image), "sha256": _sha(image)},
        "reservation": {"code": f"{CODE:#x}", "bytes": code_bytes, "seal_vector": f"{seal_at:#x}"},
        "flags": [*BUILD_FLAGS, "-fno-jump-tables", *(["-fomit-frame-pointer"] if omit_fp else [])],
        "toolchain": tools.versions(),
        "sources": {
            str(p.relative_to(ROOT if p.is_relative_to(ROOT) else SDK.parent)): _sha(p.read_bytes())
            for p in SOURCES
        },
    }
    (output / "link.json").write_text(json.dumps(record, indent=2) + "\n")
    return record


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--output", required=True, type=Path)
    p.add_argument(
        "--stride2", action="store_true", help="enable merged resident stride-2 sequences"
    )
    p.add_argument("--dw-blocks", type=int, choices=(2, 4), default=2)
    p.add_argument("--heads", action="store_true")
    p.add_argument("--stem", action="store_true")
    p.add_argument("--stem-profile", action="store_true")
    p.add_argument("--stem-vector", action="store_true")
    p.add_argument("--stem-gray", action="store_true")
    p.add_argument("--s2-even", action="store_true", help="Mercury: even stride-2 phase")
    p.add_argument("--s2-direct", action="store_true", help="direct stride-2 depthwise")
    p.add_argument("--pw-profile", action="store_true", help="timing instrument: 1x1 phase ticks")
    p.add_argument("--pw-fast", action="store_true", help="1x1 post with register-held flags")
    p.add_argument("--pw-group", type=int, default=1, help="1x1 column blocks per HMX group")
    p.add_argument("--dw-fast", action="store_true", help="unrolled depthwise, inlined post")
    p.add_argument(
        "--replay", action="store_true", help="up to 3 crops per request, one descriptor table"
    )
    p.add_argument(
        "--mheads", action="store_true", help="Mercury heatmap head: regrid and 3x3 HMX kinds"
    )
    p.add_argument(
        "--resident", action="store_true",
        help="parameter images resident in the FeatureNN blob region (replay + mheads)",
    )
    p.add_argument(
        "--loader-stamp", action="store_true",
        help="timing instrument: worker 0 stamps c31:30 around the VTCM backup and restore (status words 14-15)",
    )
    p.add_argument("--pw-ablate", type=int, default=0,
                   help="DIAGNOSTIC (wrong outputs): skip 1x1 phases, 1 HMX loads, 2 all HMX, 4 gather, 8 post, 16 group barrier")
    p.add_argument("--clock-probe", action="store_true",
                   help="DIAGNOSTIC: core cycles, packets and timer ticks per request in meta[3..8]")
    p.add_argument("--omit-fp", action="store_true",
                   help="no frame pointers in the linked image (the loader is unchanged): leaf calls stop storing frames to the DDR stack, which "
                        "every following barrier waits ~1.9 us for")
    p.add_argument("--pool", action="store_true",
                   help="workers 1 and 2 share the depthwise ops (loader and image; resident replay build)")
    p.add_argument("--pool-diag", choices=("wrap", "loader", "tail"),
                   help="DIAGNOSTIC (same outputs): one pool ingredient alone (the frame_job wrapper frame, "
                        "or the pool loader with workers returning at once)")
    p.add_argument("--tiles", action="store_true",
                   help="1x1 inputs, outputs and residuals in B-tile layout where the plan marks them (no gather)")
    p.add_argument("--code-kib", type=int, choices=(16, 20), default=16,
                   help="image reservation size: 20 takes the next page, 0x1ff4000 (needs the matching loader)")
    p.add_argument("--pw-lg", type=int, choices=(1, 2, 3), default=0,
                   help="with --pw-deep: log2 of the column blocks per group of the deep ops (default 2)")
    p.add_argument("--pw-deep", action="store_true",
                   help="with --pool --tiles: tile-input 1x1 ops use their whole scratch for up to 4 output slots per worker")
    p.add_argument("--post-batch", action="store_true",
                   help="1x1 post: load each 4 vectors' operands before storing any (same outputs)")
    p.add_argument("--pool-prof", action="store_true",
                   help="DIAGNOSTIC: pooled 1x1 wait/work tick totals in meta[3..8] (recorded as a clock probe)")
    p.add_argument("--pool-sp-pad", type=int, default=None,
                   help="with --pool: bytes added to rs_job's frame (default 8: the plain build's stack depth)")
    p.add_argument("--code-pad", type=int, default=0,
                   help="DIAGNOSTIC: bytes of padding after the image entry (moves all other code)")
    p.add_argument("--diag-pad", type=int, default=0,
                   help="DIAGNOSTIC: bytes the frame_job wrapper moves the stack down (needs a wrapper build)")
    p.add_argument("--pool-solo", action="store_true",
                   help="DIAGNOSTIC (same outputs): with --pool, worker 0 runs every share; workers leave at once")
    p.add_argument("--res-slots", action="store_true",
                   help="(--resident) four residency tag slots: models resident side by side")
    p.add_argument("--cbias", action="store_true",
                   help="(--res-slots) packed biases, expanded per op at the top of the blob region")
    p.add_argument("--win-wide", action="store_true",
                   help="(--res-slots) window up to the image reservation at 0x1ff0000 (5.69 MiB)")
    p.add_argument("--multi", action="store_true",
                   help="(--res-slots --pool) requests of one or two segments (two models' tables)")
    p.add_argument("--stream", action="store_true",
                   help="(--multi) segment 1 starts when the host has staged it; segments publish completion")
    p.add_argument("--s2-rev", action="store_true",
                   help="(--post-batch) stride-2 column blocks last to first (rows wider than 32)")
    p.add_argument("--hmx-inline", action="store_true",
                   help="HMX sequences inline in the resident operators (same instructions, no calls)")
    p.add_argument("--pw-pipe", action="store_true",
                   help="overlapped 1x1 gather/post loops (same operations; grouped resident heads build)")
    args = p.parse_args()
    r = build(
        args.output,
        stride2=args.stride2,
        dw_blocks=args.dw_blocks,
        heads=args.heads,
        stem=args.stem,
        stem_profile=args.stem_profile,
        stem_vector=args.stem_vector,
        stem_gray=args.stem_gray,
        s2_even=args.s2_even,
        s2_direct=args.s2_direct,
        pw_profile=args.pw_profile,
        pw_fast=args.pw_fast,
        pw_group=args.pw_group,
        dw_fast=args.dw_fast,
        replay=args.replay,
        mheads=args.mheads,
        resident=args.resident,
        res_slots=args.res_slots,
        cbias=args.cbias,
        win_wide=args.win_wide,
        s2_rev=args.s2_rev,
        multi=args.multi,
        stream=args.stream,
        loader_stamp=args.loader_stamp,
        pw_ablate=args.pw_ablate,
        clock_probe=args.clock_probe,
        omit_fp=args.omit_fp,
        hmx_inline=args.hmx_inline,
        pool=args.pool,
        pool_solo=args.pool_solo,
        pool_diag=args.pool_diag,
        diag_pad=args.diag_pad,
        code_pad=args.code_pad,
        pool_sp_pad=args.pool_sp_pad,
        pool_prof=args.pool_prof,
        tiles=args.tiles,
        pw_deep=args.pw_deep,
        pw_lg=args.pw_lg,
        code_kib=args.code_kib,
        post_batch=args.post_batch,
        pw_pipe=args.pw_pipe,
    )
    print(
        json.dumps(
            {k: r[k] for k in ("loader", "linked")}
            | {k: {"bytes": v["bytes"], "sha256": v["sha256"]} for k, v in r["reference"].items()},
            indent=1,
        )
    )


if __name__ == "__main__":
    main()
