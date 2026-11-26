"""Opt-in: C calls with the stack in VTCM (tests/stack_probe.c with tests/entry_mw.S).

Runs a short spin first, then longer ones, a fresh direct session each. Stops at
the first failure and never recovers.

  sudo python3 tests/stack_probe_run.py --run-on-device --probe stack_probe.fjob --output report.json
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

MAGIC = 0x53544B50
KEYS = ("magic", "framelimit", "framekey", "entry_sp", "lowest_sp", "lowest_changed", "changed_words",
        "changed_below_sp", "spun_lo", "spun_hi", "check", "framelimit_after", "sp_after")


def point(probe, spin, depth):
    nonce = secrets.randbits(31) | 1
    with Device(dispatch="direct") as device:
        r = device.submit(probe, output_size=64, opcode=12, params=(spin, depth, nonce))
    for w, st in enumerate(r.workers):
        if len(st) != 16 or not st[0] or st[1] != w or st[2] or st[0] != r.workers[0][0]:
            raise ValueError(f"worker {w} status {list(st[:4])}")
    o = dict(zip(KEYS, struct.unpack_from("<13I", r.output)))
    if o["magic"] != MAGIC:
        raise ValueError(f"probe did not complete: {o}")
    o["spun_ms"] = (o.pop("spun_lo") | o.pop("spun_hi") << 32) / 19200
    return {k: (f"{v:#010x}" if k in ("framelimit", "framekey", "entry_sp", "lowest_sp", "lowest_changed",
                                      "framelimit_after", "sp_after") else v) for k, v in o.items()}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--run-on-device", action="store_true")
    ap.add_argument("--probe", required=True)
    ap.add_argument("--output", required=True)
    ap.add_argument("--cases", default="192:2,19200:6,192000:6,288000:8", help="spin_ticks:depth")
    args = ap.parse_args(argv)
    if not args.run_on_device or os.geteuid() != 0:
        ap.error("needs --run-on-device and sudo")
    probe = Job.load(args.probe)
    rows = []
    with PowerSnapshot():
        for c in args.cases.split(","):
            spin, depth = (int(x) for x in c.split(":"))
            try:
                rows.append({"spin": spin, "depth": depth, **point(probe, spin, depth)})
            except (FrameNPUError, ValueError, OSError) as exc:
                rows.append({"spin": spin, "depth": depth, "error": f"{type(exc).__name__}: {exc}"})
                Path(args.output).write_text(json.dumps(rows, indent=1))
                print("FAILED:", rows[-1], "(no recovery attempted)", flush=True)
                return 1
            print(json.dumps(rows[-1]), flush=True)
    Path(args.output).write_text(json.dumps(rows, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
