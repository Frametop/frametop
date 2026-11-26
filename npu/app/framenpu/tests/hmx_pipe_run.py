"""Opt-in HMX overhead decomposition and pipelined ceiling (Step 2b). Root.

For each style (0 baseline per-tile, 1 pipelined load/accumulate, 2 clear+load
with one store), sweep M with a fresh Device per submit (reliable), reading
publish->completion ms and 19.2 MHz reference ticks. A least-squares slope gives
per-iteration cost, cancelling fixed offset. Stops each sweep before the 40 ms
frame_job edge and aborts on the first failure. It never recovers gate or power.

  sudo .venv/bin/python tests/hmx_pipe_run.py --run-on-device --probe hmx_pipe.fjob
"""

import argparse
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu import Device, Job
from framenpu.errors import FrameNPUError

REF_HZ = 19_200_000
MACS_PER_ITER = 32 * 32 * 32
LADDER = [200, 500, 1000]


def one(probe, iters, style):
    with Device(dispatch="direct") as device:
        result = device.submit(probe, output_size=64, opcode=7, params=(iters, style))
    w = struct.unpack_from("<16I", result.output)
    if w[0] != 0x48504950 or w[1] != iters:
        raise AssertionError("pipe probe fault %r" % (w[:4],))
    return result.elapsed_ms, result.kernel_reference_ticks


def _slope(xs, ys):
    n = len(xs)
    mx = sum(xs) / n
    my = sum(ys) / n
    den = sum((x - mx) ** 2 for x in xs)
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / den


def sweep(probe, style, cap_ms):
    points = []
    per_iter_ms = None
    for m in LADDER:
        if per_iter_ms and points and points[-1][1] + (m - points[-1][0]) * per_iter_ms > cap_ms:
            break
        elapsed, ticks = one(probe, m, style)
        points.append((m, elapsed, ticks))
        if len(points) >= 2:
            per_iter_ms = (points[-1][1] - points[0][1]) / (points[-1][0] - points[0][0])
    ms = [p[0] for p in points]
    per_iter_s = _slope(ms, [p[1] / 1e3 for p in points]) if len(points) >= 2 else None
    per_iter_ticks = _slope(ms, [p[2] for p in points]) if len(points) >= 2 else None
    out = {
        "style": style,
        "points": [{"m": p[0], "ms": round(p[1], 3), "ticks": p[2]} for p in points],
    }
    if per_iter_ticks:
        per_iter_s_ticks = per_iter_ticks / REF_HZ
        out["per_iter_us"] = round(per_iter_s_ticks * 1e6, 4)
        out["per_iter_ticks"] = round(per_iter_ticks, 3)
        out["gmac_per_s"] = round(MACS_PER_ITER / per_iter_s_ticks / 1e9, 3)
        if per_iter_s:
            out["per_iter_us_wall"] = round(per_iter_s * 1e6, 4)
    return out


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-on-device", action="store_true")
    parser.add_argument("--probe", required=True)
    parser.add_argument("--cap-ms", type=float, default=32.0)
    args = parser.parse_args(argv)
    if not args.run_on_device:
        parser.error("--run-on-device is required; this test powers and runs HMX")
    if not 0 < args.cap_ms <= 32:
        parser.error("--cap-ms must be positive and at most 32 ms")
    probe = Job.load(args.probe)
    report = {"ref_hz": REF_HZ, "target_gmac_per_s": 17.472}
    styles = {0: "baseline_per_tile", 1: "pipelined_load_accumulate", 2: "clear_load_store_once"}
    for style, name in styles.items():
        try:
            report[name] = sweep(probe, style, args.cap_ms)
        except FrameNPUError as exc:
            report["error"] = {"style": style, "message": f"{type(exc).__name__}: {exc}"}
            print(json.dumps(report, indent=2))
            return 1
    with Device() as device:
        report["final_state"] = device.status()
    # Decomposition from per-iter ticks.
    try:
        b = report["baseline_per_tile"]["per_iter_ticks"]
        p = report["pipelined_load_accumulate"]["per_iter_ticks"]
        c = report["clear_load_store_once"]["per_iter_ticks"]
        report["decomposition_ticks"] = {
            "hmx_mac_load": round(p, 2),
            "clear_per_iter": round(c - p, 2),
            "store_barrier_per_iter": round(b - c, 2),
        }
    except Exception:
        pass
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
