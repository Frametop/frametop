"""Opt-in HMX throughput-ceiling measurement (Step 2 go/no-go). Requires root.

Runs the resident-tile HMX benchmark (tests/hmx_bench.c) under direct dispatch.
Each job runs M back-to-back 32x32x32 FP16 products on resident VTCM tiles with
no per-tile upload or host round trip. Measuring two M values makes the
steady-state per-GEMM cost a slope, cancelling fixed setup, power and
notification overhead. The host wall-clock delta gives absolute seconds per
GEMM without assuming the DSP clock; the reference-tick delta cross-checks it.

  sudo .venv/bin/python tests/hmx_bench_run.py --run-on-device --probe hmx_bench.fjob
"""

import argparse
import json
from pathlib import Path
from statistics import median
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu import Device, Job

MACS_PER_GEMM = 32 * 32 * 32  # 32768


def one(device, probe, iters, readback):
    result = device.submit(probe, output_size=64, opcode=7, params=(iters, readback))
    w = struct.unpack_from("<16I", result.output)
    if w[0] != 0x484D4258 or w[1] != iters:
        raise AssertionError("bench probe fault %r" % (w[:4],))
    # elapsed_ms is publish->observed-completion: it excludes the ~34 ms HMX
    # power setup and operand upload, so it isolates frame_job + notification.
    return result.elapsed_ms, result.kernel_reference_ticks


def sample(device, probe, iters, readback, reps):
    walls, ticks = [], []
    one(device, probe, iters, readback)  # warm
    for _ in range(reps):
        wall, tk = one(device, probe, iters, readback)
        walls.append(wall)
        if tk is not None:
            ticks.append(tk)
    return median(walls), (median(ticks) if ticks else None)


def _slope(xs, ys):
    # Least-squares slope of y vs x.
    n = len(xs)
    mx = sum(xs) / n
    my = sum(ys) / n
    num = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    den = sum((x - mx) ** 2 for x in xs)
    return num / den


def measure(device, probe, readback, reps, ladder):
    points = []
    for m in ladder:
        wall, tk = sample(device, probe, m, readback, reps)
        points.append((m, wall, tk))
    ms = [p[0] for p in points]
    per_gemm_s = _slope(ms, [p[1] / 1e3 for p in points])  # seconds per extra GEMM
    have_ticks = all(p[2] is not None for p in points)
    per_gemm_ticks = _slope(ms, [p[2] for p in points]) if have_ticks else None
    out = {
        "readback": bool(readback),
        "ladder": [
            {"m": p[0], "publish_to_done_ms": round(p[1], 3), "ticks": p[2]} for p in points
        ],
        "per_gemm_us": round(per_gemm_s * 1e6, 5),
        "per_gemm_ticks": round(per_gemm_ticks, 2) if per_gemm_ticks else None,
        "tile_mac_per_s": round(MACS_PER_GEMM / per_gemm_s),
        "effective_gmac_per_s": round(MACS_PER_GEMM / per_gemm_s / 1e9, 3),
    }
    if per_gemm_ticks:
        out["ref_tick_rate_mhz"] = round(per_gemm_ticks / per_gemm_s / 1e6, 1)
    return out


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-on-device", action="store_true")
    parser.add_argument("--probe", required=True)
    parser.add_argument("--reps", type=int, default=12)
    parser.add_argument("--dispatch", choices=("benchmark", "direct"), default="direct")
    parser.add_argument(
        "--ladder",
        type=lambda s: [int(x) for x in s.split(",")],
        default=[200, 500, 1000],
        help="comma-separated M values; each must keep frame_job under 40 ms",
    )
    args = parser.parse_args(argv)
    if not args.run_on_device:
        parser.error("--run-on-device is required; this test powers and runs HMX")
    if (
        len(args.ladder) < 2
        or args.ladder != sorted(set(args.ladder))
        or not all(0 < m <= 1000 for m in args.ladder)
    ):
        parser.error("--ladder requires at least two increasing M values in 1..1000")
    if not 1 <= args.reps <= 100:
        parser.error("--reps must be between 1 and 100")
    probe = Job.load(args.probe)
    report = {
        "target": "17.472 GMAC/s (4 crops, 2 hands, 2 views, 30 Hz)",
        "dispatch": args.dispatch,
    }
    with Device(dispatch=args.dispatch) as device:
        report["state_before"] = device.status()
        report["hmx_only"] = measure(device, probe, 0, args.reps, args.ladder)
        report["hmx_plus_readback"] = measure(device, probe, 1, args.reps, args.ladder)
        report["state_after"] = device.status()
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
