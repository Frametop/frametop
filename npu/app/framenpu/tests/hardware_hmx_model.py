"""HMX model feasibility timings with public weights and synthetic activations.

No recorded/private image data or training labels are used. Every returned
matrix is compared against a CPU precision model after audited restoration.
The resident control repeats one operand pair through K; stream cases use
all distinct pairs. No retries/recovery or new resource/timeout limits.
"""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu import Device, Job, JobFailed
from framenpu.convolution import load_landmark_graph
from framenpu.landmark import specifications, weights_and_bias
from framenpu.resident_gemm import _lease, RECORD
from framenpu.resident_landmark import CONTROL, ARENA_START, ARENA_END
from hardware_ieee_probe import completion
from hardware_hmx import PowerSnapshot, audited_device
from hardware_resident import settled_health
from hardware_landmark import digest, service
from hmx_multi_run import failure_snapshot


def pack(matrix):
    import numpy as np

    m = np.asarray(matrix, dtype="<f2")
    if m.shape != (32, 32) or not np.isfinite(m).all():
        raise ValueError("invalid packed operand")
    return m.reshape(16, 2, 32).transpose(0, 2, 1).copy().tobytes()


def unpack(raw):
    import numpy as np

    return (
        np.frombuffer(raw, dtype="<f2")
        .reshape(16, 32, 2)
        .transpose(0, 2, 1)
        .reshape(32, 32)
        .astype(np.float64)
    )


