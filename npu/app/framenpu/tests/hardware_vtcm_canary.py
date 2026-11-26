"""VTCM write canary for the FeatureNN worker-0/1 scratch span (build here, run on the Frame).

Span: 0x31002000-0x311e0000 (1,912 KiB) directly above the entry's saved
8 KiB. See docs/VTCM_MAP.md for the static ownership argument. Sequence, all
under the audited direct-dispatch protections of the HMX probes:

1. read-only survey of all 8 MiB (before);
2. fill the span with a seed-keyed pattern and verify it in the same job;
3. a raw 64 KiB dump of the span start, checked against the pattern on the host;
4. verify-only jobs spread over --duration seconds (persistence);
5. read-only survey again: every chunk outside the span must be identical to
   step 1 unless it was already changing (gaze), and the span must be stable.

Stops on any unexpected result without retry, recovery, gate clearing or
service restart. The pattern is left in place (scratch semantics), so a later
--verify-only run can test persistence across a worn-headset session.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(ROOT / "tests"))
import hardware_vtcm_survey as survey

MAGIC = 0x43545643
OPCODE = 103
LO, HI = 0x31002000, 0x311E0000
BLOCKS = -(-(HI - LO) // 0x10000)
SOURCE = ROOT / "tests/vtcm_canary_probe.c"


def pattern(addr, count, seed):
    import numpy as np

    a = (np.arange(count, dtype=np.uint64) * 4 + addr).astype(np.uint32)
    x = a ^ (a << np.uint32(13))
    x = x ^ (x >> np.uint32(7))
    return x ^ np.uint32(seed)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--build", action="store_true")
    p.add_argument("--run-on-device", action="store_true")
    p.add_argument("--verify-only", action="store_true")
    p.add_argument("--jobs", required=True, type=Path)
    p.add_argument("--output", type=Path)
    p.add_argument("--seed", type=lambda v: int(v, 0), default=0x6AF91DB2)
    p.add_argument("--duration", type=float, default=30.0)
    p.add_argument("--verifies", type=int, default=10)
    p.add_argument("--label", default="unlabelled")
    args = p.parse_args()
    if args.build:
        survey.build(args.jobs / "survey")
        survey.build(args.jobs / "canary", SOURCE, "vtcm_canary_v1")
        return 0
    if not args.run_on_device or os.geteuid() != 0 or not args.output:
        p.error("--build, or Frame root with --run-on-device and --output")
    if not 1 <= args.verifies <= 200 or not 0 <= args.duration <= 3600:
        p.error("verifies 1..200, duration 0..3600 s")

    import numpy as np
    from framenpu import Device, Job
    from framenpu.resident_gemm import _lease, RECORD
    from framenpu.resident_landmark import CONTROL, ARENA_START, ARENA_END
    from hardware_ieee_probe import completion
    from hardware_hmx import PowerSnapshot, audited_device
    from hardware_resident import settled_health
    from hardware_landmark import service
    from hmx_multi_run import failure_snapshot

    sjob = Job.load(args.jobs / "survey/probe.fjob")
    cjob = Job.load(args.jobs / "canary/probe.fjob")
    report = {
        "format": "framenpu-vtcm-canary",
        "label": args.label,
        "span": [f"{LO:#x}", f"{HI:#x}"],
        "seed": f"{args.seed:#x}",
        "verify_only": args.verify_only,
        "builds": {
            k: json.loads((args.jobs / k / "build.json").read_text()) for k in ("survey", "canary")
        },
        "source_sha256": {
            n: hashlib.sha256((ROOT / n).read_bytes()).hexdigest()
            for n in ("tests/hardware_vtcm_canary.py", "tests/hardware_vtcm_survey.py")
        },
        "audits": {"restored_jobs": 0, "restored_hmx_jobs": 0},
        "service_before": service(),
        "canary": [],
    }
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

                    def submit(job, data, opcode, params, size):
                        return dev._submit(
                            job,
                            data,
                            opcode=opcode,
                            params=params,
                            output_size=size,
                            use_direct=True,
                            _completion_validator=lambda r: completion(r, job, output_bytes=size),
                        )

                    sdata = struct.pack("<I", survey.MAGIC) + bytes(124)

                    def scan():
                        res = submit(
                            sjob, sdata, survey.OPCODE, (1, 0, 0, 0, 0, 0), survey.OUTPUT_BYTES
                        )
                        if struct.unpack_from("<I", res.output)[0] != survey.MAGIC:
                            raise ValueError("survey metadata differs")
                        w = struct.unpack_from(f"<{survey.CHUNKS * 4}I", res.output, survey.META)
                        return [tuple(w[4 * i : 4 * i + 4]) for i in range(survey.CHUNKS)]

                    def canary(mode):
                        data = struct.pack("<II", MAGIC, args.seed) + bytes(120)
                        res = submit(cjob, data, OPCODE, (1, mode, args.seed, 0, 0, 0), 256)
                        m = struct.unpack_from("<64I", res.output)
                        if (
                            m[0] != MAGIC
                            or m[2] != mode
                            or m[3] != args.seed
                            or m[7] != BLOCKS
                            or (m[8], m[9]) != (4, 124)
                            or not 0x87A92000 <= m[1] < 0x87A92000 + len(cjob.code)
                        ):
                            raise ValueError("canary metadata differs")
                        entry = {
                            "mode": mode,
                            "t": time.time(),
                            "mismatched_words": m[4],
                            "per_block": list(m[16 : 16 + BLOCKS]),
                            "fill_us": m[5] / 19.2,
                            "verify_us": m[6] / 19.2,
                            "wrapper_ms": res.kernel_reference_ticks / 19200,
                        }
                        report["canary"].append(entry)
                        return entry

                    first = scan()
                    if not args.verify_only:
                        e = canary(1)
                        if e["mismatched_words"]:
                            raise ValueError("fill did not verify in the same job")
                        dd = struct.pack("<I", survey.MAGIC) + bytes(124)
                        res = submit(
                            sjob,
                            dd,
                            survey.OPCODE,
                            (1, 1, 0x10000, 0, 0, 0),
                            survey.META + survey.DUMP_BYTES,
                        )
                        raw = np.frombuffer(res.output[survey.META :], "<u4")
                        want = pattern(0x31010000, raw.size, args.seed)
                        report["host_pattern_check_words"] = int(raw.size)
                        report["host_pattern_mismatches"] = int((raw != want).sum())
                        if report["host_pattern_mismatches"]:
                            raise ValueError("raw dump differs from host pattern")
                    gap = args.duration / max(1, args.verifies)
                    for _ in range(args.verifies):
                        time.sleep(gap)
                        e = canary(2)
                        if e["mismatched_words"]:
                            report["persistence_broken"] = True
                            break
                    last = scan()
                    lo_c, hi_c = (
                        (LO - survey.BASE) // survey.CHUNK,
                        (HI - survey.BASE) // survey.CHUNK,
                    )
                    outside = [
                        i
                        for i in range(survey.CHUNKS)
                        if not lo_c <= i < hi_c and first[i] != last[i]
                    ]
                    report["outside_changed_chunks"] = [
                        f"{survey.BASE + i * survey.CHUNK:#x}" for i in outside
                    ]
                    report["outside_changed_static"] = sorted(
                        {survey.region(survey.BASE + i * survey.CHUNK) for i in outside}
                    )
                with Device() as observer:
                    after = bytes(observer._transport.memory[ARENA_START:ARENA_END])
                if after != before:
                    raise ValueError("resident arena changed")
                report["final_health"] = settled_health(power, baseline)
                report["service_after"] = service()
                if report["service_before"] != report["service_after"]:
                    raise ValueError("DSP service changed")
                report["all_checks_passed"] = not report.get("persistence_broken")
            except BaseException as exc:
                failed = True
                report["error"] = f"{type(exc).__name__}: {exc}"
                report["after_failure"] = failure_snapshot(power)
                report["interpretation"] = "stopped without retry/recovery/restart"
    finally:
        lease.close()
        args.output.write_text(json.dumps(report, indent=1) + "\n")
    c = report["canary"]
    print(
        json.dumps(
            {
                "all_checks_passed": report.get("all_checks_passed", False),
                "error": report.get("error"),
                "mismatches": [e["mismatched_words"] for e in c],
                "fill_us": [round(e["fill_us"], 1) for e in c if e["mode"] == 1],
                "verify_us": round(sorted(e["verify_us"] for e in c)[len(c) // 2], 1)
                if c
                else None,
                "outside_changed": report.get("outside_changed_chunks"),
                "host_pattern_mismatches": report.get("host_pattern_mismatches"),
            },
            indent=1,
        )
    )
    return int(failed or not report.get("all_checks_passed"))


if __name__ == "__main__":
    sys.exit(main())
