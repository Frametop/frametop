"""Opt-in correct single-owner GEMM baseline, with resident and host phase timings.

All returned tiles are checked against an integer-derived reference, including
every repeated transaction. Repeated depth blocks define a synthetic reduction
with K=32*depth, not a general model's distinct weight working set. A fresh direct
session is used for every sample; no power settings or public SDK code change.
Failures stop sampling without recovery. Warmups are checked but excluded from
percentiles. Run as root on the Frame.

  sudo python3 tests/hmx_baseline_run.py --run-on-device --probe hmx_baseline.fjob --output /tmp/hmx-baseline.json
"""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import random
import struct
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu import Device, Job
from framenpu.errors import FrameNPUError
from framenpu.power import HmxPower
from framenpu.transport import Transport
from hardware_hmx import PowerSnapshot, health
from hmx_multi_run import failure_snapshot, timing

REF_HZ = 19_200_000
MAGIC = 0x484D5342
CASES = ("identity", "signed_dense")


def operands(case):
    """Integers in eighths keep every accumulation exact in FP32."""
    rng = random.Random(0x53494E47 + CASES.index(case))
    a = (
        [8 * int(r == c) for r in range(32) for c in range(32)]
        if case == "identity"
        else [rng.randint(-4, 4) for _ in range(1024)]
    )
    b = [rng.randint(-4, 4) for _ in range(1024)]
    data = struct.pack("<2048e", *[x / 8 for x in a + b])
    product = [
        sum(a[r * 32 + k] * b[k * 32 + c] for k in range(32)) for r in range(32) for c in range(32)
    ]
    return data, product


def reference(product, depth):
    row_bytes = struct.pack("<1024e", *[x * depth / 64 for x in product])
    row_bits = struct.unpack("<1024H", row_bytes)
    packed_bits = [
        row_bits[r * 32 + c]
        for pair in range(16)
        for c in range(32)
        for r in (pair * 2, pair * 2 + 1)
    ]
    return struct.pack("<1024H", *packed_bits), struct.pack(
        "<1024f", *struct.unpack("<1024e", row_bytes)
    )


def decode(result, depth, transactions, expected):
    size = 256 + transactions * 2048 + 4096
    if len(result.output) != size or len(result.workers) != 3:
        raise ValueError("incomplete baseline result")
    for w, status in enumerate(result.workers):
        if (
            len(status) != 16
            or not status[0]
            or status[1] != w
            or status[2]
            or status[0] != result.workers[0][0]
        ):
            raise ValueError(f"invalid worker-{w} completion")
    wrapper_start, wrapper_end = timing(result.workers[0])
    words = struct.unpack_from("<64I", result.output)
    if words[:8] != (MAGIC, 1, depth, transactions, 4096, transactions * 2048, 4096, 0):
        raise ValueError("baseline metadata mismatch")
    if any(words[14:16]) or any(words[24:32]) or any(words[32 + 8 * transactions :]):
        raise ValueError("unused metadata was modified")

    def tick(index):
        return words[index] | words[index + 1] << 32

    start, transferred, packed = tick(8), tick(10), tick(12)
    if not wrapper_start <= start < transferred < packed < wrapper_end:
        raise ValueError("invalid input copy/packing timestamps")
    hmx, raw_copy, previous = 0, 0, packed
    transaction_ticks = []
    for rep in range(transactions):
        offset = 32 + 8 * rep
        begin, end, copy_start, copy_end = [tick(offset + i) for i in (0, 2, 4, 6)]
        if not previous <= begin < end <= copy_start < copy_end <= wrapper_end:
            raise ValueError("invalid or overlapping HMX/transfer timestamps")
        transaction_ticks.append(end - begin)
        hmx += end - begin
        raw_copy += copy_end - copy_start
        previous = copy_end
        raw = result.output[256 + rep * 2048 : 256 + (rep + 1) * 2048]
        if raw != expected[0]:
            count = sum(
                a != b
                for a, b in zip(struct.unpack("<1024H", raw), struct.unpack("<1024H", expected[0]))
            )
            raise ValueError(f"transaction {rep}: {count}/1024 FP16 results differ")
    convert_start, convert_end, copied = tick(16), tick(18), tick(20)
    if not previous <= convert_start < convert_end < copied <= wrapper_end:
        raise ValueError("invalid output conversion/transfer timestamps")
    if result.output[256 + transactions * 2048 :] != expected[1]:
        raise ValueError("full FP32 conversion output differs")
    stages = {
        "input_copy": transferred - start,
        "packing": packed - transferred,
        "hmx": hmx,
        "raw_output_copy": raw_copy,
        "output_conversion": convert_end - convert_start,
        "fp32_output_copy": copied - convert_end,
    }
    stages["unattributed"] = wrapper_end - wrapper_start - sum(stages.values())
    if stages["unattributed"] < 0:
        raise ValueError("phase intervals exceed frame_job timing")
    return {
        "depth": depth,
        "transactions": transactions,
        "macs": 32768 * depth * transactions,
        "checked_fp16_elements": 1024 * transactions,
        "checked_fp32_elements": 1024,
        "dsp_phase_ticks": stages,
        "hmx_transaction_ticks": transaction_ticks,
        "frame_job_ticks": wrapper_end - wrapper_start,
        "timer_pair_ticks": tick(22),
        "publish_to_completion_ms": result.elapsed_ms,
    }


