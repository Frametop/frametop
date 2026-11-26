"""Read-only VTCM occupancy survey (build on a workstation, run on the Frame).

The probe (tests/vtcm_survey_probe.c) reads all 8 MiB of VTCM on worker 0 and
returns four folded words per 4 KiB chunk. It writes no VTCM, firmware memory
or device register; its only stores go to its own job output. Running it
repeatedly while gaze/hand tracking are idle or active classifies every chunk
as all-zero, constant or changing. That is the empirical half of the
ownership evidence for a larger VTCM region. Absence of change over a session
is NOT proof of ownership: it must be combined with the static firmware map.

Protections are the audited direct-dispatch ones used by the HMX probes:
per-job restoration of code/header/status/VTCM backup/arena, callback and
HMX power, resident-arena and DSP-service comparison, and stop on any
unexpected result without retry, recovery, gate clearing or service restart.

    python tests/hardware_vtcm_survey.py --build --jobs /path/jobs
    python tests/hardware_vtcm_survey.py --run-on-device --jobs /path/jobs \
        --output survey.json --samples 40 --interval 0.5 --label idle
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(ROOT / "tests"))

MAGIC = 0x53565456
OPCODE = 102
BASE = 0x31000000
CHUNK = 4096
CHUNKS = 2048
META = 256
OUTPUT_BYTES = META + CHUNKS * 16
DUMP_BYTES = 65536
SOURCE = ROOT / "tests/vtcm_survey_probe.c"
ENTRY = ROOT / "tests/resident_graph_entry.S"

# Static firmware map (cdsp.analysis.elf, see docs/VTCM_MAP.md).
STATIC = (
    (0x31000000, 0x310F0000, "featurenn_worker0_scratch"),
    (0x310F0000, 0x311E0000, "featurenn_worker1_scratch"),
    (0x311E0000, 0x316EEA80, "featurenn_model_blob"),
    (0x316EEA80, 0x31700000, "unreferenced_gap"),
    (0x31700000, 0x31776600, "gaze_static"),
    (0x31776600, 0x317D4600, "gaze_work_estimate"),
    (0x317D4600, 0x31800000, "unreferenced_top"),
)


def build(output, source=SOURCE, name="vtcm_survey_v1"):
    from framenpu import Job
    from framenpu.compiler import BUILD_FLAGS, SDK, Toolchain, _run

    output.mkdir(parents=True, exist_ok=True)
    t = Toolchain.discover()
    objects = []
    for i, p in enumerate((ENTRY, source)):
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
    if re.search(r"vmemu?\((r29|r30)", text):
        raise ValueError("vector stack access")
    job = Job(binary.read_bytes(), name, True, True)
    job.save(output / "probe.fjob")
    record = {
        "bytes": len(job.code),
        "sha256": hashlib.sha256(job.code).hexdigest(),
        "sources": {
            str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in (ENTRY, source)
        },
    }
    (output / "build.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps(record, indent=2))


def region(addr):
    for lo, hi, name in STATIC:
        if lo <= addr < hi:
            return name
    return "unknown"


def classify(samples):
    """samples: list of CHUNKS x 4 tuples -> per-chunk class and per-region summary."""
    chunks = []
    for i in range(CHUNKS):
        values = {s[i] for s in samples}
        if len(values) > 1:
            kind = "changing"
        elif next(iter(values))[2] == 0:
            kind = "zero"
        else:
            kind = "constant"
        chunks.append(kind)
    runs, start = [], 0
    for i in range(1, CHUNKS + 1):
        if i == CHUNKS or chunks[i] != chunks[start]:
            lo, hi = BASE + start * CHUNK, BASE + i * CHUNK
            runs.append(
                {
                    "start": f"{lo:#x}",
                    "end": f"{hi:#x}",
                    "kib": (hi - lo) // 1024,
                    "kind": chunks[start],
                    "static": region(lo),
                }
            )
            start = i
    regions = {}
    for lo, hi, name in STATIC:
        a, b = (lo - BASE) // CHUNK, -(-(hi - BASE) // CHUNK)
        kinds = [chunks[i] for i in range(a, b)]
        regions[name] = {k: kinds.count(k) * 4 for k in ("zero", "constant", "changing")}
    return chunks, runs, regions


def run(args):
    from framenpu import Job
    from framenpu.resident_gemm import _lease, RECORD
    from framenpu.resident_landmark import CONTROL, ARENA_START, ARENA_END
    from framenpu import Device
    from hardware_ieee_probe import completion
    from hardware_hmx import PowerSnapshot, audited_device
    from hardware_resident import settled_health
    from hardware_landmark import service
    from hmx_multi_run import failure_snapshot

    job = Job.load(args.jobs / "probe.fjob")
    data = struct.pack("<I", MAGIC) + bytes(124)
    report = {
        "format": "framenpu-vtcm-survey",
        "label": args.label,
        "build": json.loads((args.jobs / "build.json").read_text()),
        "source_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "audits": {"restored_jobs": 0, "restored_hmx_jobs": 0},
        "service_before": service(),
        "samples": [],
    }
    raw = []
    failed = False
    lease = _lease()
    try:
        with PowerSnapshot() as power:
            baseline = power.read()
            report["initial_health"] = settled_health(power, baseline)
            try:
                with Device() as observer:
                    if bytes(observer._transport.memory[CONTROL : CONTROL + 8]) == RECORD:
                        raise ValueError("resident refusal record present")
                    before = bytes(observer._transport.memory[ARENA_START:ARENA_END])
                with audited_device(power, report["audits"])(dispatch="direct") as dev:
                    for offset in args.dump:
                        size = META + DUMP_BYTES
                        res = dev._submit(
                            job,
                            data,
                            opcode=OPCODE,
                            params=(1, 1, offset, 0, 0, 0),
                            output_size=size,
                            use_direct=True,
                            _completion_validator=lambda r: completion(r, job, output_bytes=size),
                        )
                        m = struct.unpack_from("<3I", res.output)
                        if m[0] != MAGIC or m[2] != offset:
                            raise ValueError("dump metadata differs")
                        name = args.output.with_name(f"{args.output.stem}-{BASE + offset:#x}.bin")
                        name.write_bytes(res.output[META:])
                        report.setdefault("dumps", {})[f"{BASE + offset:#x}"] = {
                            "file": name.name,
                            "sha256": hashlib.sha256(res.output[META:]).hexdigest(),
                        }
                    for n in range(args.samples):
                        res = dev._submit(
                            job,
                            data,
                            opcode=OPCODE,
                            params=(1, 0, 0, 0, 0, 0),
                            output_size=OUTPUT_BYTES,
                            use_direct=True,
                            _completion_validator=lambda r: completion(
                                r, job, output_bytes=OUTPUT_BYTES
                            ),
                        )
                        m = struct.unpack_from("<64I", res.output)
                        if (
                            m[0] != MAGIC
                            or m[2] != CHUNKS
                            or not 0x87A92000 <= m[1] < 0x87A92000 + len(job.code)
                            or any(m[7:64])
                        ):
                            raise ValueError("survey metadata differs")
                        words = struct.unpack_from(f"<{CHUNKS * 4}I", res.output, META)
                        raw.append([tuple(words[4 * i : 4 * i + 4]) for i in range(CHUNKS)])
                        body = (m[5] | m[6] << 32) - (m[3] | m[4] << 32)
                        report["samples"].append(
                            {
                                "t": time.time(),
                                "body_us": body / 19.2,
                                "wrapper_ms": res.kernel_reference_ticks / 19200,
                            }
                        )
                        time.sleep(args.interval)
                with Device() as observer:
                    after = bytes(observer._transport.memory[ARENA_START:ARENA_END])
                if after != before:
                    raise ValueError("resident arena changed")
                report["final_health"] = settled_health(power, baseline)
                report["service_after"] = service()
                if report["service_before"] != report["service_after"]:
                    raise ValueError("DSP service changed")
                report["all_checks_passed"] = True
            except BaseException as exc:
                failed = True
                report["error"] = f"{type(exc).__name__}: {exc}"
                report["after_failure"] = failure_snapshot(power)
                report["interpretation"] = "stopped without retry/recovery/restart"
    finally:
        lease.close()
        if raw:
            _, runs, regions = classify(raw)
            report["runs"], report["regions"] = runs, regions
            report["raw_first"] = [list(w) for w in raw[0]]
            report["raw_last"] = [list(w) for w in raw[-1]]
            report["changing_chunks"] = sorted(
                i for i in range(CHUNKS) if len({s[i] for s in raw}) > 1
            )
        args.output.write_text(json.dumps(report, indent=1) + "\n")
    print(
        json.dumps(
            {
                "all_checks_passed": report.get("all_checks_passed", False),
                "error": report.get("error"),
                "regions": report.get("regions"),
            },
            indent=1,
        )
    )
    return int(failed)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--build", action="store_true")
    p.add_argument("--run-on-device", action="store_true")
    p.add_argument("--jobs", required=True, type=Path)
    p.add_argument("--output", type=Path)
    p.add_argument("--samples", type=int, default=40)
    p.add_argument("--interval", type=float, default=0.5)
    p.add_argument("--label", default="unlabelled")
    p.add_argument(
        "--dump",
        type=lambda v: int(v, 0),
        action="append",
        default=[],
        help="VTCM offset (64 KiB aligned) to copy raw before the survey",
    )
    args = p.parse_args()
    if args.build:
        return build(args.jobs)
    if not args.run_on_device or os.geteuid() != 0 or not args.output:
        p.error("--build, or Frame root with --run-on-device and --output")
    if len(args.dump) > 16 or any(o & 0xFFFF or not 0 <= o <= 0x7F0000 for o in args.dump):
        p.error("at most 16 dumps at 64 KiB-aligned offsets below 8 MiB")
    if not 0 <= args.samples <= 400 or not 0.05 <= args.interval <= 10:
        p.error("samples 1..400, interval 0.05..10 s")
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
