"""Opt-in DDR -> VTCM copy-strategy timing (ddr_copy_cached.c, cached code).

Points (method, unroll, spin) at n = 64 and 768 vectors; per-vector cost is the
slope. Every copy is checked against the host data (XOR fold, first and last
vector). Cleanup markers are validated before the session reclaims code or
power. Failures stop without recovery.

  sudo python3 tests/ddr_copy_run.py --run-on-device --probe ddr_copy.fjob --output /tmp/ddr-copy.json
"""

import argparse
import functools
import hashlib
import json
import os
from pathlib import Path
import random
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu import Device, Job
from framenpu.errors import FrameNPUError
from hardware_hmx import PowerSnapshot, health
from hmx_cached_run import BASES, validate_cleanup
from hmx_multi_run import failure_snapshot

REF_HZ = 19_200_000
POINTS = {
    "rs_copy": (0, 1, 0),
    "unc_u2": (1, 2, 0), "unc_u4": (1, 4, 0), "unc_u8": (1, 8, 0), "unc_u16": (1, 16, 0),
    "cached_u8": (2, 8, 0), "cached_u16": (2, 16, 0),
    "l2fetch_u8": (3, 8, 0), "l2fetch_u16": (3, 16, 0),
    "prefetched_u8": (4, 8, 9600), "prefetched_u16": (4, 16, 9600),
}


def xor_fold(data):
    acc = 0
    for i in range(0, len(data), 128):
        acc ^= int.from_bytes(data[i:i + 128], "little")
    return acc.to_bytes(128, "little")


def one(probe, data, method, u, spin, cache):
    n = len(data) // 128
    with Device(dispatch="direct") as device:
        result = device._submit(
            probe, data, output_size=640, opcode=13, params=(method, n, u, spin, cache),
            use_direct=True, _completion_validator=lambda r: validate_cleanup(r, cache),
        )
    out = result.output
    meta = struct.unpack_from("<16I", out, 0)
    if meta[0] != 0x44435059 or meta[7] != 4 or meta[1:4] != (method, n, u):
        raise ValueError(f"unexpected probe metadata {meta[:9]}")
    if not BASES[cache] <= meta[8] < BASES[cache] + 4096:
        raise ValueError(f"wrong execution alias {meta[8]:#x}")
    if bytes(out[256:384]) != xor_fold(data) or bytes(out[384:512]) != data[:128] or bytes(out[512:640]) != data[-128:]:
        raise ValueError(f"method {method} u {u} n {n}: copy differs")
    return meta[5], meta[6]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--run-on-device", action="store_true")
    ap.add_argument("--probe", required=True)
    ap.add_argument("--rounds", type=int, default=8)
    ap.add_argument("--points", nargs="+", default=list(POINTS), choices=list(POINTS))
    ap.add_argument("--sizes", type=int, nargs="+", default=[64, 768])
    ap.add_argument("--cache", type=int, choices=(0, 1), default=1)
    ap.add_argument("--output", type=Path, required=True)
    args = ap.parse_args(argv)
    if not args.run_on_device:
        ap.error("--run-on-device is required; this publishes cached DSP code")
    if os.geteuid() != 0:
        ap.error("run with sudo for /dev/mem access")
    if not 1 <= args.rounds <= 50 or any(s % 16 or not 16 <= s <= 768 for s in args.sizes):
        ap.error("1..50 rounds; sizes multiples of 16 in 16..768")
    probe = Job.load(args.probe)
    rng, failed = random.Random(0x44435059), False
    pts = [(p, s) for p in args.points for s in args.sizes]
    report = {"code_sha256": hashlib.sha256(probe.code).hexdigest(), "ref_hz": REF_HZ,
              "cache": args.cache, "samples": []}
    with PowerSnapshot() as power:
        baseline = power.read()
        try:
            report["initial_health"] = health(power, baseline)
            for rnd in range(args.rounds):
                order = pts[:]
                rng.shuffle(order)
                for name, n in order:
                    data = rng.randbytes(n * 128)
                    t, pf = one(probe, data, *POINTS[name], args.cache)
                    if health(power, baseline) != report["initial_health"]:
                        raise ValueError("session restoration differs from initial health")
                    report["samples"].append({"round": rnd, "point": name, "n": n, "ticks": t, "prefetch_ticks": pf})
                print(f"copy: round {rnd + 1}/{args.rounds}", file=sys.stderr, flush=True)
            report["final_health"] = health(power, baseline)
            lo, hi = min(args.sizes), max(args.sizes)
            summary = {}
            for name in args.points:
                med = {}
                for n in (lo, hi):
                    v = sorted(s["ticks"] for s in report["samples"] if (s["point"], s["n"]) == (name, n))
                    med[n] = v[len(v) // 2]
                slope = (med[hi] - med[lo]) / (hi - lo) / REF_HZ * 1e9 if hi > lo else float("nan")
                summary[name] = {"median_us": {n: t / REF_HZ * 1e6 for n, t in med.items()},
                                 "ns_per_vector": slope, "GB_per_s": 128 / slope if slope > 0 else None}
            report["summary"] = summary
        except (FrameNPUError, ValueError, OSError) as exc:
            failed = True
            report["error"] = f"{type(exc).__name__}: {exc}"
            report["after_failure"] = failure_snapshot(power)
            report["interpretation"] = "inconclusive; no recovery attempted"
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"error": report.get("error"), "summary": report.get("summary")}, indent=1))
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