class TimedTransport(Transport):
    """Measure existing host phases without changing transport behavior."""

    def __init__(self):
        super().__init__()
        self.current = None
        self.benchmarks = []

    def claim(self, timeout):
        start = time.monotonic_ns()
        super().claim(timeout)
        end = time.monotonic_ns()
        self.current = {"claim_ms": (end - start) / 1e6, "claim_end_ns": end}

    def publish(self, *, direct=False):
        p = self.profile
        if self.get(p.header + 36) == 9 and self.get(p.header + 12) & 1:
            self.benchmarks.append(self.current)
        super().publish(direct=direct)

    def wait_complete(self, seq, timeout):
        super().wait_complete(seq, timeout)
        self.current["complete_ns"] = time.monotonic_ns()

    def acknowledge(self, timeout):
        start = time.monotonic_ns()
        if "power_off_end_ns" in self.current:
            self.current["ddr_restore_ms"] = (start - self.current["power_off_end_ns"]) / 1e6
        super().acknowledge(timeout)
        self.current["acknowledge_ms"] = (time.monotonic_ns() - start) / 1e6


class TimedPower(HmxPower):
    def __init__(self, trace):
        super().__init__()
        self.trace = trace

    def start(self):
        start = time.monotonic_ns()
        self.trace["backup_upload_ms"] = (start - self.trace["claim_end_ns"]) / 1e6
        result = super().start()
        self.trace["power_on_ms"] = (time.monotonic_ns() - start) / 1e6
        self.trace["powered_configuration"] = {
            "pll": {f"0x{o:x}": self._r(self.p, o) for o in (0, 4, 0x10, 0x14, 0x18)},
            "clocks": {f"0x{o:x}": self._r(self.c, o) for o in (0x180, 0x184, 0x1A0, 0x200)},
        }
        return result

    def close(self):
        start = time.monotonic_ns()
        if "complete_ns" in self.trace:
            self.trace["readback_ms"] = (start - self.trace["complete_ns"]) / 1e6
        super().close()
        end = time.monotonic_ns()
        self.trace["power_off_ms"] = (end - start) / 1e6
        self.trace["power_off_end_ns"] = end


def one(probe, data, depth, transactions, expected):
    session_start = time.monotonic_ns()
    transport = TimedTransport()
    with Device(
        dispatch="direct",
        _transport=transport,
        _power_factory=lambda: TimedPower(transport.current),
    ) as device:
        start = time.monotonic_ns()
        result = device.submit(
            probe,
            data,
            output_size=256 + transactions * 2048 + 4096,
            opcode=9,
            params=(depth, transactions),
        )
        submit_ms = (time.monotonic_ns() - start) / 1e6
    session_ms = (time.monotonic_ns() - session_start) / 1e6
    sample = decode(result, depth, transactions, expected)
    if len(transport.benchmarks) != 1:
        raise ValueError("ambiguous host timing trace")
    trace = transport.benchmarks[0]
    sample["host_phase_ms"] = {k: v for k, v in trace.items() if k.endswith("_ms")}
    sample["powered_configuration"] = trace["powered_configuration"]
    sample["submit_wall_ms"], sample["session_wall_ms"] = submit_ms, session_ms
    return sample


def percentiles(values):
    ordered = sorted(values)
    if not ordered or any(not math.isfinite(v) or v < 0 for v in ordered):
        raise ValueError("invalid latency samples")
    return {f"p{q}": ordered[max(0, math.ceil(len(ordered) * q / 100) - 1)] for q in (50, 95, 99)}


