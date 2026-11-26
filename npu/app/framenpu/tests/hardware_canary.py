"""Opt-in persistent-arena canary soak.

Fills the arena-candidate span (DDR 0x1a22000..0x2000000, above every reserved
FeatureNN worker slot) with a deterministic pattern, then — after the headset is
used normally — verifies it is intact. A mismatch means stock firmware disturbed
the span, so it is NOT free for a persistent weight/activation arena.

Both phases run the pattern on the DSP (proving DSP TLB read/write at the span)
and cross-check from the host through the uncached mmap. Chunked to stay far
under the 40 ms job window. Run as steamos; no root or HMX needed.

  # write the pattern, then put the headset on and use it for a few minutes
  python tests/hardware_canary.py --run-on-device --phase fill --probe canary_probe.fjob
  # after the soak (seed is printed by the fill run)
  python tests/hardware_canary.py --run-on-device --phase verify --probe canary_probe.fjob --seed S
"""

import argparse
import json
from pathlib import Path
import struct
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu import Device, Job

SAFE_FLOOR = 0x1A22000
BUF_SIZE = 0x2000000
STATE = Path("/tmp/fnpu-canary-state.json")


def _pattern(seed, off):
    v = (seed * 0x01000193) & 0xFFFFFFFF
    v ^= (off * 0x9E3779B1) & 0xFFFFFFFF
    v ^= v >> 15
    v = (v * 0x85EBCA77) & 0xFFFFFFFF
    v ^= v >> 13
    return v & 0xFFFFFFFF


def _host_expected(seed, off, length):
    words = (_pattern(seed, off + 4 * i) for i in range(length // 4))
    return struct.pack("<%dI" % (length // 4), *words)


def run(phase, probe, seed, floor, chunk, cached):
    report = {"phase": phase, "seed": seed, "span": [hex(floor), hex(BUF_SIZE)], "cached": cached}
    total = BUF_SIZE - floor
    host_mismatch = 0
    first = None
    with Device() as device:
        report["state_before"] = device.status()
        mem = device._transport.memory
        offset = floor
        while offset < BUF_SIZE:
            length = min(chunk, BUF_SIZE - offset)
            exp = _host_expected(seed, offset, length)
            if phase == "fill":
                # The DSP writes the pattern through its cached alias and flushes
                # to DDR: this exercises the real resident-weight write path and
                # proves DSP TLB write access at the span. The host then confirms
                # the bytes reached DDR through the uncached mapping.
                result = device.submit(
                    probe, output_size=64, opcode=1, params=(1, seed, offset, length, cached)
                )
                w = struct.unpack("<16I", result.output)
                if w[0] != 0x434E5259 or w[2]:
                    raise AssertionError("fill probe fault %r" % (w[:6],))
                if bytes(mem[offset : offset + length]) != exp:
                    host_mismatch += length
            else:
                # Verify is host-only: read the span straight from DDR through the
                # uncached alias and compare. No job window to overrun, and a
                # firmware write (gaze/FeatureNN flush their outputs) reaches DDR,
                # so this is the authoritative "was the span disturbed" check.
                got = bytes(mem[offset : offset + length])
                if got != exp:
                    for i in range(0, length, 4):
                        if got[i : i + 4] != exp[i : i + 4]:
                            host_mismatch += 4
                            if first is None:
                                first = {
                                    "offset": hex(offset + i),
                                    "expected": exp[i : i + 4].hex(),
                                    "got": got[i : i + 4].hex(),
                                }
            offset += length
        report["state_after"] = device.status()

    report["span_bytes"] = total
    if phase == "fill":
        report["dsp_write_reached_ddr"] = host_mismatch == 0
        STATE.write_text(json.dumps({"seed": seed, "floor": floor, "cached": cached}))
        report["note"] = "Pattern written. Use the headset normally, then run --phase verify."
    else:
        report["disturbed_bytes"] = host_mismatch
        report["first_mismatch"] = first
        report["PASS"] = host_mismatch == 0
    print(json.dumps(report, indent=2))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-on-device", action="store_true")
    parser.add_argument("--probe", required=True)
    parser.add_argument("--phase", choices=("fill", "verify"), required=True)
    parser.add_argument("--seed", type=lambda s: int(s, 0), default=None)
    parser.add_argument("--floor", type=lambda s: int(s, 0), default=SAFE_FLOOR)
    parser.add_argument("--chunk", type=lambda s: int(s, 0), default=0x10000)
    parser.add_argument("--cached", type=int, choices=(0, 1), default=1)
    args = parser.parse_args(argv)
    if not args.run_on_device:
        parser.error("--run-on-device is required; this test writes shared DSP DDR")
    if args.floor < SAFE_FLOOR or args.floor >= BUF_SIZE or args.floor & 127:
        parser.error("floor must be 128-aligned within [%#x, %#x)" % (SAFE_FLOOR, BUF_SIZE))
    if args.chunk & 127 or not 0 < args.chunk <= 0x200000:
        parser.error("chunk must be 128-aligned and at most 2 MiB")
    seed = args.seed
    if seed is None:
        if args.phase == "verify" and STATE.exists():
            seed = json.loads(STATE.read_text())["seed"]
        else:
            seed = int(time.time()) & 0xFFFFFFFF or 1
    run(args.phase, Job.load(args.probe), seed, args.floor, args.chunk, args.cached)


if __name__ == "__main__":
    main()
