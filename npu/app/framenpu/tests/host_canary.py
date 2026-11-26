"""Host-only persistent-arena canary: fill/verify the arena-candidate span by
writing and reading the uncached CDSP mapping directly, without claiming the
stock operation gate. This is the soak-safe path: it never contends with live
gaze/FeatureNN work, so the firmware runs completely normally during the soak.

A firmware write into the span (gaze/FeatureNN flush their outputs to DDR) is
observed on readback. Run as steamos.

  python tests/host_canary.py --phase fill   --seed 0xabc   # then wear + use headset
  python tests/host_canary.py --phase verify --seed 0xabc
"""

import argparse
import json
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu.transport import Transport
from tests_canary_pattern import pattern_bytes  # noqa: E402

SAFE_FLOOR = 0x1A22000
BUF_SIZE = 0x2000000
STATE = Path("/tmp/fnpu-canary-state.json")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--phase", choices=("fill", "verify"), required=True)
    parser.add_argument("--seed", type=lambda s: int(s, 0), default=None)
    parser.add_argument("--floor", type=lambda s: int(s, 0), default=SAFE_FLOOR)
    args = parser.parse_args(argv)
    if args.floor < SAFE_FLOOR or args.floor >= BUF_SIZE or args.floor & 127:
        parser.error("floor must be 128-aligned within [%#x, %#x)" % (SAFE_FLOOR, BUF_SIZE))
    seed = args.seed
    if seed is None:
        if args.phase == "verify" and STATE.exists():
            seed = json.loads(STATE.read_text())["seed"]
        else:
            seed = int(time.time()) & 0xFFFFFFFF or 1

    t = Transport()
    try:
        report = {"phase": args.phase, "seed": hex(seed), "span": [hex(args.floor), hex(BUF_SIZE)]}
        report["control_before"] = t.status()["control"]
        expected = pattern_bytes(seed, args.floor, BUF_SIZE - args.floor)
        if args.phase == "fill":
            t.memory[args.floor : BUF_SIZE] = expected
            readback = bytes(t.memory[args.floor : BUF_SIZE])
            report["write_confirmed"] = readback == expected
            STATE.write_text(json.dumps({"seed": seed, "floor": args.floor}))
            report["note"] = "Filled. Wear + use the headset, then run --phase verify."
        else:
            got = bytes(t.memory[args.floor : BUF_SIZE])
            disturbed = 0
            first = None
            if got != expected:
                for i in range(0, len(got), 4):
                    if got[i : i + 4] != expected[i : i + 4]:
                        disturbed += 4
                        if first is None:
                            first = {
                                "offset": hex(args.floor + i),
                                "expected": expected[i : i + 4].hex(),
                                "got": got[i : i + 4].hex(),
                            }
            report["disturbed_bytes"] = disturbed
            report["first_mismatch"] = first
            report["PASS"] = disturbed == 0
        report["control_after"] = t.status()["control"]
        print(json.dumps(report, indent=2))
    finally:
        t.close()


if __name__ == "__main__":
    main()
