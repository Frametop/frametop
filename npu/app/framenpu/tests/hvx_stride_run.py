"""Opt-in VTCM stride sweep (tests/hvx_stride.c with tests/entry_mw.S, direct dispatch).

For each stride, two repetition counts in fresh sessions; the slope gives ns per
vector (128 tiles x 16 vectors per repetition). Stops at the first failure and
never recovers.

  sudo python3 tests/hvx_stride_run.py --run-on-device --probe hvx_stride.fjob --output report.json
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
from hardware_hmx import PowerSnapshot

MAGIC = 0x53545244
VECTORS = 128 * 16


def point(probe, stride, reps, writes, hi=0, group=0):
    nonce = secrets.randbits(31) | 1
    with Device(dispatch="direct") as device:
        r = device.submit(probe, output_size=64, opcode=11, params=(stride, reps, writes, nonce, hi, group))
    for w, st in enumerate(r.workers):
        if len(st) != 16 or not st[0] or st[1] != w or st[2] or st[0] != r.workers[0][0]:
            raise ValueError(f"worker {w} status {list(st[:4])}")
    o = struct.unpack_from("<9I", r.output)
    if o[:3] != (MAGIC, stride, reps):
        raise ValueError(f"probe did not complete: {o}")
    return o[3] | o[4] << 32, o[5] | o[6] << 32, o[7] | o[8] << 32


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--run-on-device", action="store_true")
    ap.add_argument("--probe", required=True)
    ap.add_argument("--output", required=True)
    ap.add_argument("--strides", default="1,2,4,7,8,16,17,31,32,33,43,48,64,96,128,136,143,144,160,192,248")
    ap.add_argument("--writes", default="0,1")
    ap.add_argument("--hi", type=int, choices=(0, 1), default=0)
    ap.add_argument("--group", type=int, choices=(0, 1), default=0)
    args = ap.parse_args(argv)
    if not args.run_on_device or os.geteuid() != 0:
        ap.error("needs --run-on-device and sudo")
    probe = Job.load(args.probe)
    rows = []
    with PowerSnapshot():
        for writes in (int(x) for x in args.writes.split(",")):
            for s in (int(x) for x in args.strides.split(",")):
                try:
                    lo, hi = point(probe, s, 4, writes, args.hi, args.group), point(probe, s, 20, writes, args.hi, args.group)
                except (FrameNPUError, ValueError, OSError) as exc:
                    rows.append({"stride": s, "writes": writes, "error": f"{type(exc).__name__}: {exc}"})
                    Path(args.output).write_text(json.dumps(rows, indent=1))
                    print("FAILED:", rows[-1], "(no recovery attempted)", flush=True)
                    return 1
                d = [(b - a) / 16 for a, b in zip(lo, hi)]
                row = {"stride": s, "writes": writes, "hi": args.hi, "group": args.group, "ns_per_vector": d[0] / 19.2e6 / VECTORS * 1e9,
                       "cycles_per_vector": d[1] / VECTORS, "ipc": d[2] / d[1]}
                rows.append(row)
                print(f"hi {args.hi} group {args.group} {'write' if writes else 'read ':5s} stride {s:3d} ({s * 128:6d} B): "
                      f"{row['ns_per_vector']:6.2f} ns/vector  {row['cycles_per_vector']:5.1f} cycles  "
                      f"IPC {row['ipc']:.3f}", flush=True)
    Path(args.output).write_text(json.dumps(rows, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
