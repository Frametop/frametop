"""Opt-in INT8 HMX probe runner (tests/hmx_int8_probe.c). Records raw outputs only.

Mode 0 (convert only) sweeps bias-block patterns with a cleared fixed-point
accumulator, to find how the two 32-word halves of the 256-byte bias block map to
uint8 outputs. Mode 1 runs D activation/weight load pairs from given operands.
Every dump is the whole 4 KiB VTCM span around the output tile, prefilled with
0xa5, so the report shows how many bytes the store wrote and where. A fresh
direct session is used for every job; health is checked before and after each.
Failures stop without recovery. Run as root on the Frame.

  sudo python3 tests/hmx_int8_run.py --run-on-device --probe hmx_int8.fjob --mode 0 --output /tmp/int8-m0.json
"""

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu import Device, Job
from framenpu.errors import FrameNPUError
from hardware_hmx import PowerSnapshot, health
from hmx_multi_run import failure_snapshot

MAGIC = 0x484D3849
CANARY = 0xA5


def bias_block(w0, w1):
    """Two 32-word halves; each argument is a value or a per-channel function."""
    words = [(w0(c) if callable(w0) else w0) for c in range(32)]
    words += [(w1(c) if callable(w1) else w1) for c in range(32)]
    return struct.pack("<64I", *[w & 0xFFFFFFFF for w in words])


BIT = lambda c: 1 << c  # noqa: E731
MODE0 = [
    ("zero", 0, 0),
    ("fp16_one_scale", 0x3C00, 0),
    ("w1_bit", 0, BIT),
    ("w0_bit", BIT, 0),
    ("w0_fp16one_w1_bit", 0x3C00, BIT),
    ("w0_bit_w1_fp16one", BIT, 0x3C00),
    ("w0_ones", 0xFFFFFFFF, 0),
    ("w1_ones", 0, 0xFFFFFFFF),
    ("w0_bit_w1_ones", BIT, 0xFFFFFFFF),
    ("w0_ones_w1_bit", 0xFFFFFFFF, BIT),
    ("w0_1_w1_bit", 1, BIT),
    ("w0_bit_w1_1", BIT, 1),
    ("w0_256_w1_bit", 0x100, BIT),
    ("w0_bit_w1_256", BIT, 0x100),
    ("w0_65536_w1_bit", 0x10000, BIT),
    ("w0_bit_w1_65536", BIT, 0x10000),
    ("w0_7fff_w1_bit", 0x7FFF, BIT),
    ("w0_bit_w1_7fff", BIT, 0x7FFF),
    ("w0_chan_w1_chan", lambda c: c, lambda c: c),
    ("w0_chan_shl16_w1_chan", lambda c: c << 16, lambda c: c),
    ("w0_chan_w1_chan_shl16", lambda c: c, lambda c: c << 16),
    ("w0_chan_shl8_w1_chan_shl8", lambda c: c << 8, lambda c: c << 8),
    ("w0_neg_bit", lambda c: -(1 << c), 0),
    ("w1_neg_bit", 0, lambda c: -(1 << c)),
]


def analyse(span, bias):
    """Which bytes of the 4 KiB span changed from the canary (outside the bias block)."""
    written = [i for i, b in enumerate(span) if b != CANARY and not 0x800 <= i < 0x900]
    tile = span[:2048]
    return {
        "changed_bytes": len(written),
        "changed_range": [written[0], written[-1]] if written else None,
        "bias_block_intact": span[0x800:0x900] == bias,
        "distinct_tile_bytes": sorted(set(tile))[:64],
        "first_row_bytes": list(tile[:64]),
    }


