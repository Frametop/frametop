"""Opt-in INT8 vs FP16 HMX load-pair timing, cached and uncached code (hmx_int8_cached.c).

All-ones operands, so every output is known whatever the layout: INT8 tiles are
floor(32*D/512) with FP16 scale 1.0 and bias 0; FP16 tiles are 32*D. Cleanup
markers are validated before the session reclaims code or power (as
hmx_cached_run.py). Points alternate in a fixed shuffled order each round; the
per-pair cost is the slope between D=8 and D=512. Failures stop without recovery.

  sudo python3 tests/hmx_int8_speed.py --run-on-device --probe hmx_int8_cached.fjob --output /tmp/int8-speed.json
"""

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
from framenpu import Device, Job
from framenpu.errors import FrameNPUError
from hardware_hmx import PowerSnapshot, health
from hmx_cached_run import BASES, validate_cleanup
from hmx_multi_run import failure_snapshot

REF_HZ = 19_200_000
KINDS = {"int8": 1, "fp16": 2}
SCALE_BLOCK = struct.pack("<64I", *([0x3C00] * 32 + [0] * 32))


def operands(kind):
    if kind == "int8":
        return bytes([1]) * 4096
    return struct.pack("<2048e", *([1.0] * 2048))


def expected_tile(kind, depth):
    if kind == "int8":
        return bytes([min(255, 32 * depth // 512)]) * 2048
    return struct.pack("<1024e", *([float(32 * depth)] * 1024))


def one(probe, kind, depth, cache):
    mode, configs = KINDS[kind], 2
    data = operands(kind) + SCALE_BLOCK * configs
    with Device(dispatch="direct") as device:
        result = device._submit(
            probe, data, output_size=256 + configs * 4096, opcode=11,
            params=(mode, configs, depth, 0, cache), use_direct=True,
            _completion_validator=lambda r: validate_cleanup(r, cache),
        )
    out = result.output
    meta = struct.unpack_from("<64I", out, 0)
    if meta[0] != 0x484D3849 or meta[7] != 7 or meta[1:4] != (mode, configs, depth):
        raise ValueError(f"unexpected probe metadata {meta[:9]}")
    if not BASES[cache] <= meta[8] < BASES[cache] + 4096:
        raise ValueError(f"wrong execution alias {meta[8]:#x}")
    want = expected_tile(kind, depth)
    for k in range(configs):
        if bytes(out[256 + k * 4096 : 256 + k * 4096 + 2048]) != want:
            raise ValueError(f"{kind} D={depth} cache={cache}: output differs")
    return [meta[17 + 2 * k] for k in range(configs)]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--run-on-device", action="store_true")
    ap.add_argument("--probe", required=True)
    ap.add_argument("--rounds", type=int, default=10)
    ap.add_argument("--caches", type=int, nargs="+", choices=(0, 1), default=[0, 1])
    ap.add_argument("--depths", type=int, nargs="+", default=[8, 512])
    ap.add_argument("--output", type=Path, required=True)
    args = ap.parse_args(argv)
    if not args.run_on_device:
        ap.error("--run-on-device is required; this publishes cached DSP code and powers HMX")
    if os.geteuid() != 0:
        ap.error("run with sudo for /dev/mem access")
    if not 1 <= args.rounds <= 50 or any(not 1 <= d <= 512 for d in args.depths):
        ap.error("require 1..50 rounds and depths 1..512")
    probe = Job.load(args.probe)
    points = [(k, d, c) for k in KINDS for d in args.depths for c in args.caches]
    report = {"code_sha256": hashlib.sha256(probe.code).hexdigest(), "ref_hz": REF_HZ, "samples": []}
    rng, failed = random.Random(0x49385350), False
    with PowerSnapshot() as power:
        baseline = power.read()
        try:
            report["initial_health"] = health(power, baseline)
            for rnd in range(args.rounds):
                order = points[:]
                rng.shuffle(order)
                for kind, depth, cache in order:
                    ticks = one(probe, kind, depth, cache)
                    if health(power, baseline) != report["initial_health"]:
                        raise ValueError("session restoration differs from initial health")
                    report["samples"].append(
                        {"round": rnd, "kind": kind, "depth": depth, "cache": cache, "ticks": ticks})
                print(f"int8 speed: round {rnd + 1}/{args.rounds}", file=sys.stderr, flush=True)
            report["final_health"] = health(power, baseline)
            summary = {}
            for kind in KINDS:
                for cache in args.caches:
                    med = {}
                    for d in args.depths:
                        v = sorted(t for s in report["samples"] if (s["kind"], s["depth"], s["cache"]) == (kind, d, cache) for t in s["ticks"])
                        med[d] = v[len(v) // 2]
                    lo, hi = min(args.depths), max(args.depths)
                    us = (med[hi] - med[lo]) / (hi - lo) / REF_HZ * 1e6 if hi > lo else float("nan")
                    macs = 65536 if kind == "int8" else 32768
                    summary[f"{kind}_{'cached' if cache else 'uncached'}"] = {
                        "median_ticks": med, "us_per_pair": us, "gmac_per_s": macs / us / 1e3}
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
