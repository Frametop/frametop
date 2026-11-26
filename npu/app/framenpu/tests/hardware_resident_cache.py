"""Opt-in cached entry alias, image replacement, rejection and guarded-fault controls."""

import argparse
import hashlib
import json
import os
from pathlib import Path
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


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--run-on-device", action="store_true")
    p.add_argument("--probe-a", required=True, type=Path)
    p.add_argument("--probe-b", required=True, type=Path)
    p.add_argument("--output", required=True, type=Path)
    args = p.parse_args()
    if not args.run_on_device or os.geteuid() != 0:
        p.error("--run-on-device and Frame root are required")
    jobs = [Job.load(args.probe_a), Job.load(args.probe_b)]
    if jobs[0].code == jobs[1].code:
        p.error("replacement images must differ")
    words = [(i * 0x9E3779B9 + 0x80000001) & 0xFFFFFFFF for i in range(32)]
    data = struct.pack("<32I", *words)
    report = {
        "format": "framenpu-resident-cache-controls",
        "jobs": [
            {"bytes": len(j.code), "sha256": hashlib.sha256(j.code).hexdigest()} for j in jobs
        ],
        "audits": {"restored_jobs": 0, "restored_hmx_jobs": 0},
        "samples": [],
        "service_before": service(),
    }
    failed = False
    with PowerSnapshot() as power:
        baseline = power.read()
        report["initial_health"] = settled_health(power, baseline)
        try:
            with audited_device(power, report["audits"])(dispatch="direct") as device:
                # Alternate instruction images and aliases repeatedly; trap in
                # each image, then prove a normal job works after cleanup.
                for iteration in range(4):
                    for mode in (0, 1):
                        for tag in (iteration % 2, 1 - iteration % 2):
                            for fault in (False, True, False):
                                job = jobs[tag]
                                expected = struct.pack(
                                    "<32I", *[v ^ (0xA5A5A5A5 + tag) for v in words]
                                )
                                observed = []

                                def validate(result):
                                    try:
                                        for worker, row in enumerate(result.workers):
                                            if (
                                                not row[0]
                                                or row[0] != result.workers[0][0]
                                                or row[1] != worker
                                                or row[2] != (2 if fault and worker == 0 else 0)
                                                or row[7] != SSR[worker]
                                                or tuple(row[14:16])
                                                != ((mode, mode) if worker == 0 else (0, 0))
                                            ):
                                                raise ValueError("cache return/cleanup differs")
                                            if worker == 0 and fault:
                                                base = 0x87A92000 if mode else 0xF0A92000
                                                if row[3] & 255 != 37 or not base <= row[
                                                    4
                                                ] < base + len(job.code):
                                                    raise ValueError("cache trap origin differs")
                                            elif any(row[3:6]):
                                                raise ValueError("unexpected cache fault")
                                        if (
                                            result.workers[0][8] != 0x87000000
                                            or result.kernel_reference_ticks is None
                                            or not 0 < result.kernel_reference_ticks < 768000
                                        ):
                                            raise ValueError("cache EVB/timing differs")
                                        meta = struct.unpack("<32I", result.output[128:])
                                        base = 0x87A92000 if mode else 0xF0A92000
                                        if (
                                            result.output[:128] != expected
                                            or meta[0] != 0x43414331
                                            or meta[2] != tag
                                            or not base <= meta[1] < base + len(job.code)
                                            or meta[7] != int(fault)
                                            or any(meta[8:])
                                        ):
                                            raise ValueError("cache image/alias/sentinel differs")
                                        start = meta[3] | meta[4] << 32
                                        end = meta[5] | meta[6] << 32
                                        if (
                                            not start
                                            or (not fault and not 0 < end - start < 768000)
                                            or (fault and end)
                                        ):
                                            raise ValueError("cache probe timing differs")
                                        observed.append(
                                            {
                                                "mode": mode,
                                                "tag": tag,
                                                "fault": fault,
                                                "pc": meta[1],
                                                "cleanup": list(result.workers[0][14:16]),
                                                "wrapper_ms": result.kernel_reference_ticks / 19200,
                                                "body_ticks": end - start if not fault else None,
                                            }
                                        )
                                    except BaseException as exc:
                                        raise RecoveryRequired(
                                            "cached entry validation failed; retain resources"
                                        ) from exc

                                try:
                                    device._submit(
                                        job,
                                        data,
                                        opcode=94,
                                        output_size=256,
                                        params=(mode, 0, 0, 0, 0, int(fault)),
                                        use_direct=True,
                                        _completion_validator=validate,
                                    )
                                    if fault:
                                        raise RecoveryRequired("cache trap failed to fault")
                                except JobFailed:
                                    if not fault or len(observed) != 1:
                                        raise
                                report["samples"].append(observed[0])
                for mode in (0, 1, 2):
                    rejected = []

                    def reject_check(result):
                        expected_mode = int(mode == 1)
                        for worker, row in enumerate(result.workers):
                            if (
                                row[2] != int(worker == 0)
                                or any(row[3:6])
                                or row[7] != SSR[worker]
                                or tuple(row[14:16])
                                != ((expected_mode, expected_mode) if worker == 0 else (0, 0))
                            ):
                                raise RecoveryRequired(
                                    "rejected cache request lacks clean completion"
                                )
                        if any(result.output):
                            raise RecoveryRequired("rejected cache request wrote output")
                        rejected.append(True)

                    try:
                        device._submit(
                            jobs[0],
                            data,
                            opcode=94,
                            output_size=256,
                            params=(mode, 1 if mode < 2 else 0),
                            use_direct=True,
                            _completion_validator=reject_check,
                        )
                        raise RecoveryRequired("invalid cache parameters accepted")
                    except JobFailed:
                        if not rejected:
                            raise
                    report["samples"].append({"mode": mode, "rejected": True})
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
