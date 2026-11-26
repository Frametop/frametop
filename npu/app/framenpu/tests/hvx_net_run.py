"""Opt-in: the network's 1x1 gather/post code alone (tests/hvx_net.c with tests/entry_mw.S).

Per case, two repetition counts in fresh direct sessions; the slope gives one
full-resolution 1x1 op's time, cycles and packets. Stops at the first failure
and never recovers.

  sudo python3 tests/hvx_net_run.py --run-on-device --probe hvx_net.fjob --output report.json
"""

import argparse
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
from hardware_hmx import PowerSnapshot, health

MAGIC = 0x4E455448
NAMES = {1: "gather", 2: "post", 3: "gather+post", 4: "post ld/st", 5: "post +qf16", 6: "post +clip"}


def point(probe, reps, phases, clip, res):
    nonce = secrets.randbits(31) | 1
    with Device(dispatch="direct") as device:
        r = device.submit(probe, output_size=64, opcode=10, params=(reps, phases, clip, res, nonce))
    for w, st in enumerate(r.workers):
        if len(st) != 16 or not st[0] or st[1] != w or st[2] or st[0] != r.workers[0][0]:
            raise ValueError(f"worker {w} status {list(st[:4])}")
    o = struct.unpack_from("<10I", r.output)
    if o[:3] != (MAGIC, reps, phases):
        raise ValueError(f"probe did not complete: {o}")
    return {"reps": reps, "ticks": o[3] | o[4] << 32, "cycles": o[5] | o[6] << 32,
            "packets": o[7] | o[8] << 32, "sample": o[9]}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--run-on-device", action="store_true")
    ap.add_argument("--probe", required=True)
    ap.add_argument("--output", required=True)
    ap.add_argument("--cases", default="3:1:0,1:1:0,2:1:0,2:0:1", help="phases:clip:res, in order")
    args = ap.parse_args(argv)
    if not args.run_on_device or os.geteuid() != 0:
        ap.error("needs --run-on-device and sudo")
    probe = Job.load(args.probe)
    report = {"cases": []}
    with PowerSnapshot() as power:
        baseline = power.read()
        for c in args.cases.split(","):
            phases, clip, res = (int(x) for x in c.split(":"))
            case = {"phases": NAMES[phases], "clip": clip, "res": res}
            report["cases"].append(case)
            try:
                lo, hi = point(probe, 2, phases, clip, res), point(probe, 10, phases, clip, res)
            except (FrameNPUError, ValueError, OSError) as exc:
                case["error"] = f"{type(exc).__name__}: {exc}"
                Path(args.output).write_text(json.dumps(report, indent=1))
                print("FAILED:", case["error"], "(no recovery attempted)", flush=True)
                return 1
            d = {k: (hi[k] - lo[k]) / 8 for k in ("ticks", "cycles", "packets")}
            case.update(points=[lo, hi], us_per_op=d["ticks"] / 19.2, cycles_per_op=d["cycles"],
                        packets_per_op=d["packets"], ipc=d["packets"] / d["cycles"],
                        health=health(power, baseline))
            print(f"{NAMES[phases]:12s} clip {clip} res {res}: {case['us_per_op']:6.1f} us/op  "
                  f"{d['packets']:8.0f} packets  IPC {case['ipc']:.3f}", flush=True)
    Path(args.output).write_text(json.dumps(report, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
