"""Opt-in benchmark/direct comparison. Produces measurements only on a Frame."""

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from framenpu import Device
from framenpu.dispatch import DirectDispatcher, _firmware_original


def percentiles(values):
    ordered = sorted(values)
    return {
        f"p{percent}": ordered[max(0, math.ceil(len(ordered) * percent / 100) - 1)]
        for percent in (50, 95, 99)
    }


def measure(device, job, data=b"", *, expected=b"", opcode=4, params=(), iterations=20):
    wall, roundtrip, ticks = [], [], []
    cold = None
    for index in range(iterations + 1):
        started = time.monotonic_ns()
        result = device.submit(job, data, output_size=len(expected), opcode=opcode, params=params)
        elapsed = (time.monotonic_ns() - started) / 1e6
        if result.output != expected:
            raise AssertionError("dispatch probe output differs")
        if index == 0:
            cold = elapsed
            continue
        wall.append(elapsed)
        roundtrip.append(result.elapsed_ms)
        if result.kernel_reference_ticks is not None:
            ticks.append(result.kernel_reference_ticks)
    return {
        "first_call_wall_ms": cold,
        "warm_wall_ms": percentiles(wall),
        "warm_publish_to_completion_ms": percentiles(roundtrip),
        "frame_job_reference_ticks": percentiles(ticks) if ticks else None,
        "iterations": iterations,
    }


def run(mode, iterations, hmx):
    result = {"dispatch": mode}
    original = _firmware_original()
    with Device(dispatch=mode) as device:
        job = device.builtin(hmx=False)
        result["empty"] = measure(device, job, iterations=iterations)
        data = bytes(range(256)) * 16
        result["copy_4k"] = measure(
            device, job, data, expected=data, opcode=5, iterations=iterations
        )
        if hmx:
            # Identity x quarter-step B gives an exactly representable reference.
            left = [float(i == j) for i in range(32) for j in range(32)]
            right = [(i % 17 - 8) / 4 for i in range(1024)]
            packed = struct.pack("<2048e", *(left + right)) + bytes(128)
            result["gemm_32"] = measure(
                device,
                device.builtin(hmx=True),
                packed,
                expected=struct.pack("<1024f", *right),
                opcode=1,
                params=(32, 32, 32, 0),
                iterations=iterations,
            )
        if mode == "direct":
            # Exercise the untagged compatibility route with the patch active.
            legacy = device._submit(job, opcode=4)
            result["untagged_compatibility_roundtrip_ms"] = legacy.elapsed_ms
        state = device.status()
        if state["control"] != [1, 0, 1] or state["callback"]:
            raise AssertionError(state)
    # Reconnect after close to verify removal, rather than reading closed maps.
    with Device() as observer:
        restored = DirectDispatcher.window(observer._transport)
        if restored != original:
            raise AssertionError("dispatcher window was not restored")
        result["restored_window_sha256"] = hashlib.sha256(restored).hexdigest()
        result["final_state"] = observer.status()
    print(json.dumps(result), flush=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-on-device", action="store_true")
    parser.add_argument("--dispatch", choices=("benchmark", "direct", "both"), default="both")
    parser.add_argument("--iterations", type=int, default=20)
    parser.add_argument("--hmx", action="store_true", help="include HMX GEMM; requires root")
    args = parser.parse_args(argv)
    if not args.run_on_device:
        parser.error("--run-on-device is required; this test changes DSP firmware memory")
    if not 1 <= args.iterations <= 1000:
        parser.error("--iterations must be between 1 and 1000")
    modes = ("benchmark", "direct") if args.dispatch == "both" else (args.dispatch,)
    for mode in modes:
        run(mode, args.iterations, args.hmx)


if __name__ == "__main__":
    main()
