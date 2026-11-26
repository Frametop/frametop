"""Opt-in, read-only DSP address-map probe: system registers and TLB entries.

Uses the default benchmark backend as steamos. The probe performs no TLB or
firmware writes; it only fills its own job output.
"""

import argparse
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu import Device, Job

REGISTERS = "ssr evb cfgbase rev syscfg sp ccr htid stid modectl gp ugp".split()


def decode(index, raw):
    """Hexagon TLB entry layout, as in QEMU's hexagon reg_fields."""
    ppd = raw & 0xFFFFFF
    entry = {"index": index, "raw": f"{raw:#018x}", "valid": bool(raw >> 63)}
    if not ppd:
        return entry
    shift = (ppd & -ppd).bit_length() - 1  # 0 => 4 KiB, each step is 4x
    size = 0x1000 << (2 * shift)
    physical = ((raw >> 61 & 1) << 35) | (ppd >> 1 << 12)
    physical &= ~(size - 1)
    entry.update(
        virtual=f"{(raw >> 32 & 0xFFFFF) << 12 & ~(size - 1):#010x}",
        physical=f"{physical:#011x}",
        size=size,
        asid=raw >> 52 & 0x7F,
        global_=bool(raw >> 62 & 1),
        perms="".join(
            name for bit, name in ((31, "x"), (30, "w"), (29, "r"), (28, "u")) if raw >> bit & 1
        ),
        cache=raw >> 24 & 0xF,
        atr=raw >> 59 & 3,
    )
    return entry


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-on-device", action="store_true")
    parser.add_argument("--probe", required=True, help="tlb_probe.fjob built on a workstation")
    parser.add_argument("--first", type=int, default=0)
    parser.add_argument("--count", type=int, default=64)
    args = parser.parse_args(argv)
    if not args.run_on_device:
        parser.error("--run-on-device is required; this test submits a native DSP job")
    probe = Job.load(args.probe)
    with Device() as device:
        result = device.submit(
            probe, output_size=64 + args.count * 8, params=(args.first, args.count)
        )
        state = device.status()
    words = struct.unpack_from("<16I", result.output)
    entries = struct.unpack_from(f"<{args.count}Q", result.output, 64)
    print(
        json.dumps(
            {
                "registers": {n: f"{v:#010x}" for n, v in zip(REGISTERS, words)},
                "entries": [decode(args.first + i, raw) for i, raw in enumerate(entries)],
                "final_state": state,
            }
        )
    )


if __name__ == "__main__":
    main()
