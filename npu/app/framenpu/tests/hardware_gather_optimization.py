"""Opt-in gather bit/bounds qualification and paired baseline/optimized timing."""

import argparse
import hashlib
import itertools
import json
import os
from pathlib import Path
import statistics
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu import Job, JobFailed, RecoveryRequired
from framenpu.resident_gemm import SSR
from hardware_hmx import PowerSnapshot, audited_device
from hardware_resident import settled_health
from hardware_landmark import service
from hmx_multi_run import failure_snapshot

SHAPES = (
    (1, 1, 1),
    (1, 1, 7),
    (1, 2, 14),
    (1, 3, 70),
    (2, 3, 7),
    (2, 7, 7),
    (2, 14, 14),
    (2, 28, 28),
    (2, 56, 56),
    (2, 112, 112),
    (2, 3, 70),
    (3, 7, 7),
)


def cases_for(shape):
    channels, height, width = shape
    result = set()
    for stride, pad, channel in itertools.product((1, 2), (0, 1, 2), {0, channels - 1}):
        oh, ow = (height + stride - 1) // stride, (width + stride - 1) // stride
        last = (ow - 1) // 32 * 32
        tiles = {
            (0, 0),
            (0, last),
            (oh - 1, 0),
            (oh - 1, last),
            (min(1, oh - 1), min(1, ow - 1)),
            (max(0, oh - 2), last),
            (oh, ow),
        }
        for (y, x), ky, kx in itertools.product(sorted(tiles), range(5), range(5)):
            for count in {1, 32, min(32, max(1, ow - x))}:
                result.add((stride, stride, pad, pad, channel, y, x, ky, kx, count))
    return sorted(result)


def source_words(shape):
    edges = (
        0,
        0x80000000,
        1,
        0x007FFFFF,
        0x00800000,
        0x3F800000,
        0x7F7FFFFF,
        0x7F800000,
        0xFF800000,
        0x7FC00001,
        0xFFA54321,
    )
    return tuple(
        edges[i % len(edges)] if i % 3 == 0 else (i * 1664525 + 0x9E3779B9) & 0xFFFFFFFF
        for i in range(shape[0] * shape[1] * shape[2])
    )


def expected_words(shape, source, case):
    _, h, w = shape
    sy, sx, py, px, ch, y, x, ky, kx, count = case
    iy = y * sy - py + ky
    return [
        source[(ch * h + iy) * w + (x + i) * sx - px + kx]
        if i < count and 0 <= iy < h and 0 <= (x + i) * sx - px + kx < w
        else 0
        for i in range(32)
    ]


def payload(shape, source, cases, repeats=1):
    if not 1 <= len(cases) <= 32 or not 1 <= repeats <= 16 or len(cases) * repeats > 32:
        raise ValueError("gather work exceeds probe bounds")
    header = [*shape, len(cases), repeats] + [0] * 27
    raw = struct.pack(f"<{len(source)}I", *source)
    raw = raw.ljust((len(raw) + 127) & ~127, b"\0")
    descriptors = b"".join(struct.pack("<16I", *case, *([0] * 6)) for case in cases)
    return struct.pack("<32I", *header) + raw + descriptors, header


