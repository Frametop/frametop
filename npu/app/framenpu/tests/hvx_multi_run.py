"""Opt-in multi-worker HVX scaling (tests/hvx_multi.c with tests/entry_mw.S).

Runs a ladder of (code cache, mode, active workers) cases, two iteration counts
each, a fresh direct Device per submission. Stops at the first failure and never
recovers: a stall or timeout is reported with a state snapshot and left alone.

  sudo python3 tests/hvx_multi_run.py --run-on-device --probe hvx_multi.fjob --output report.json

Per worker, the slope of its own timed loop between the two iteration counts
cancels setup. Aggregate speedup at N workers = N * single-worker slope / the
slope of the envelope (last end - first start) of all active workers.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import secrets
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(ROOT / "tests"))
from framenpu import Device, Job
from framenpu.errors import FrameNPUError
from framenpu.transport import Transport
from hardware_hmx import PowerSnapshot, health

REF_HZ = 19_200_000
MAGIC = 0x48564D57
OUTPUT_BYTES = 256
MAX_TICKS = REF_HZ * 20 // 1000
MODES = {0: "copy", 1: "post", 2: "gather"}
ITERS = {0: (2, 8), 1: (50, 200)}  # uncached code is ~0.3 us per packet


def decode(result, iters, active, mode, nonce):
    if len(result.output) != OUTPUT_BYTES or len(result.workers) != 3:
        raise ValueError("incomplete multi-worker result")
    out = struct.unpack("<64I", result.output)
    rows = []
    for w, st in enumerate(result.workers):
        if len(st) != 16 or not st[0] or st[1] != w or st[2]:
            raise ValueError(f"worker {w} status {list(st[:4])}")
        if st[0] != result.workers[0][0]:
            raise ValueError("worker completion sequences differ")
        span = (st[12] | st[13] << 32) - (st[10] | st[11] << 32)
        if not 0 < span < MAX_TICKS:
            raise ValueError(f"worker {w} call span {span} ticks")
        r = out[16 * w : 16 * w + 16]
        if w < active:
            if r[:4] != (MAGIC, iters, w, mode) or r[9] != 1 or out[54 + w] != nonce or out[50 + w] != nonce:
                raise ValueError(f"worker {w} did not complete: {list(r[:10])}")
            t0, t1 = r[4] | r[5] << 32, r[6] | r[7] << 32
            if not 0 < t1 - t0 < MAX_TICKS:
                raise ValueError(f"worker {w} loop span {t1 - t0}")
            rows.append({"w": w, "t0": t0, "t1": t1, "loop_ticks": t1 - t0, "call_ticks": span,
                         "ssr": st[7], "check": r[8]})
        elif any(r):
            raise ValueError(f"idle worker {w} wrote results")
    if out[48] != nonce:
        raise ValueError("publish word missing")
    return {"iters": iters, "envelope_ticks": max(r["t1"] for r in rows) - min(r["t0"] for r in rows),
            "start_skew_ticks": max(r["t0"] for r in rows) - min(r["t0"] for r in rows), "workers": rows}


def snapshot(power):
    try:
        t = Transport()
        try:
            return {"state": t.status(), "power": power.read(),
                    "worker_status": [list(struct.unpack_from("<16I", t.memory, t.profile.status + 64 * w))
                                      for w in range(3)]}
        finally:
            t.close()
    except Exception as exc:
        return {"snapshot_error": f"{type(exc).__name__}: {exc}"}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--run-on-device", action="store_true")
    ap.add_argument("--probe", required=True)
    ap.add_argument("--output", required=True)
    ap.add_argument("--cases", default="0:0:1,0:0:2,0:0:3,1:0:1,1:0:2,1:0:3,1:1:1,1:1:2,1:1:3,1:2:1,1:2:2,1:2:3",
                    help="comma list of cached:mode:active, run in order")
    args = ap.parse_args(argv)
    if not args.run_on_device:
        ap.error("--run-on-device is required")
    if os.geteuid() != 0:
        ap.error("run with sudo for /dev/mem access")
    probe = Job.load(args.probe)
    cases = [tuple(int(x) for x in c.split(":")) for c in args.cases.split(",")]
    report = {"code_sha256": hashlib.sha256(probe.code).hexdigest(), "cases": []}

    def save():
        Path(args.output).write_text(json.dumps(report, indent=1))

    with PowerSnapshot() as power:
        baseline = power.read()
        report["initial_health"] = health(power, baseline)
        for cached, mode, active in cases:
            case = {"cached": cached, "mode": MODES[mode], "active": active, "points": []}
            report["cases"].append(case)
            try:
                for iters in ITERS[cached]:
                    nonce = secrets.randbits(31) | 1
                    with Device(dispatch="direct") as device:
                        result = device.submit(probe, output_size=OUTPUT_BYTES, opcode=9,
                                               params=(iters, active, cached, mode, nonce))
                    case["points"].append(decode(result, iters, active, mode, nonce))
                lo, hi = case["points"]
                d = hi["iters"] - lo["iters"]
                case["worker_ticks_per_iter"] = [
                    (b["loop_ticks"] - a["loop_ticks"]) / d for a, b in zip(lo["workers"], hi["workers"])]
                case["envelope_ticks_per_iter"] = (hi["envelope_ticks"] - lo["envelope_ticks"]) / d
                case["health"] = health(power, baseline)
                print(f"cached {cached} {MODES[mode]:6s} active {active}: worker ticks/iter "
                      f"{[round(x, 1) for x in case['worker_ticks_per_iter']]} envelope "
                      f"{case['envelope_ticks_per_iter']:.1f} skew {hi['start_skew_ticks']}", flush=True)
                save()
            except (FrameNPUError, ValueError, OSError) as exc:
                case["error"] = f"{type(exc).__name__}: {exc}"
                report["after_failure"] = snapshot(power)
                report["interpretation"] = "stopped at first failure; no recovery attempted"
                save()
                print("FAILED:", case["error"], flush=True)
                return 1
        report["final_health"] = health(power, baseline)
    save()
    return 0


if __name__ == "__main__":
    sys.exit(main())
