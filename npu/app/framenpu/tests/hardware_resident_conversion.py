"""Opt-in resident-packing primitive qualification, with no resident writes."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu import Job
from hardware_hmx import PowerSnapshot, audited_device
from hardware_resident import settled_health


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--run-on-device", action="store_true")
    p.add_argument("--probe", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    args = p.parse_args()
    if not args.run_on_device or os.geteuid() != 0:
        p.error("--run-on-device and Frame root are required")
    import numpy as np

    job = Job.load(args.probe)
    # Every finite binary16 pattern including signed zero, plus every positive
    # and negative half midpoint and adjacent binary32 neighbors, plus random
    # finite binary32 words across all exponents (including overflow/underflow).
    bits = np.arange(65536, dtype=np.uint16)
    finite = (bits & 0x7C00) != 0x7C00
    values = bits[finite].view(np.float16).astype(np.float32)
    positive = np.arange(0x7BFF + 1, dtype=np.uint16).view(np.float16).astype(np.float32)
    mid = (positive[:-1] + positive[1:]) * np.float32(0.5)
    boundaries = np.stack(
        (np.nextafter(mid, np.float32(-np.inf)), mid, np.nextafter(mid, np.float32(np.inf))), axis=1
    ).ravel()
    random_bits = np.random.default_rng(0x524C4D4B).integers(0, 2**32, 131072, dtype=np.uint32)
    random_bits = random_bits[(random_bits & 0x7F800000) != 0x7F800000]
    source = np.concatenate(
        (
            values,
            boundaries,
            -boundaries,
            random_bits.view(np.float32),
            np.array([65519.996, 65520.0, 65520.004, -65520.0, 1e30, -1e30], dtype=np.float32),
        )
    )
    report = {
        "probe_sha256": hashlib.sha256(job.code).hexdigest(),
        "values": len(source),
        "audits": {"restored_jobs": 0, "restored_hmx_jobs": 0},
        "chunks": [],
    }
    with np.errstate(over="ignore", invalid="ignore"):
        expected = source.astype(np.float16).view(np.uint16)
    with PowerSnapshot() as power:
        baseline = power.read()
        report["initial_health"] = settled_health(power, baseline)
        with audited_device(power, report["audits"])(dispatch="direct") as device:
            for start in range(0, len(source), 8192):
                count = min(8192, len(source) - start)
                padded = ((count + 63) // 64) * 64
                data = np.zeros(padded, dtype=np.float32)
                data[:count] = source[start : start + count]
                with np.errstate(over="ignore", invalid="ignore"):
                    target = (
                        data.astype(np.float16)
                        .view(np.uint16)
                        .reshape(-1, 2, 32)
                        .transpose(0, 2, 1)
                        .ravel()
                        .tobytes()
                    )

                # Completion validation deliberately occurs before reclamation.
                def validate(result):
                    from framenpu import RecoveryRequired

                    try:
                        if result.output != target:
                            raise ValueError("integer FP32-to-FP16 conversion/order differs")
                        if any(row[2] or any(row[3:6]) for row in result.workers):
                            raise ValueError("conversion native fault")
                    except BaseException as exc:
                        raise RecoveryRequired("resident conversion validation failed") from exc

                result = device._submit(
                    job,
                    data.tobytes(),
                    output_size=padded * 2,
                    opcode=90,
                    params=(padded // 64,),
                    use_direct=True,
                    _completion_validator=validate,
                )
                actual = (
                    np.frombuffer(result.output, dtype=np.uint16)
                    .reshape(-1, 32, 2)
                    .transpose(0, 2, 1)
                    .ravel()[:count]
                )
                if not np.array_equal(actual, expected[start : start + count]):
                    raise ValueError("independent half ordering verification differs")
                report["chunks"].append(
                    {
                        "values": count,
                        "output_sha256": hashlib.sha256(result.output).hexdigest(),
                        "wrapper_ms": result.kernel_reference_ticks / 19200,
                    }
                )
        report["final_health"] = settled_health(power, baseline)
    report["all_checks_passed"] = True
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(
        json.dumps({"passed": True, "values": len(source), "audits": report["audits"]}), flush=True
    )


if __name__ == "__main__":
    main()
