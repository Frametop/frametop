"""Opt-in weight-resident streaming ceiling (experiment #2). Root.

Splits the HMX load into a weight load and an activation load and compares three
styles over a fresh Device per submit, reading publish->completion ms and
19.2 MHz reference ticks:

  style 1  clear; loop (wgt + act); store      control, = the pipelined pair
  style 3  clear; wgt once; loop (act); store   weight-resident streaming
  style 5  clear; loop (act only); store        activation MAC with no weight load

A least-squares slope gives per-iteration cost, cancelling fixed offset. If
style 3's per-iter cost is ~half style 1's, the weight reload was ~half the pair
load and a weight-resident executor roughly doubles single-worker HMX. Style 5
bounds the activation issue alone. Stops each sweep before the 40 ms frame_job
edge and aborts on the first failure. It never recovers gate or power.

  sudo python3 tests/hmx_stream_run.py --run-on-device --probe hmx_stream.fjob
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
MAGIC = 0x48535452
LADDER = [200, 500, 1000]


def one(probe, iters, style):
    with Device(dispatch="direct") as device:
        result = device.submit(probe, output_size=64, opcode=7, params=(iters, style))
    w = struct.unpack_from("<16I", result.output)
    if w[0] != MAGIC or w[1] != iters:
        raise AssertionError("stream probe fault %r" % (w[:4],))
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
    parser.add_argument(
        "--style",
        type=int,
        choices=(1, 3, 5),
        default=1,
        help="3 and 5 are known-faulting experimental encodings",
    )
    args = parser.parse_args(argv)
    if not args.run_on_device:
        parser.error("--run-on-device is required; this test powers and runs HMX")
    if not 0 < args.cap_ms <= 32:
        parser.error("--cap-ms must be positive and at most 32 ms")
    probe = Job.load(args.probe)
    report = {"ref_hz": REF_HZ, "target_gmac_per_s": 17.472}
    styles = {1: "pair_control", 3: "weight_resident", 5: "activation_only"}
    for style, name in styles.items():
        if style != args.style:
            continue
        try:
            report[name] = sweep(probe, style, args.cap_ms)
        except FrameNPUError as exc:
            report["error"] = {"style": style, "message": f"{type(exc).__name__}: {exc}"}
            print(json.dumps(report, indent=2))
            return 1
    with Device() as device:
        report["final_state"] = device.status()
    try:
        pair = report["pair_control"]["per_iter_ticks"]
        wres = report["weight_resident"]["per_iter_ticks"]
        act = report["activation_only"]["per_iter_ticks"]
        report["analysis_ticks"] = {
            "pair_load": round(pair, 2),
            "weight_resident": round(wres, 2),
            "activation_only": round(act, 2),
            "weight_load_share": round((pair - wres) / pair, 3) if pair else None,
            "speedup_vs_pair": round(pair / wres, 3) if wres else None,
        }
    except Exception:
        pass
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
