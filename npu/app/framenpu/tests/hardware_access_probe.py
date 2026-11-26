"""Opt-in memory-access cost table: uncached DDR, cached alias and VTCM primitives."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu import Device, Job, RecoveryRequired
from framenpu.resident_gemm import SSR, RECORD, _lease
from framenpu.resident_landmark import CONTROL
from hardware_hmx import PowerSnapshot, audited_device
from hardware_resident import settled_health
from hardware_landmark import service
from hmx_multi_run import failure_snapshot

WINDOW = (0x1C1B000, 0x10000)  # cached-alias reads; wider reads stay uncached
PRIMITIVES = {
    0: "empty loop",
    1: "uncached DDR scalar read (128-byte stride)",
    2: "uncached DDR scalar read (one 128-byte block)",
    3: "VTCM scalar read",
    4: "cached-alias DDR scalar read, warm",
    5: "uncached DDR aligned vector load",
    6: "uncached DDR unaligned vector load",
    7: "VTCM aligned vector load",
    8: "cached-alias DDR vector load, warm",
    9: "cached-alias DDR vector load, cold lines",
    10: "uncached job-arena scalar write",
    11: "uncached job-arena aligned vector store",
    12: "uncached job-arena unaligned vector store",
    13: "VTCM aligned vector store",
    14: "VTCM scalar write",
    15: "depthwise tap arithmetic, registers only",
    16: "uncached DDR unaligned load, overlapping 4-byte steps",
    17: "uncached DDR unaligned load, spread over 3 MiB",
    18: "uncached overlapping load + tap arithmetic",
    19: "VTCM load + tap arithmetic",
    20: "uncached job-arena unaligned store, 4-byte steps",
    21: "VTCM unaligned vector load, 4-byte steps",
    22: "stack vector store + reload",
    23: "job-stack scalar read",
    24: "job-stack scalar write",
    25: "framed call and return (two leaf calls inside)",
    26: "VTCM vector store + load of the same vector",
    27: "VTCM two vector stores + overlapping unaligned load",
    28: "VTCM vector store + scalar read of it",
    29: "VTCM vector store + load of another vector",
    30: "vector lane to scalar register (vextract)",
    31: "unaligned DDR span: two aligned loads joined by valign",
    32: "unaligned DDR span: two aligned loads through a VTCM pair",
    33: "VTCM vector store + eight tap computations + load of it",
    34: "reference tick read",
    35: "partial store: VTCM vector + 17 scalar copies to the job arena",
    36: "partial store: one masked aligned vector store to the job arena",
    37: "VTCM scalar write + vector load of that vector",
    38: "VTCM scalar write + that vector copied to the job arena",
    39: "VTCM scalar write + scalar read of it",
    40: "register vector stored to the job arena",
}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--run-on-device", action="store_true")
    p.add_argument("--reads", required=True, type=Path)
    p.add_argument("--stores", required=True, type=Path)
    p.add_argument("--taps", required=True, type=Path)
    p.add_argument("--stack", required=True, type=Path)
    p.add_argument("--unit", required=True, type=Path)
    p.add_argument("--output", required=True, type=Path)
    p.add_argument("--rounds", type=int, default=6)
    args = p.parse_args()
    if not args.run_on_device or os.geteuid() != 0:
        p.error("--run-on-device and Frame root are required")
    jobs = [
        Job.load(args.reads),
        Job.load(args.stores),
        Job.load(args.taps),
        Job.load(args.stack),
        Job.load(args.unit),
    ]
    report = {
        "format": "framenpu-access-probe",
        "jobs": [
            {"bytes": len(j.code), "sha256": hashlib.sha256(j.code).hexdigest()} for j in jobs
        ],
        "audits": {"restored_jobs": 0, "restored_hmx_jobs": 0},
        "samples": [],
        "service_before": service(),
    }
    failed = False
    lease = _lease()
    with PowerSnapshot() as power:
        baseline = power.read()
        report["initial_health"] = settled_health(power, baseline)
        try:
            with Device() as observer:
                if bytes(observer._transport.memory[CONTROL : CONTROL + 8]) == RECORD:
                    raise ValueError("resident refusal record present; not probing")
                before = bytes(observer._transport.memory[WINDOW[0] : WINDOW[0] + WINDOW[1]])
            with audited_device(power, report["audits"])(dispatch="direct") as device:

                def run(kind, count, which=None):
                    which = (
                        (
                            4
                            if kind >= 34
                            else 3
                            if kind >= 23
                            else 2
                            if kind >= 15
                            else int(kind >= 10)
                        )
                        if which is None
                        else which
                    )
                    job = jobs[which]
                    seen = []

                    def validate(result):
                        try:
                            for worker, row in enumerate(result.workers):
                                if (
                                    not row[0]
                                    or row[1] != worker
                                    or row[2]
                                    or any(row[3:6])
                                    or row[7] != SSR[worker]
                                    or tuple(row[14:16]) != ((1, 1) if worker == 0 else (0, 0))
                                ):
                                    raise ValueError("probe completion/cleanup differs")
                            ticks = result.kernel_reference_ticks
                            meta = struct.unpack("<32I", result.output[:128])
                            if (
                                ticks is None
                                or not 0 < ticks < 768000
                                or meta[0] != 0x41435053
                                or not 0x87A92000 <= meta[1] < 0x87A92000 + len(job.code)
                                or meta[2] != kind
                                or meta[3] > count
                                or meta[9] != int(meta[3] < count)
                                or meta[11] != which
                                or meta[10] != int(kind in (4, 8, 9))
                            ):
                                raise ValueError("probe metadata/alias/cleanup differs")
                            body = (meta[6] | meta[7] << 32) - (meta[4] | meta[5] << 32)
                            if not 0 <= body < 768000:
                                raise ValueError("probe timing differs")
                            if which == 3:
                                report["stack_address"] = f"{meta[12]:08x}"
                            seen.append((body, ticks, meta[3]))
                        except BaseException as exc:
                            raise RecoveryRequired("access probe validation failed") from exc

                    device._submit(
                        job,
                        bytes(128),
                        opcode=95,
                        output_size=4352,
                        params=(1, kind, count),
                        use_direct=True,
                        _completion_validator=validate,
                    )
                    return seen[0]

                # A short checked run sizes each primitive for about 10 ms;
                # the native loop independently stops at 20 ms.
                counts = {}
                for kind in PRIMITIVES:
                    body, _, done = run(kind, 64)
                    if done != 64:
                        raise ValueError(f"primitive {kind} exceeds the probe bound")
                    limit = 512 if kind == 9 else 16384
                    high = min(limit, int(192000 * 64 / max(body, 1)) // 256 * 256)
                    if high < 256:
                        raise ValueError(f"primitive {kind} is too slow to fit")
                    counts[kind] = (high // 8, high)
                # The empty loop is measured in both images.
                report["empty_ticks"] = [
                    [run(0, n, which)[0] for n in counts[0]] for which in (0, 1, 2, 3, 4)
                ]
                report["counts"] = counts
                for round_ in range(args.rounds):
                    order = list(PRIMITIVES)
                    if round_ % 2:
                        order.reverse()
                    for kind in order:
                        low, high = counts[kind]
                        pair = [run(kind, n) for n in ((low, high) if round_ % 2 else (high, low))]
                        if round_ % 2 == 0:
                            pair.reverse()
                        if pair[0][2] != low or pair[1][2] != high:
                            raise ValueError(f"primitive {kind} ended early")
                        report["samples"].append(
                            {
                                "kind": kind,
                                "low": low,
                                "high": high,
                                "low_ticks": pair[0][0],
                                "high_ticks": pair[1][0],
                                "max_wrapper_ms": max(pair[0][1], pair[1][1]) / 19200,
                            }
                        )
            with Device() as observer:
                after = bytes(observer._transport.memory[WINDOW[0] : WINDOW[0] + WINDOW[1]])
            if after != before:
                raise ValueError("read-only resident window changed")
            table = []
            empty = None
            for kind, name in PRIMITIVES.items():
                slopes = [
                    (s["high_ticks"] - s["low_ticks"]) / (s["high"] - s["low"]) / 19.2
                    for s in report["samples"]
                    if s["kind"] == kind
                ]
                row = {
                    "kind": kind,
                    "name": name,
                    "us_per_iteration_median": statistics.median(slopes),
                    "us_per_iteration_min": min(slopes),
                    "us_per_iteration_max": max(slopes),
                }
                if kind == 0:
                    empty = row["us_per_iteration_median"]
                row["us_minus_empty_loop"] = row["us_per_iteration_median"] - empty
                table.append(row)
            report["table"] = table
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
        finally:
            lease.close()
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    for row in report.get("table", []):
        print(f"{row['name']:44s} {row['us_minus_empty_loop']:8.4f} us", flush=True)
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
