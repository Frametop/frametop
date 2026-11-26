"""Opt-in exact resident-gather primitive tests in the borrowed job arena."""

import argparse
import hashlib
import itertools
import json
import os
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu import Job, RecoveryRequired
from hardware_hmx import PowerSnapshot, audited_device
from hardware_resident import settled_health


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--run-on-device", action="store_true")
    p.add_argument("--probe", required=True, type=Path)
    p.add_argument("--output", required=True, type=Path)
    args = p.parse_args()
    if not args.run_on_device or os.geteuid() != 0:
        p.error("--run-on-device and Frame root are required")
    job = Job.load(args.probe)
    source = [(i - 217) / 8 for i in range(420)]
    report = {
        "probe_sha256": hashlib.sha256(job.code).hexdigest(),
        "cases": 0,
        "audits": {"restored_jobs": 0, "restored_hmx_jobs": 0},
    }
    with PowerSnapshot() as power:
        baseline = power.read()
        report["initial_health"] = settled_health(power, baseline)
        with audited_device(power, report["audits"])(dispatch="direct") as device:
            cases = itertools.product(
                (1, 16, 31, 32), (1, 2), (0, 1, 16, 30, 35, 68, 70), range(3), range(3), range(3)
            )
            for count, stride, x, y, ky, kx in cases:
                iy = y * stride - 1 + ky
                expected = [0.0] * 32
                for i in range(count):
                    ix = (x + i) * stride - 1 + kx
                    if 0 <= iy < 3 and 0 <= ix < 70:
                        expected[i] = source[210 + iy * 70 + ix]
                target = struct.pack("<32f", *expected)

                def validate(result):
                    if result.output != target or any(
                        row[2] or any(row[3:6]) for row in result.workers
                    ):
                        raise RecoveryRequired(f"gather differs: {(count, stride, x, y, ky, kx)}")

                device._submit(
                    job,
                    struct.pack("<420f", *source),
                    opcode=92,
                    output_size=128,
                    params=(count, stride, x, y, ky, kx),
                    use_direct=True,
                    _completion_validator=validate,
                )
                report["cases"] += 1
        report["final_health"] = settled_health(power, baseline)
    report["all_checks_passed"] = True
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(
        json.dumps({"passed": True, "cases": report["cases"], "audits": report["audits"]}),
        flush=True,
    )


if __name__ == "__main__":
    main()
