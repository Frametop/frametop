"""Opt-in full-tile HMX accumulator isolation test. Requires root and direct dispatch.

First validate each worker alone against exact FP16 results. Then serialize full
transactions as a control, defer stores across other workers' clear/load phases,
and release complete transactions together. Numeric mismatches in the latter two
experiments are findings; malformed results, failed controls and timeouts stop the
test. No recovery or service restart is attempted. Exit zero means the experiment
completed; inspect isolation_passed for its numerical outcome.

  sudo python3 tests/hmx_accum_run.py --run-on-device --probe hmx_accum.fjob --phase suite
"""

import argparse
from collections import Counter
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
from hmx_multi_run import failure_snapshot, timing

OUTPUT_BYTES = 256 + 3 * 2048 + 2 * 8192
MAGIC = 0x48414343
MODES = {"isolated": 0, "serialized": 0, "staged": 1, "concurrent": 2}
MASKS = (3, 5, 6, 7)


def expected_tile(iters, worker):
    return struct.pack("<e", 32 * iters * (1 << worker)) * 1024


def decode(result, iters, mask, mode, controls):
    if len(result.output) != OUTPUT_BYTES or len(result.workers) != 3:
        raise ValueError("incomplete accumulator result")
    selected = [w for w in range(3) if mask & (1 << w)]
    generations = struct.unpack_from("<3I", result.output)
    aborts = struct.unpack_from("<3I", result.output, 16)
    rounds = 2 if mode == 2 else 2 + len(selected) * (2 if mode == 1 else 1)
    workers, tiles = [], {}
    for w, status in enumerate(result.workers):
        if len(status) != 16 or not status[0] or status[1] != w or status[2]:
            raise ValueError(f"invalid worker-{w} completion")
        if status[0] != result.workers[0][0]:
            raise ValueError("worker completion sequences differ")
        wrapper_start, wrapper_end = timing(status)
        meta = struct.unpack_from("<16I", result.output, 64 + 64 * w)
        raw = result.output[256 + 2048 * w : 256 + 2048 * (w + 1)]
        if w not in selected:
            if any(meta) or any(raw) or generations[w] or aborts[w]:
                raise ValueError(f"inactive worker {w} wrote results")
            continue
        if meta[:6] != (MAGIC, w, mask, mode, iters, 0):
            raise ValueError(f"worker {w} metadata or rendezvous failed: {meta[:6]}")
        if generations[w] != rounds or aborts[w]:
            raise ValueError(f"worker {w} did not complete all rendezvous")
        ready, start, loaded, store_start, end = [
            meta[i] | meta[i + 1] << 32 for i in (6, 8, 10, 12, 14)
        ]
        if not wrapper_start <= ready < start <= loaded <= store_start < end <= wrapper_end:
            raise ValueError(f"worker {w} HMX timestamps are missing or out of order")
        expected = expected_tile(iters, w)
        control = controls.get((iters, w), expected)
        actual_words = struct.unpack("<1024H", raw)
        expected_words = struct.unpack("<1024H", expected)
        control_words = struct.unpack("<1024H", control)
        mismatches = [i for i, (a, e) in enumerate(zip(actual_words, expected_words)) if a != e]
        histogram = Counter(actual_words)
        workers.append(
            {
                "htid": w,
                "expected_value": 32 * iters * (1 << w),
                "expected_bits": f"0x{expected_words[0]:04x}",
                "checked_elements": 1024,
                "mismatched_elements": len(mismatches),
                "mismatches_vs_isolated_control": sum(
                    a != c for a, c in zip(actual_words, control_words)
                ),
                "first_mismatches": [
                    {"index": i, "actual_bits": f"0x{actual_words[i]:04x}"} for i in mismatches[:8]
                ],
                "distinct_values": len(histogram),
                "histogram": [
                    {"bits": f"0x{bits:04x}", "count": count}
                    for bits, count in histogram.most_common(16)
                ],
                "tile_sha256": hashlib.sha256(raw).hexdigest(),
                "ready_ticks": ready,
                "hmx_start_ticks": start,
                "load_end_ticks": loaded,
                "store_start_ticks": store_start,
                "hmx_end_ticks": end,
                "wrapper_span_ticks": wrapper_end - wrapper_start,
            }
        )
        tiles[(iters, w)] = raw
    if max(w["ready_ticks"] for w in workers) >= min(w["hmx_start_ticks"] for w in workers):
        raise ValueError("HMX started before every selected worker finished setup")
    # Ordered phases must be disjoint, and deferred stores must follow all loads.
    if mode in (0, 1):
        for left, right in zip(workers, workers[1:]):
            boundary = left["hmx_end_ticks"] if mode == 0 else left["load_end_ticks"]
            if boundary > right["hmx_start_ticks"]:
                raise ValueError("serialized HMX phases overlap")
        if mode == 1:
            if max(w["load_end_ticks"] for w in workers) > min(
                w["store_start_ticks"] for w in workers
            ):
                raise ValueError("a deferred store preceded another worker's load")
            if any(
                a["hmx_end_ticks"] > b["store_start_ticks"] for a, b in zip(workers, workers[1:])
            ):
                raise ValueError("deferred stores overlap")
    overlap = min(w["hmx_end_ticks"] for w in workers) - max(w["hmx_start_ticks"] for w in workers)
    return {
        "iters": iters,
        "mask": mask,
        "mode": mode,
        "elapsed_ms": round(result.elapsed_ms, 3),
        "hmx_start_skew_ticks": max(w["hmx_start_ticks"] for w in workers)
        - min(w["hmx_start_ticks"] for w in workers),
        "all_worker_window_overlap_ticks": max(0, overlap)
        if mode == 2 and len(workers) > 1
        else None,
        "matches_independent_results": all(w["mismatched_elements"] == 0 for w in workers),
        "workers": workers,
    }, tiles


