"""Opt-in streaming INT8 vs FP16 HMX timing (hmx_int8_stream_cached.c, cached code).

Per point: kind, nk loads per output tile, L total loads (T = L / nk tiles). The
per-load and per-tile costs come from the slope between L=64 and L=512. Outputs
(first and last tile) are checked: INT8 tiles are nk, FP16 tiles 32*nk. Cleanup
markers are validated before the session reclaims code or power. Points
alternate in a shuffled order each round. Failures stop without recovery.

  sudo python3 tests/hmx_int8_stream_run.py --run-on-device --probe hmx_stream_cached.fjob --output /tmp/int8-stream.json
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
BLOCKS = {
    "int8": struct.pack("<64I", *([0x4C00] * 32 + [0] * 32)),  # FP16 16.0: out = acc / 32
    "fp16": struct.pack("<64I", *([0x3C00] * 32 + [0] * 32)),
}
ROWS = {"int8": 64, "fp16": 32}  # output rows per tile (x 32 channels)


def expected(kind, nk):
    if kind == "int8":
        return bytes([nk]) * 2048
    return struct.pack("<1024e", *([float(32 * nk)] * 1024))


def one(probe, kind, nk, loads, cache, flags=0):
    tiles = loads // nk
    with Device(dispatch="direct") as device:
        result = device._submit(
            probe, BLOCKS[kind], output_size=256 + 4096, opcode=12,
            params=(KINDS[kind], tiles, nk, flags, cache), use_direct=True,
            _completion_validator=lambda r: validate_cleanup(r, cache),
        )
    out = result.output
    meta = struct.unpack_from("<16I", out, 0)
    if meta[0] != 0x484D5354 or meta[7] != 4 or meta[1:4] != (KINDS[kind], tiles, nk):
        raise ValueError(f"unexpected probe metadata {meta[:9]}")
    if not BASES[cache] <= meta[8] < BASES[cache] + 4096:
        raise ValueError(f"wrong execution alias {meta[8]:#x}")
    want = expected(kind, nk)
    if flags & 4:
        if bytes([0xA5]) * 2048 in (bytes(out[256:2304]), bytes(out[2304:4352])):
            raise ValueError(f"{kind} nk={nk} L={loads}: tile not written")
    elif bytes(out[256:2304]) != want or bytes(out[2304:4352]) != want:
        raise ValueError(f"{kind} nk={nk} L={loads}: output differs")
    return meta[5], meta[6]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--run-on-device", action="store_true")
    ap.add_argument("--probe", required=True)
    ap.add_argument("--rounds", type=int, default=10)
    ap.add_argument("--nk", type=int, nargs="+", default=[1, 2, 4, 8])
    ap.add_argument("--loads", type=int, nargs="+", default=[64, 512])
    ap.add_argument("--cache", type=int, choices=(0, 1), default=1)
    ap.add_argument("--flags", type=int, nargs="+", default=[0], help="probe flag sets to compare (0..31)")
    ap.add_argument("--output", type=Path, required=True)
    args = ap.parse_args(argv)
    if not args.run_on_device:
        ap.error("--run-on-device is required; this publishes cached DSP code and powers HMX")
    if os.geteuid() != 0:
        ap.error("run with sudo for /dev/mem access")
    if not 1 <= args.rounds <= 50 or any(n not in (1, 2, 4, 8, 16) for n in args.nk):
        ap.error("require 1..50 rounds and nk in 1,2,4,8,16")
    if any(L % 16 or not 16 <= L <= 512 for L in args.loads):
        ap.error("loads must be multiples of 16 in 16..512")
    probe = Job.load(args.probe)
    if any(not 0 <= f <= 31 for f in args.flags):
        ap.error("flags are 0..31")
    points = [(k, n, L, f) for k in KINDS for n in args.nk for L in args.loads for f in args.flags]
    report = {"code_sha256": hashlib.sha256(probe.code).hexdigest(), "ref_hz": REF_HZ,
              "cache": args.cache, "samples": []}
    rng, failed = random.Random(0x53545245), False
    with PowerSnapshot() as power:
        baseline = power.read()
        try:
            report["initial_health"] = health(power, baseline)
            for rnd in range(args.rounds):
                order = points[:]
                rng.shuffle(order)
                for kind, nk, loads, flags in order:
                    t, done = one(probe, kind, nk, loads, args.cache, flags)
                    if health(power, baseline) != report["initial_health"]:
                        raise ValueError("session restoration differs from initial health")
                    report["samples"].append({"round": rnd, "kind": kind, "nk": nk, "loads": loads, "flags": flags, "ticks": t, "done_ticks": done})
                print(f"stream: round {rnd + 1}/{args.rounds}", file=sys.stderr, flush=True)
            report["final_health"] = health(power, baseline)
            lo, hi = min(args.loads), max(args.loads)
            summary = {}
            for kind in KINDS:
              for flags in args.flags:
                for nk in args.nk:
                    row = {}
                    for key in ("ticks", "done_ticks"):
                        med = {}
                        for L in (lo, hi):
                            v = sorted(s[key] for s in report["samples"] if (s["kind"], s["nk"], s["loads"], s["flags"]) == (kind, nk, L, flags))
                            med[L] = v[len(v) // 2]
                        us_load = (med[hi] - med[lo]) / (hi - lo) / REF_HZ * 1e6 if hi > lo else float("nan")
                        row[key] = {"median_ticks": med, "ns_per_load": us_load * 1e3,
                                    "ns_per_32rows_depth32": us_load * 1e3 * 32 / ROWS[kind]}
                    summary[f"{kind}_nk{nk}_f{flags}"] = row
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