def run_job(probe, mode, depth, cvt_rs, act, wei, blocks):
    data = act + wei + b"".join(blocks)
    with Device(dispatch="direct") as device:
        result = device.submit(
            probe,
            data,
            output_size=256 + len(blocks) * 4096,
            opcode=11,
            params=(mode, len(blocks), depth, cvt_rs),
        ).output
    if len(result) != 256 + len(blocks) * 4096:
        raise ValueError("incomplete probe output")
    meta = struct.unpack_from("<64I", result, 0)
    if meta[0] != MAGIC or meta[7] != 7 or meta[1:5] != (mode, len(blocks), depth, cvt_rs):
        raise ValueError(f"unexpected probe metadata {meta[:8]}")
    dumps = [bytes(result[256 + k * 4096 : 256 + (k + 1) * 4096]) for k in range(len(blocks))]
    ticks = [meta[17 + 2 * k] for k in range(len(blocks))]
    return dumps, ticks, meta[5]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--run-on-device", action="store_true")
    ap.add_argument("--probe", required=True)
    ap.add_argument("--mode", type=int, choices=(0, 1), default=0)
    ap.add_argument("--cvt-rs", type=lambda s: int(s, 0), nargs="+", default=[2])
    ap.add_argument("--operands", type=Path, help="mode 1: JSON {act, wei (hex), bias: [hex...], depth}")
    ap.add_argument("--output", type=Path, required=True)
    args = ap.parse_args(argv)
    if not args.run_on_device:
        ap.error("--run-on-device is required; this powers HMX and runs unproven HMX encodings")
    if os.geteuid() != 0:
        ap.error("run with sudo for /dev/mem access")
    probe = Job.load(args.probe)
    if not probe.requires_hmx or not probe.supports_direct:
        ap.error("probe requires HMX and direct-entry flags")
    if args.mode == 0:
        names = [n for n, _, _ in MODE0]
        blocks = [bias_block(a, b) for _, a, b in MODE0]
        act = wei = bytes(2048)
        depth = 0
    else:
        spec = json.loads(args.operands.read_text())
        act, wei = bytes.fromhex(spec["act"]), bytes.fromhex(spec["wei"])
        blocks = [bytes.fromhex(b) for b in spec["bias"]]
        names = spec.get("names", [f"bias{k}" for k in range(len(blocks))])
        depth = int(spec["depth"])
        if len(act) != 2048 or len(wei) != 2048 or not 1 <= depth <= 64:
            ap.error("operands need 2048-byte act/wei and depth 1..64")
    report = {
        "code_sha256": hashlib.sha256(probe.code).hexdigest(),
        "mode": args.mode,
        "depth": depth,
        "act_sha256": hashlib.sha256(act).hexdigest(),
        "wei_sha256": hashlib.sha256(wei).hexdigest(),
        "runs": [],
    }
    failed = False
    with PowerSnapshot() as power:
        baseline = power.read()
        try:
            report["initial_health"] = health(power, baseline)
            for rs in args.cvt_rs:
                dumps, ticks, total = run_job(probe, args.mode, depth, rs, act, wei, blocks)
                if health(power, baseline) != report["initial_health"]:
                    raise ValueError("session restoration differs from initial health")
                report["runs"].append(
                    {
                        "cvt_rs": rs,
                        "total_ticks": total,
                        "configs": [
                            {
                                "name": n,
                                "bias_hex": b.hex(),
                                "hmx_ticks": t,
                                **analyse(d, b),
                                "span_b64": base64.b64encode(d).decode(),
                            }
                            for n, b, d, t in zip(names, blocks, dumps, ticks)
                        ],
                    }
                )
                print(f"int8 probe: cvt_rs={rs:#x} done", file=sys.stderr, flush=True)
            report["final_health"] = health(power, baseline)
        except (FrameNPUError, ValueError, OSError) as exc:
            failed = True
            report["error"] = f"{type(exc).__name__}: {exc}"
            report["after_failure"] = failure_snapshot(power)
            report["interpretation"] = "inconclusive; no recovery attempted"
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"report": str(args.output), "error": report.get("error"),
                      "runs": len(report["runs"])}))
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
