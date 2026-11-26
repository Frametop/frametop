"""Device run of the single-thread HVX issue-rate microbenchmark
(tests/hvx_rate_probe.c). Per mode: N = 256 and 4096 iterations of 8
independent operations, alternated over rounds; cycles per operation is the
slope. Touches only saved VTCM and the proven FeatureNN scratch span; no DDR
arena writes. Audited direct dispatch, one guarded fault and one rejected
mode; unexpected results stop without retry, recovery or restart.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(ROOT / "tests"))
from framenpu import Job, JobFailed
from hardware_ieee_probe import completion
from hardware_hmx import PowerSnapshot, audited_device
from hardware_resident import settled_health
from hardware_landmark import digest, service
from hmx_multi_run import failure_snapshot

MAGIC, OPCODE, META = 0x31544852, 107, 1024
MODES = {
    0: "vadd_w",
    1: "vtcm_load",
    2: "valign",
    3: "qf16_mpy_add_pair",
    4: "vtcm_store",
    5: "empty",
}
OPS = {0: 8, 1: 8, 2: 8, 3: 4, 4: 8, 5: 1}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--run-on-device", action="store_true")
    p.add_argument("--jobs", required=True, type=Path)
    p.add_argument("--output", required=True, type=Path)
    p.add_argument("--rounds", type=int, default=5)
    args = p.parse_args()
    if not args.run_on_device or os.geteuid() != 0 or not 1 <= args.rounds <= 20:
        p.error("Frame root, --run-on-device and rounds in 1..20 required")
    job = Job.load(args.jobs / "probe.fjob")
    build = json.loads((args.jobs / "build.json").read_text())
    if hashlib.sha256(job.code).hexdigest() != build["sha256"]:
        p.error("build/image hash differs")
    for name, sha in build["sources"].items():
        if digest(ROOT / name) != sha:
            p.error(f"source pin differs: {name}")
    report = {
        "format": "framenpu-hvx-rate-probe",
        "build": build,
        "audits": {"restored_jobs": 0, "restored_hmx_jobs": 0},
    }
    report["service_before"] = service()
    failed = False
    try:
        with PowerSnapshot() as power:
            baseline = power.read()
            report["initial_health"] = settled_health(power, baseline)
            try:
                with audited_device(power, report["audits"])(dispatch="direct") as dev:

                    def run(mode, n, *, fault=False, reject=False):
                        seen = []

                        def validate(result):
                            completion(result, job, fault=fault, reject=reject, output_bytes=META)
                            seen.append(result)

                        try:
                            res = dev._submit(
                                job,
                                bytes(128),
                                opcode=OPCODE,
                                params=(1, mode, n, 0, 0, int(fault)),
                                output_size=META,
                                use_direct=True,
                                _completion_validator=validate,
                            )
                        except JobFailed:
                            if (
                                not seen
                                or not (fault or reject)
                                or seen[0].workers[0][2] != (2 if fault else 1)
                            ):
                                raise
                            return None
                        if fault or reject:
                            raise ValueError("expected fault/rejection missing")
                        m = struct.unpack_from("<5I", res.output)
                        if m[:3] != (MAGIC, mode, n) or not 0 < m[4] <= res.kernel_reference_ticks:
                            raise ValueError(f"rate metadata differs: {m}")
                        return m[3], m[4]

                    rows = {}
                    for mode in MODES:
                        run(mode, 256)  # warmup
                        slopes, mhz = [], []
                        for _ in range(args.rounds):
                            c1, t1 = run(mode, 256)
                            c2, t2 = run(mode, 4096)
                            slopes.append((c2 - c1) / (4096 - 256))
                            mhz.append((c2 - c1) / ((t2 - t1) / 19.2))
                        slopes.sort()
                        med = slopes[len(slopes) // 2]
                        rows[MODES[mode]] = {
                            "cycles_per_iteration": med,
                            "cycles_per_op": med / OPS[mode],
                            "core_mhz": sorted(mhz)[len(mhz) // 2],
                            "samples": slopes,
                        }
                        print(
                            json.dumps({MODES[mode]: rows[MODES[mode]]["cycles_per_op"]}),
                            flush=True,
                        )
                    run(0, 256, fault=True)
                    run(9, 256, reject=True)
                    run(0, 256)
                    report["modes"] = rows
                    report["controls"] = {
                        "guarded_fault": 1,
                        "mode_rejection": 1,
                        "post_fault_valid": 1,
                    }
                report["final_health"] = settled_health(power, baseline)
                report["service_after"] = service()
                if report["service_before"] != report["service_after"]:
                    raise ValueError("DSP service changed")
                report["all_checks_passed"] = True
            except BaseException as exc:
                failed = True
                report["error"] = f"{type(exc).__name__}: {exc}"
                report["after_failure"] = failure_snapshot(power)
    finally:
        args.output.write_text(json.dumps(report, indent=1) + "\n")
    print(
        json.dumps(
            {
                "all_checks_passed": report.get("all_checks_passed", False),
                "error": report.get("error"),
            }
        )
    )
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
