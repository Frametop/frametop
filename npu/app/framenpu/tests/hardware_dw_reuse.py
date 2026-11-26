"""Opt-in bitwise controls for shared depthwise masks and zero identities."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu import Job, RecoveryRequired
from framenpu.resident_gemm import SSR, _lease
from hardware_hmx import PowerSnapshot, audited_device
from hardware_resident import settled_health
from hardware_landmark import service
from hmx_multi_run import failure_snapshot

FINITE = (
    0,
    0x80000000,
    0x3F000000,
    0xBF000000,
    0x3F800000,
    0xBF800000,
    0x3F800001,
    0xBF800001,
    0x40000000,
    0xC0000000,
)
SPECIAL = (
    0,
    0x80000000,
    1,
    0x80000001,
    0x007FFFFF,
    0x00800000,
    0x7F7FFFFF,
    0xFF7FFFFF,
    0x7F800000,
    0xFF800000,
    0x7F800001,
    0x7FC00000,
    0xFFFFFFFF,
)


def cases():
    rng = random.Random(0x44575231)
    for iteration in range(128):
        a = [FINITE[(i + iteration) % len(FINITE)] for i in range(32)]
        b = [FINITE[(i * 3 + iteration // len(FINITE)) % len(FINITE)] for i in range(32)]
        raw = [
            SPECIAL[(i + iteration) % len(SPECIAL)] if i < 16 else rng.getrandbits(32)
            for i in range(32)
        ]
        yield a, b, raw


def classification(raw):
    def nonfinite(x):
        return (x & 0x7F800000) == 0x7F800000

    return (
        [0xFFFFFFFF if nonfinite(x) or nonfinite(x ^ 0xFFFFFFFF) else 0 for x in raw],
        [0xFFFFFFFF if x & 0x7FFFFFFF == 0 else 0 for x in raw],
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-on-device", action="store_true")
    parser.add_argument("--probe", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if not args.run_on_device or os.geteuid() != 0:
        parser.error("--run-on-device and Frame root are required")
    job = Job.load(args.probe)
    report = {
        "format": "framenpu-depthwise-mask-controls",
        "job_sha256": hashlib.sha256(job.code).hexdigest(),
        "audits": {"restored_jobs": 0, "restored_hmx_jobs": 0},
        "cases": 0,
        "checked_lanes": 0,
        "service_before": service(),
    }
    lease = _lease()
    failed = False
    try:
        with PowerSnapshot() as power:
            before = power.read()
            report["initial_health"] = settled_health(power, before)
            try:
                with audited_device(power, report["audits"])(dispatch="direct") as device:
                    for a, b, raw in cases():
                        flags, zeros = classification(raw)
                        checked = []

                        def validate(result):
                            try:
                                for worker, row in enumerate(result.workers):
                                    if (
                                        not row[0]
                                        or row[0] != result.workers[0][0]
                                        or row[1] != worker
                                        or row[2]
                                        or any(row[3:6])
                                        or row[7] != SSR[worker]
                                        or tuple(row[14:16]) != ((1, 1) if worker == 0 else (0, 0))
                                    ):
                                        raise ValueError("worker/fault/cache mismatch")
                                words = struct.unpack("<128I", result.output)
                                if (
                                    words[0] != 0x44575231
                                    or not 0x87A92000 <= words[1] < 0x87A92000 + len(job.code)
                                    or any(words[2:32])
                                    or list(words[64:96]) != flags
                                    or list(words[96:128]) != zeros
                                    or result.workers[0][8] != 0x87000000
                                    or result.kernel_reference_ticks is None
                                    or not 0 < result.kernel_reference_ticks < 768000
                                ):
                                    raise ValueError("classification/arithmetic/timing mismatch")
                                for i, (x, y) in enumerate(zip(a, b)):
                                    if (
                                        y & 0x7FFFFFFF == 0
                                        and x & 0x7FFFFFFF != 0
                                        and words[32 + i] != x
                                    ):
                                        raise ValueError("x plus signed zero differs")
                                    if x & 0x7FFFFFFF == 0 and words[32 + i] != y:
                                        raise ValueError("signed zero plus y differs")
                                checked.append(True)
                            except Exception as exc:
                                raise RecoveryRequired(
                                    "depthwise mask controls invalid; retain resources"
                                ) from exc

                        device._submit(
                            job,
                            struct.pack("<96I", *(a + b + raw)),
                            opcode=97,
                            params=(1,),
                            output_size=512,
                            use_direct=True,
                            _completion_validator=validate,
                        )
                        assert checked
                        report["cases"] += 1
                        report["checked_lanes"] += 32
                report["final_health"] = settled_health(power, before)
                report["service_after"] = service()
                assert report["service_before"] == report["service_after"]
                report["all_checks_passed"] = True
            except Exception as exc:
                failed = True
                report["error"] = f"{type(exc).__name__}: {exc}"
                report["causes"] = []
                cause = exc.__cause__
                while cause:
                    report["causes"].append(f"{type(cause).__name__}: {cause}")
                    cause = cause.__cause__
                report["after_failure"] = failure_snapshot(power)
    finally:
        lease.close()
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report), flush=True)
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