def cases(phase, iterations, reps):
    for iters in iterations:
        for w in range(3):
            yield "isolated", iters, 1 << w, 0
    if phase == "controls":
        return
    for iters in iterations:
        for mask in MASKS:
            yield "serialized", iters, mask, 0
    for name in ("staged", "concurrent"):
        if phase not in (name, "suite"):
            continue
        for iters in iterations:
            for mask in MASKS:
                for rep in range(reps):
                    yield name, iters, mask, rep


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-on-device", action="store_true")
    parser.add_argument("--probe", required=True)
    parser.add_argument(
        "--phase",
        choices=("controls", "serialized", "staged", "concurrent", "suite"),
        default="controls",
    )
    parser.add_argument("--iters", type=int, nargs="+", default=[1, 32])
    parser.add_argument("--reps", type=int, default=3)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    if not args.run_on_device:
        parser.error("--run-on-device is required; this powers HMX and changes DSP state")
    if not 1 <= args.reps <= 20 or not args.iters or any(not 1 <= n <= 32 for n in args.iters):
        parser.error("require 1..20 repetitions and iteration counts in 1..32")
    if os.geteuid() != 0:
        parser.error("run with sudo for /dev/mem access")
    probe = Job.load(args.probe)
    if not probe.requires_hmx or not probe.supports_direct:
        parser.error("probe requires HMX and direct-entry flags")
    report = {
        "ref_hz": 19_200_000,
        "code_bytes": len(probe.code),
        "code_sha256": hashlib.sha256(probe.code).hexdigest(),
        "phase": args.phase,
        "runs": [],
        "numerical_check": "all 1024 FP16 elements per selected worker, exact bits",
    }
    controls, failed = {}, False
    with PowerSnapshot() as power:
        baseline = power.read()
        try:
            report["initial_health"] = health(power, baseline)
            for name, iters, mask, rep in cases(args.phase, sorted(set(args.iters)), args.reps):
                with Device(dispatch="direct") as device:
                    result = device.submit(
                        probe, output_size=OUTPUT_BYTES, opcode=8, params=(iters, mask, MODES[name])
                    )
                run, tiles = decode(result, iters, mask, MODES[name], controls)
                run.update(phase=name, repetition=rep, health=health(power, baseline))
                report["runs"].append(run)
                if name in ("isolated", "serialized") and not run["matches_independent_results"]:
                    raise ValueError(f"{name} control failed: mask={mask}, iterations={iters}")
                if name == "isolated":
                    controls.update(tiles)
            report["final_health"] = health(power, baseline)
            report["isolation_passed"] = all(
                r["matches_independent_results"] for r in report["runs"]
            )
        except (FrameNPUError, ValueError, OSError) as exc:
            failed = True
            report["error"] = f"{type(exc).__name__}: {exc}"
            report["after_failure"] = failure_snapshot(power)
            report["interpretation"] = "inconclusive; no recovery attempted"
    encoded = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.write_text(encoded)
        print(
            json.dumps(
                {
                    "report": str(args.output),
                    "completed_runs": len(report["runs"]),
                    "isolation_passed": report.get("isolation_passed"),
                    "error": report.get("error"),
                    "phases": {
                        name: {
                            "runs": sum(r["phase"] == name for r in report["runs"]),
                            "numerical_failures": sum(
                                r["phase"] == name and not r["matches_independent_results"]
                                for r in report["runs"]
                            ),
                        }
                        for name in MODES
                    },
                },
                indent=2,
            )
        )
    else:
        print(encoded, end="")
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