def summarize(samples):
    groups = {}
    for sample in samples:
        groups.setdefault((sample["case"], sample["depth"]), []).append(sample)
    points = []
    for (case, depth), rows in sorted(groups.items()):
        phases = {
            name: percentiles([r["dsp_phase_ticks"][name] / REF_HZ * 1e6 for r in rows])
            for name in rows[0]["dsp_phase_ticks"]
        }
        point = {
            "case": case,
            "depth": depth,
            "logical_shape": [32, 32 * depth, 32],
            "transactions": rows[0]["transactions"],
            "samples": len(rows),
            "dsp_phase_us": phases,
            "hmx_gmac_per_s_at_p50": rows[0]["macs"] / phases["hmx"]["p50"] / 1000,
            "frame_job_us": percentiles([r["frame_job_ticks"] / REF_HZ * 1e6 for r in rows]),
            "publish_to_completion_ms": percentiles([r["publish_to_completion_ms"] for r in rows]),
            "submit_wall_ms": percentiles([r["submit_wall_ms"] for r in rows]),
            "session_wall_ms": percentiles([r["session_wall_ms"] for r in rows]),
            "host_phase_ms": {
                name: percentiles([r["host_phase_ms"][name] for r in rows])
                for name in rows[0]["host_phase_ms"]
            },
            "timer_pair_ticks": percentiles([r["timer_pair_ticks"] for r in rows]),
        }
        points.append(point)
    slopes = []
    for case in sorted({point["case"] for point in points}):
        rows = [point for point in points if point["case"] == case]
        if len(rows) < 2:
            continue
        xs = [r["depth"] for r in rows]
        ys = [r["dsp_phase_us"]["hmx"]["p50"] * REF_HZ / 1e6 / r["transactions"] for r in rows]
        mx, my = sum(xs) / len(xs), sum(ys) / len(ys)
        slope = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sum((x - mx) ** 2 for x in xs)
        if slope <= 0:
            raise ValueError("nonpositive resident timing slope; inconclusive measurement")
        slopes.append(
            {
                "case": case,
                "ticks_per_depth_load": slope,
                "transaction_fixed_ticks": my - slope * mx,
                "load_slope_gmac_per_s": 32768 * REF_HZ / slope / 1e9,
                "method": "least squares of median HMX ticks per transaction versus depth count",
            }
        )
    return {"points": points, "resident_slopes": slopes}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-on-device", action="store_true")
    parser.add_argument("--probe", required=True)
    parser.add_argument("--samples", type=int, default=50)
    parser.add_argument("--warmup", type=int, default=2)
    parser.add_argument("--depths", type=int, nargs="+", default=[1, 32, 128, 512])
    parser.add_argument("--transactions", type=int, default=4)
    parser.add_argument("--cases", choices=CASES, nargs="+", default=list(CASES))
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    if not args.run_on_device:
        parser.error("--run-on-device is required; this powers HMX and changes DSP state")
    if not 1 <= args.samples <= 200 or not 0 <= args.warmup <= 10:
        parser.error("require 1..200 samples and 0..10 warmups")
    if not 1 <= args.transactions <= 4 or any(not 1 <= n <= 512 for n in args.depths):
        parser.error("require 1..4 transactions and depth counts in 1..512")
    if os.geteuid() != 0:
        parser.error("run with sudo for /dev/mem access")
    probe = Job.load(args.probe)
    if not probe.requires_hmx or not probe.supports_direct:
        parser.error("probe requires HMX and direct-entry flags")
    cases = {case: operands(case) for case in dict.fromkeys(args.cases)}
    expected = {
        (case, depth): reference(cases[case][1], depth)
        for case in cases
        for depth in sorted(set(args.depths))
    }
    report = {
        "ref_hz": REF_HZ,
        "code_bytes": len(probe.code),
        "code_sha256": hashlib.sha256(probe.code).hexdigest(),
        "case_input_sha256": {
            case: hashlib.sha256(value[0]).hexdigest() for case, value in cases.items()
        },
        "samples_per_point": args.samples,
        "warmups_per_point": args.warmup,
        "percentile_method": "nearest rank; small-sample p99 may equal the maximum",
        "residency": "two 2 KiB VTCM blocks reused along K and across transactions within one submission",
        "samples": [],
        "checked_warmup_jobs": 0,
    }
    rng, failed = random.Random(0x42415345), False
    with PowerSnapshot() as power:
        baseline = power.read()
        try:
            report["initial_health"] = health(power, baseline)
            for index in range(args.samples + args.warmup):
                order = list(expected)
                rng.shuffle(order)
                for case, depth in order:
                    sample = one(
                        probe, cases[case][0], depth, args.transactions, expected[(case, depth)]
                    )
                    current_health = health(power, baseline)
                    if current_health != report["initial_health"]:
                        raise ValueError("session restoration differs from initial health")
                    sample.update(case=case, sample_index=index - args.warmup)
                    if index < args.warmup:
                        report["checked_warmup_jobs"] += 1
                    else:
                        report["samples"].append(sample)
                if index == args.warmup - 1 or (index - args.warmup + 1) % 10 == 0:
                    print(
                        f"baseline: {len(report['samples'])} measured jobs, {report['checked_warmup_jobs']} checked warmups",
                        file=sys.stderr,
                        flush=True,
                    )
            report["summary"] = summarize(report["samples"])
            report["final_health"] = health(power, baseline)
            report["all_outputs_match"] = True
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
                    "measured_jobs": len(report["samples"]),
                    "all_outputs_match": report.get("all_outputs_match", False),
                    "error": report.get("error"),
                    "resident_slopes": report.get("summary", {}).get("resident_slopes"),
                },
                indent=2,
            )
        )
    else:
        print(encoded, end="")
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
