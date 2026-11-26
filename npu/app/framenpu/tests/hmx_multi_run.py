"""Opt-in multi-worker HMX timing. Stops at the first failure; never recovers.

Build with handoff-tools/build_mw.py and tests/entry_mw.S. Each submission uses
a fresh direct Device. Two iteration counts cancel fixed initialization and
VTCM save/restore costs. Aggregate throughput uses the envelope of all active
workers' timed calls. Concurrent accumulator contents are unchecked, so this
measures issue timing, not independent, numerically correct matrix operations.

  sudo python3 tests/hmx_multi_run.py --run-on-device --probe hmx_multi.fjob --active 1

Workers 1/2 have no custom fault vectors. A trap/hang can require a firmware
reload. A timeout does not establish that HMX is single-owner or serialized.
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
from framenpu import Device, Job
from framenpu.errors import FrameNPUError
from framenpu.transport import Transport
from hardware_hmx import PowerSnapshot, health

REF_HZ = 19_200_000
MACS_PER_ITER = 32 * 32 * 32
MAGIC = 0x484D574B
OUTPUT_BYTES = 64 + 2 * 8192
MAX_TICKS = REF_HZ * 40 // 1000


def timing(stat):
    start = stat[10] | (stat[11] << 32)
    end = stat[12] | (stat[13] << 32)
    if not start or not end or not 0 < end - start < MAX_TICKS:
        raise ValueError("missing, reversed or over-window worker timing")
    return start, end


def decode(result, iters, active):
    if len(result.output) != OUTPUT_BYTES or len(result.workers) != 3:
        raise ValueError("incomplete multi-worker result")
    out = struct.unpack_from("<16I", result.output)
    workers = []
    for w, st in enumerate(result.workers):
        if len(st) != 16 or not st[0] or st[1] != w or st[2]:
            raise ValueError(f"invalid worker-{w} status")
        if st[0] != result.workers[0][0]:
            raise ValueError("worker completion sequences differ")
        start, end = timing(st)
        words = out[w * 4 : w * 4 + 4]
        ran = w < active
        if ran and (words[0] != MAGIC or words[1] != iters or words[3] != w):
            raise ValueError(f"worker {w} did not complete its HMX loop")
        if not ran and any(words):
            raise ValueError(f"idle worker {w} wrote results")
        workers.append(
            {
                "htid": w,
                "error": st[2],
                "ssr_before": st[7],
                "start_ticks": start,
                "end_ticks": end,
                "span_ticks": end - start,
                "ran": ran,
                "out_iters": words[1] if ran else 0,
                "tc0": words[2] if ran else None,
            }
        )
    running = workers[:active]
    envelope = max(w["end_ticks"] for w in running) - min(w["start_ticks"] for w in running)
    return {
        "iters": iters,
        "elapsed_ms": result.elapsed_ms,
        "envelope_ticks": envelope,
        "workers": workers,
    }


def summarize(points, active):
    low, high = points
    delta = high["iters"] - low["iters"]
    slopes = [
        (high["workers"][w]["span_ticks"] - low["workers"][w]["span_ticks"]) / delta
        for w in range(active)
    ]
    envelope = (high["envelope_ticks"] - low["envelope_ticks"]) / delta
    if envelope <= 0 or any(s <= 0 for s in slopes):
        raise ValueError("nonpositive timing slope; measurement is inconclusive")
    return {
        "active": active,
        "worker_ticks_per_iter": slopes,
        "envelope_ticks_per_iter": envelope,
        "aggregate_issue_gmac_per_s": active * MACS_PER_ITER * REF_HZ / envelope / 1e9,
    }


def failure_snapshot(power):
    try:
        t = Transport()
        try:
            state = t.status()
            rows = [
                list(struct.unpack_from("<16I", t.memory, t.profile.status + 64 * w))
                for w in range(3)
            ]
        finally:
            t.close()
        return {"state": state, "worker_status": rows, "power": power.read()}
    except Exception as exc:
        return {"snapshot_error": f"{type(exc).__name__}: {exc}"}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--run-on-device", action="store_true")
    ap.add_argument("--probe", required=True)
    ap.add_argument("--iters", type=int, default=1000)
    ap.add_argument("--short-iters", type=int, default=200)
    ap.add_argument("--active", type=int, choices=(1, 2, 3), default=1)
    args = ap.parse_args(argv)
    if not args.run_on_device:
        ap.error("--run-on-device is required; this test powers and runs HMX in direct mode")
    if not 0 < args.short_iters < args.iters <= 1000:
        ap.error("require 0 < --short-iters < --iters <= 1000 to bound each submission")
    if os.geteuid() != 0:
        ap.error("run with sudo for /dev/mem access")
    probe = Job.load(args.probe)
    if not probe.requires_hmx or not probe.supports_direct:
        ap.error("probe requires HMX and direct-entry flags")
    report = {
        "ref_hz": REF_HZ,
        "active": args.active,
        "code_sha256": hashlib.sha256(probe.code).hexdigest(),
        "code_bytes": len(probe.code),
        "points": [],
        "numerical_correctness_checked": False,
    }
    with PowerSnapshot() as power:
        baseline = power.read()
        try:
            report["initial_health"] = health(power, baseline)
            for iters in (args.short_iters, args.iters):
                with Device(dispatch="direct") as device:
                    result = device.submit(
                        probe, output_size=OUTPUT_BYTES, opcode=7, params=(iters, args.active)
                    )
                point = decode(result, iters, args.active)
                point["health"] = health(power, baseline)
                report["points"].append(point)
            report["measurement"] = summarize(report["points"], args.active)
            report["final_health"] = health(power, baseline)
        except (FrameNPUError, ValueError, OSError) as exc:
            report["error"] = f"{type(exc).__name__}: {exc}"
            report["after_failure"] = failure_snapshot(power)
            report["interpretation"] = "inconclusive; no recovery attempted"
            print(json.dumps(report, indent=2), flush=True)
            return 1
    print(json.dumps(report, indent=2), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
