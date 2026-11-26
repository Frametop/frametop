"""Opt-in FeatureNN VTCM blob claim test (blob_claim_cached.c, cached code).

Steps (run as root on the Frame, tracker stopped):
  verify  compare VTCM 0x311e0000.. with the blob's DDR source (expect 0 differing chunks
          while the firmware flag says loaded)
  claim   clear the firmware's blob-loaded flag (DDR 0xb12014), then fill the blob range
          with a seeded pattern and check it
  check   check the pattern is still intact
Every step prints the flag block (+0x0c copy count, +0x14 loaded flag) before and after,
and checks session health. The job never writes the flag; only `claim` does, host-side,
and only from 1 to 0. Failures stop without recovery.
"""

import argparse
import json
import os
from pathlib import Path
import struct
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu import Device, Job
from framenpu.errors import FrameNPUError
from hardware_hmx import PowerSnapshot, health
from hmx_cached_run import BASES, validate_cleanup
from hmx_multi_run import failure_snapshot

FLAGS, LOADED = 0xB12000, 0xB12014
CHUNKS = 81


def flag_block(device):
    return struct.unpack_from("<8I", device._transport.memory, FLAGS)


def job(device, probe, mode, seed, cache):
    result = device._submit(
        probe, b"", output_size=256 + CHUNKS * 4 + 60, opcode=14, params=(mode, seed, 0, 0, cache),
        use_direct=True, _completion_validator=lambda r: validate_cleanup(r, cache),
    )
    out = result.output
    meta = struct.unpack_from("<16I", out, 0)
    if meta[0] != 0x424C4F42 or meta[7] != 3 or meta[1:3] != (mode, seed):
        raise ValueError(f"unexpected probe metadata {meta[:9]}")
    if not BASES[cache] <= meta[8] < BASES[cache] + 4096:
        raise ValueError(f"wrong execution alias {meta[8]:#x}")
    counts = list(struct.unpack_from(f"<{CHUNKS}I", out, 256))
    return {"ms": meta[5] / 19200, "differing_chunks": meta[6], "chunk_words": counts}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--run-on-device", action="store_true")
    ap.add_argument("--probe", required=True)
    ap.add_argument("--step", required=True, choices=("verify", "claim", "check"))
    ap.add_argument("--seed", type=lambda v: int(v, 0), default=0x6C41494D)
    ap.add_argument("--cache", type=int, choices=(0, 1), default=1)
    ap.add_argument("--output", type=Path, required=True)
    args = ap.parse_args(argv)
    if not args.run_on_device or os.geteuid() != 0:
        ap.error("root on the Frame with --run-on-device")
    probe = Job.load(args.probe)
    mode = {"verify": 0, "claim": 1, "check": 2}[args.step]
    report = {"step": args.step, "seed": args.seed, "time": time.time()}
    failed = False
    with PowerSnapshot() as power:
        baseline = power.read()
        try:
            report["initial_health"] = health(power, baseline)
            with Device(dispatch="direct") as device:
                report["flags_before"] = flag_block(device)
                if args.step == "claim":
                    device._transport.put(LOADED, 0)
                    if device._transport.get(LOADED) != 0:
                        raise ValueError("flag clear did not read back")
                report["result"] = job(device, probe, mode, args.seed, args.cache)
                report["flags_after"] = flag_block(device)
            if health(power, baseline) != report["initial_health"]:
                raise ValueError("session restoration differs from initial health")
            report["final_health"] = health(power, baseline)
        except (FrameNPUError, ValueError, OSError) as exc:
            failed = True
            report["error"] = f"{type(exc).__name__}: {exc}"
            report["after_failure"] = failure_snapshot(power)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    r = report.get("result", {})
    print(json.dumps({"step": args.step, "error": report.get("error"), "flags_before": [hex(x) for x in report.get("flags_before", [])],
                      "flags_after": [hex(x) for x in report.get("flags_after", [])], "ms": r.get("ms"),
                      "differing_chunks": r.get("differing_chunks"),
                      "first_differing": [i for i, n in enumerate(r.get("chunk_words", [])) if n][:8]}))
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