def execute(device, job, shape, source, cases, repeats=1, override=None):
    data, header = payload(shape, source, cases, repeats)
    target = b"".join(struct.pack("<32I", *expected_words(shape, source, case)) for case in cases)
    rejecting = override is not None
    if rejecting:
        changed = bytearray(data)
        struct.pack_into("<I", changed, override[0], override[1])
        data = bytes(changed)
    captured = []

    def validate(result):
        try:
            for worker, row in enumerate(result.workers):
                error = int(rejecting and worker == 0)
                if (
                    not row[0]
                    or row[0] != result.workers[0][0]
                    or row[1] != worker
                    or row[2] != error
                    or any(row[3:6])
                    or row[7] != SSR[worker]
                    or any(row[14:])
                ):
                    raise ValueError("gather worker/fault/SSR differs")
            if (
                result.workers[0][8] != 0x87000000
                or result.kernel_reference_ticks is None
                or not 0 < result.kernel_reference_ticks < 768000
            ):
                raise ValueError("gather EVB/timing differs")
            if rejecting:
                if any(result.output):
                    raise ValueError("rejected gather wrote output")
                captured.append({"rejected": True})
                return
            if result.output[:-128] != target:
                raise ValueError(f"gather bit mismatch in {shape}, {cases}")
            meta = list(struct.unpack("<32I", result.output[-128:]))
            if (
                meta[:16] != header[:16]
                or meta[21] != 0x47415432
                or any(meta[22:])
                or not 0xF0A92000 <= meta[20] < 0xF0A92000 + len(job.code)
            ):
                raise ValueError("gather metadata/PC differs")
            start = meta[16] | meta[17] << 32
            end = meta[18] | meta[19] << 32
            if not 0 < end - start < 768000:
                raise ValueError("gather body timing differs")
            captured.append(
                {
                    "body_ticks": end - start,
                    "wrapper_ms": result.kernel_reference_ticks / 19200,
                    "publication_ms": result.elapsed_ms,
                }
            )
        except BaseException as exc:
            raise RecoveryRequired("gather completion invalid; retain resources") from exc

    try:
        device._submit(
            job,
            data,
            opcode=93,
            output_size=len(cases) * 128 + 128,
            use_direct=True,
            _completion_validator=validate,
        )
        if rejecting:
            raise RecoveryRequired("invalid gather descriptor was accepted")
    except JobFailed:
        if not rejecting or len(captured) != 1:
            raise
    return captured[0]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--run-on-device", action="store_true")
    p.add_argument("--probes", required=True, type=Path)
    p.add_argument("--output", required=True, type=Path)
    args = p.parse_args()
    if not args.run_on_device or os.geteuid() != 0:
        p.error("--run-on-device and Frame root are required")
    record = json.loads((args.probes / "comparison.json").read_text())
    jobs = {name: Job.load(args.probes / (name + ".fjob")) for name in ("baseline", "optimized")}
    for name, job in jobs.items():
        if hashlib.sha256(job.code).hexdigest() != record[name]["sha256"]:
            p.error("comparison job pin differs")
    report = {
        "format": "framenpu-gather-comparison",
        "build": record,
        "shapes": [],
        "timing": [],
        "audits": {"restored_jobs": 0, "restored_hmx_jobs": 0},
        "service_before": service(),
    }
    failed = False
    with PowerSnapshot() as power:
        baseline = power.read()
        report["initial_health"] = settled_health(power, baseline)
        try:
            with audited_device(power, report["audits"])(dispatch="direct") as device:
                source = source_words((2, 7, 7))
                case = (1, 1, 1, 1, 0, 0, 0, 1, 0, 7)
                for name, job in jobs.items():
                    for mutation in (
                        (0, 0),
                        (3 * 4, 33),
                        (4 * 4, 17),
                        (5 * 4, 1),
                        (128 + 512 + 9 * 4, 33),
                    ):
                        execute(device, job, (2, 7, 7), source, [case], override=mutation)
                for shape in SHAPES:
                    source = source_words(shape)
                    cases = cases_for(shape)
                    for at in range(0, len(cases), 32):
                        batch = cases[at : at + 32]
                        for name, job in jobs.items():
                            execute(device, job, shape, source, batch)
                    row = {
                        "shape": shape,
                        "cases_per_job": len(cases),
                        "checked_word_values": len(cases) * 32 * 2,
                    }
                    report["shapes"].append(row)
                    print(json.dumps(row), flush=True)
                profiles = (
                    ("left_7", (2, 7, 7), (1, 1, 2, 2, 1, 3, 0, 2, 0, 7)),
                    ("right_7", (2, 7, 7), (1, 1, 2, 2, 1, 3, 0, 2, 4, 7)),
                    ("left_7_interior_channel", (3, 7, 7), (1, 1, 2, 2, 1, 3, 0, 2, 0, 7)),
                    ("right_7_interior_channel", (3, 7, 7), (1, 1, 2, 2, 1, 3, 0, 2, 4, 7)),
                    ("left_14", (2, 14, 14), (1, 1, 2, 2, 1, 7, 0, 2, 0, 14)),
                    ("left_112", (2, 112, 112), (1, 1, 1, 1, 1, 4, 0, 1, 0, 32)),
                    ("interior_112", (2, 112, 112), (1, 1, 1, 1, 1, 4, 32, 1, 1, 32)),
                    ("stride2_left", (2, 14, 14), (2, 2, 1, 1, 1, 3, 0, 1, 0, 7)),
                    ("first_input", (1, 7, 7), (1, 1, 2, 2, 0, 0, 0, 2, 0, 7)),
                    ("last_input", (1, 7, 7), (1, 1, 2, 2, 0, 6, 0, 2, 2, 7)),
                )
                for label, shape, case in profiles:
                    source = source_words(shape)
                    samples = {name: {n: [] for n in (1, 16)} for name in jobs}
                    for sample in range(12):
                        for n in (1, 16):
                            for name in tuple(jobs) if sample % 2 == 0 else tuple(reversed(jobs)):
                                result = execute(device, jobs[name], shape, source, [case], n)
                                if sample >= 2:
                                    samples[name][n].append(result["body_ticks"])
                    summary = {
                        name: {
                            "ticks_1": statistics.median(s[1]),
                            "ticks_16": statistics.median(s[16]),
                            "ticks_per_extra_gather": (
                                statistics.median(s[16]) - statistics.median(s[1])
                            )
                            / 15,
                        }
                        for name, s in samples.items()
                    }
                    row = {
                        "profile": label,
                        "shape": shape,
                        "case": case,
                        "samples": samples,
                        "summary": summary,
                        "slope_speedup": summary["baseline"]["ticks_per_extra_gather"]
                        / summary["optimized"]["ticks_per_extra_gather"],
                    }
                    report["timing"].append(row)
                    print(json.dumps({k: v for k, v in row.items() if k != "samples"}), flush=True)
            report["final_health"] = settled_health(power, baseline)
            report["service_after"] = service()
            if report["service_after"] != report["service_before"]:
                raise ValueError("DSP service changed")
            report["all_checks_passed"] = True
        except BaseException as exc:
            failed = True
            report["error"] = f"{type(exc).__name__}: {exc}"
            report["causes"] = []
            cause = exc.__cause__
            while cause:
                report["causes"].append(f"{type(cause).__name__}: {cause}")
                cause = cause.__cause__
            report["after_failure"] = failure_snapshot(power)
            report["interpretation"] = "stopped without recovery or restart"
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(
        json.dumps(
            {
                "passed": report.get("all_checks_passed", False),
                "audits": report["audits"],
                "error": report.get("error"),
                "causes": report.get("causes"),
            }
        ),
        flush=True,
    )
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