def cases(graph):
    import numpy as np

    result = []
    r, c = np.indices((32, 32))
    for depth in (1, 9, 25):
        a = np.stack(
            [
                (((13 * r + 7 * c + 5 * t) % 17 - 8) / 64 + t / 1024).astype(np.float16)
                for t in range(depth)
            ]
        )
        b = np.stack(
            [
                (((3 * r + 11 * c + 9 * t) % 19 - 9) / 32 + t / 512).astype(np.float16)
                for t in range(depth)
            ]
        )
        result.append(
            (
                "dense_" + str(depth),
                a,
                b,
                ((np.arange(32) % 5 - 2) / 16).astype(np.float32),
                "synthetic dense weights and activations",
            )
        )
    specs = {s["name"]: s for s in specifications(graph)}
    for name in (
        "convdwclip_0",
        "convdwclip_8",
        "convdwclip_12",
        "conv_10",
        "convclip_12",
        "linear_42",
    ):
        s = specs[name]
        values, bias = weights_and_bias(graph, s)
        n = s["output_shape"][0]
        if s["operator"] == "ConvolutionDepthWise":
            channels, h, w = s["input_shape"]
            _, oh, ow = s["output_shape"]
            kh, kw = s["kernel"]
            sh, sw = s["stride"]
            pt, pb, pl, pr = s["padding"]
            depth = kh * kw
            weight = np.asarray(values, dtype=np.float16).reshape(n, depth)
            a = np.zeros((depth, 32, 32), dtype=np.float16)
            b = np.zeros_like(a)
            base = (oh // 2) * ow if oh * ow >= 64 else 0
            for t in range(depth):
                ky, kx = divmod(t, kw)
                for ch in range(min(channels, 32)):
                    a[t, ch, ch] = weight[ch, t]
                    for p in range(32):
                        pos = base + p
                        if pos >= oh * ow:
                            continue
                        oy, ox = divmod(pos, ow)
                        iy = oy * sh + ky - pt
                        ix = ox * sw + kx - pl
                        if 0 <= iy < h and 0 <= ix < w:
                            b[t, ch, p] = ((17 * ch + 3 * iy + 5 * ix) % 31 - 15) / 32
            role = "public depthwise weights embedded diagonally; synthetic spatial features"
        else:
            k = (
                math.prod(s["input_shape"])
                if s["operator"] == "InnerProduct"
                else s["input_shape"][0]
            )
            depth = (k + 31) // 32
            a = np.zeros((depth, 32, 32), dtype=np.float16)
            b = np.zeros_like(a)
            weight = np.asarray(values, dtype=np.float16).reshape(n, k)
            for t in range(depth):
                active = min(32, k - t * 32)
                a[t, : min(n, 32), :active] = weight[:32, t * 32 : t * 32 + active]
                for kk in range(active):
                    for p in range(1 if s["operator"] == "InnerProduct" else 32):
                        b[t, kk, p] = ((17 * (t * 32 + kk) + 5 * p) % 31 - 15) / 32
            role = "public pointwise/head weights; synthetic features"
        bias32 = np.zeros(32, dtype=np.float32)
        bias32[: min(n, 32)] = bias[:32]
        result.append((name, a, b, bias32, role))
    return result


def payload(a, b, bias):
    depth = len(a)
    return (
        struct.pack("<32I", depth, *([0] * 31))
        + b"".join(pack(x) for x in a)
        + b"".join(pack(x) for x in b)
        + bias.astype("<f4").tobytes()
        + bias.astype("<f2").tobytes().ljust(128, b"\0")
    )


def validate_values(result, a, b, bias, mode):
    import numpy as np

    if mode == 0:
        a = np.repeat(a[:1], len(a), axis=0)
        b = np.repeat(b[:1], len(b), axis=0)
    aa = np.concatenate(list(a.astype(np.float64)), axis=1)
    bb = np.concatenate(list(b.astype(np.float64)), axis=0)
    exact = aa @ bb
    condition = np.abs(aa) @ np.abs(bb)
    if not np.isfinite(exact).all() or np.max(condition) > 60000:
        raise ValueError("fixture exceeds half output/range bound")
    rounded = exact.astype(np.float16).astype(np.float64)
    # Conservative accumulation roundoff plus one half-store ULP. Distinct
    # dense dyadic fixtures also record exact-value matches separately.
    gamma = (aa.shape[1] * 2**-23) / (1 - aa.shape[1] * 2**-23)
    allowance = gamma * condition + 2**-10 * np.abs(rounded) + 2**-20
    if mode == 2:
        actual = (
            np.frombuffer(result.output, dtype="<f4", offset=2304)
            .reshape(32, 32)
            .astype(np.float64)
        )
        expected = np.clip(rounded + bias[:, None].astype(np.float64), 0, 6)
        allowance += 2**-21 * (np.abs(rounded) + np.abs(bias[:, None])) + 2**-20
    else:
        actual = unpack(result.output[256:2304])
        if mode == 3:
            hb = bias.astype(np.float16).astype(np.float64)[:, None]
            expected = np.clip((rounded + hb).astype(np.float16).astype(np.float64), 0, 6)
            allowance += 2**-9 * (np.abs(rounded) + np.abs(hb)) + 2**-14
        else:
            expected = rounded
    error = np.abs(actual - expected)
    if not np.isfinite(actual).all() or np.any(error > allowance):
        raise ValueError("HMX matrix differs after clean restoration")
    return {
        "values": 1024,
        "max_abs": float(np.max(error)),
        "rms": float(np.sqrt(np.mean(error * error))),
        "exact_values": int(np.sum(actual == expected)),
        "max_error_to_bound_ratio": float(np.max(error / allowance)),
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--run-on-device", action="store_true")
    for name in ("jobs", "model", "output"):
        p.add_argument("--" + name, required=True, type=Path)
    p.add_argument("--samples", type=int, default=5)
    args = p.parse_args()
    if not args.run_on_device or os.geteuid() != 0 or not 1 <= args.samples <= 10:
        p.error("Frame root, --run-on-device and samples in1..10 required")
    graph = load_landmark_graph(args.model / "hand.ncnn.param", args.model / "hand.ncnn.bin")
    jobs = []
    builds = []
    for mode in range(4):
        folder = args.jobs / str(mode)
        r = json.loads((folder / "build.json").read_text())
        j = Job.load(folder / "probe.fjob")
        if (
            hashlib.sha256(j.code).hexdigest() != r["sha256"]
            or f"-DMODEL_MODE={mode}" not in r["flags"]
        ):
            p.error("build/mode differs")
        for path, sha in r["sources"].items():
            if digest(ROOT / path) != sha:
                p.error("probe source pin differs")
        jobs.append(j)
        builds.append(r)
    report = {
        "format": "framenpu-hmx-model-probe",
        "model_sha256": graph.weights_sha256,
        "param_sha256": graph.param_sha256,
        "builds": builds,
        "source_sha256": {
            n: digest(ROOT / n)
            for n in ["tests/hardware_hmx_model.py", "tests/hardware_ieee_probe.py"]
        },
        "conditions": "public weights, deterministic synthetic activations; no private images or recordings",
        "audits": {"restored_jobs": 0, "restored_hmx_jobs": 0},
        "cases": [],
        "service_before": service(),
    }
    failed = False
    lease = _lease()
    try:
        with PowerSnapshot() as power:
            baseline = power.read()
            report["initial_health"] = settled_health(power, baseline)
            try:
                with Device() as observer:
                    if bytes(observer._transport.memory[CONTROL : CONTROL + 8]) == RECORD:
                        raise ValueError("resident refusal record present")
                    before = bytes(observer._transport.memory[ARENA_START:ARENA_END])
                report["arena_before_sha256"] = hashlib.sha256(before).hexdigest()
                with audited_device(power, report["audits"])(dispatch="direct") as dev:

                    def run(job, mode, data, repeat, *, fault=False, reject=False):
                        seen = []

                        def validate(result):
                            completion(result, job, fault=fault, reject=reject, output_bytes=6400)
                            seen.append(result)

                        try:
                            res = dev._submit(
                                job,
                                data,
                                opcode=100,
                                params=(1, mode, repeat, 0, 0, int(fault)),
                                output_size=6400,
                                use_direct=True,
                                _completion_validator=validate,
                            )
                        except JobFailed:
                            if (
                                not seen
                                or not (fault or reject)
                                or seen[0].workers[0][2] != (2 if fault else 1)
                            ):
                                raise
                            return seen[0], None
                        if fault or reject:
                            raise ValueError("expected fault/rejection missing")
                        m = struct.unpack_from("<64I", res.output)
                        body = (m[8] | m[9] << 32) - (m[6] | m[7] << 32)
                        if (
                            m[0] != 0x484D5031
                            or not 0x87A92000 <= m[1] < 0x87A92000 + len(job.code)
                            or m[2] != mode
                            or m[3] != struct.unpack_from("<I", data)[0]
                            or m[4] != repeat
                            or m[5]
                            or m[10] != repeat
                            or any(m[11:])
                            or not 0 <= body <= res.kernel_reference_ticks
                        ):
                            raise ValueError(
                                "HMX metadata/finite/completion/budget differs after restoration"
                            )
                        return res, body

                    fixture = cases(graph)
                    for name, a, b, bias, role in fixture:
                        start = time.monotonic_ns()
                        data = payload(a, b, bias)
                        packing = (time.monotonic_ns() - start) / 1e6
                        pair_hashes = {
                            hashlib.sha256(pack(x) + pack(y)).hexdigest() for x, y in zip(a, b)
                        }
                        if len(pair_hashes) != len(a):
                            raise ValueError("stream case repeats a depth operand pair")
                        entry = {
                            "name": name,
                            "role": role,
                            "depth": len(a),
                            "distinct_depth_pairs": len(pair_hashes),
                            "payload_sha256": hashlib.sha256(data).hexdigest(),
                            "payload_bytes": len(data),
                            "host_pack_ms": packing,
                            "samples": [],
                        }
                        report["cases"].append(entry)
                        # Checked warmups; then alternate mode/count order across
                        # rounds to reduce simple drift confounding.
                        for mode, job in enumerate(jobs):
                            res, _ = run(job, mode, data, 1)
                            validate_values(res, a, b, bias, mode)
                        for round_ in range(args.samples):
                            for mode in range(4) if round_ % 2 == 0 else reversed(range(4)):
                                for repeat in (1, 8) if round_ % 2 else (8, 1):
                                    res, body = run(jobs[mode], mode, data, repeat)
                                    accuracy = validate_values(res, a, b, bias, mode)
                                    entry["samples"].append(
                                        {
                                            "mode": mode,
                                            "repeat": repeat,
                                            "round": round_,
                                            "pipeline_us": body / 19.2,
                                            "wrapper_ms": res.kernel_reference_ticks / 19200,
                                            "error": accuracy,
                                        }
                                    )
                        print(
                            json.dumps({"case": name, "depth": len(a), "passed": True}), flush=True
                        )
                        args.output.write_text(json.dumps(report, indent=2) + "\n")
                    data = payload(fixture[0][1], fixture[0][2], fixture[0][3])
                    for mode, job in enumerate(jobs):
                        run(job, mode, data, 1, fault=True)
                        run(job, (mode + 1) % 4, data, 1, reject=True)
                    report["guarded_faults"] = 4
                    report["wrong_mode_rejections"] = 4
                    res, _ = run(jobs[1], 1, data, 1)
                    validate_values(res, *fixture[0][1:4], 1)
                with Device() as observer:
                    after = bytes(observer._transport.memory[ARENA_START:ARENA_END])
                report["arena_after_sha256"] = hashlib.sha256(after).hexdigest()
                if after != before:
                    raise ValueError("resident arena changed")
                report["final_health"] = settled_health(power, baseline)
                report["service_after"] = service()
                if report["service_before"] != report["service_after"]:
                    raise ValueError("DSP service changed")
                report["all_checks_passed"] = True
            except BaseException as exc:
                failed = True
                report["error"] = f"{type(exc).__name__}: {exc}"
                report["after_failure"] = failure_snapshot(power)
                report["interpretation"] = "stopped without retry/recovery/restart"
    finally:
        lease.close()
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(
        json.dumps(
            {
                "all_checks_passed": report.get("all_checks_passed", False),
                "error": report.get("error"),
            }
        ),
        flush=True,
    )
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
