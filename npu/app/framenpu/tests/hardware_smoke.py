"""Execute only on the authorized Frame: PYTHONPATH=src python3 tests/hardware_smoke.py."""

import argparse
import json
from pathlib import Path
import random
import struct
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from framenpu import Device, Job, Model, Dense, JobFailed

ROOT = Path(__file__).resolve().parents[1]


def half(x):
    return struct.unpack("<e", struct.pack("<e", x))[0]


def fp32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def reference(a, b, bias=None, relu=False):
    m, k, n = len(a), len(a[0]), len(b[0])
    out = []
    for i in range(m):
        row = []
        for j in range(n):
            value = 0.0
            for t in range(0, k, 32):
                partial = half(sum(half(a[i][q]) * half(b[q][j]) for q in range(t, min(t + 32, k))))
                value = fp32(value + partial)
            value = fp32(value + (0 if bias is None else bias[j]))
            row.append(max(0, value) if relu else value)
        out.append(row)
    return out


def equal(name, got, expected):
    error = max(abs(x - y) for row, ref in zip(got, expected) for x, y in zip(row, ref))
    if got != expected:
        raise AssertionError((name, error, got[:2], expected[:2]))
    print(
        json.dumps({"case": name, "rows": len(got), "cols": len(got[0]), "max_abs_error": error}),
        flush=True,
    )


def run(custom_only=False, dispatch="benchmark"):
    rng = random.Random(0x46504E55)
    with Device(dispatch=dispatch) as npu:
        print("INITIAL", json.dumps(npu.status()), flush=True)
        job = Job.load(ROOT / "examples/vector_add.fjob")
        data = struct.pack("<8f", 1, 2, 3, 4, 5, 6, 7, 8)
        result = npu.submit(job, data, output_size=16, params=(4,))
        if struct.unpack("<4f", result.output) != (6, 8, 10, 12):
            raise AssertionError("custom vector add")
        print(
            "CUSTOM_JOB",
            json.dumps(
                {
                    "output": list(struct.unpack("<4f", result.output)),
                    "roundtrip_ms": result.elapsed_ms,
                    "workers": result.workers,
                }
            ),
            flush=True,
        )
        try:
            npu.submit(npu.builtin(hmx=False), opcode=999)
        except JobFailed:
            print("EXPECTED_JOB_ERROR_RECOVERED", flush=True)
        else:
            raise AssertionError("invalid opcode was accepted")
        if not custom_only:
            for m, k, n in [(1, 1, 2), (4, 2, 1), (31, 17, 33), (32, 32, 32), (33, 65, 35)]:
                a = [[rng.randint(-4, 4) / 2 for j in range(k)] for i in range(m)]
                b = [[rng.randint(-4, 4) / 2 for j in range(n)] for i in range(k)]
                bias = [rng.randint(-4, 4) / 4 for j in range(n)]
                equal(
                    "gemm_%dx%dx%d" % (m, k, n), npu.matmul(a, b, bias=bias), reference(a, b, bias)
                )
            equal(
                "relu",
                [npu.elementwise([[-3, -1, 0, 1, 8]], activation="relu")[0]],
                [[0, 0, 0, 1, 8]],
            )
            equal(
                "clip",
                npu.elementwise([[-3, -1, 0, 1, 8]], activation="clip", minimum=-1, maximum=2),
                [[-1, -1, 0, 1, 2]],
            )
            model = Model(
                2, [Dense([[1, -1], [-1, 1]], activation="relu"), Dense([[1], [1]])], name="xor"
            )
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "xor.fnpu"
                model.save(path)
                loaded = Model.load(path)
                equal(
                    "loaded_xor_model",
                    loaded.run(npu, [[0, 0], [0, 1], [1, 0], [1, 1]]),
                    [[0], [1], [1], [0]],
                )
        final = npu.status()
        if final["control"] != [1, 0, 1] or final["callback"] != 0:
            raise AssertionError(final)
        print("FINAL", json.dumps(final), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--custom-only", action="store_true")
    parser.add_argument("--dispatch", choices=("benchmark", "direct"), default="benchmark")
    parser.add_argument(
        "--run-on-device",
        action="store_true",
        help="explicitly execute jobs on the connected, supported Frame",
    )
    args = parser.parse_args()
    if not args.run_on_device:
        parser.error("--run-on-device is required; this test changes DSP state")
    run(args.custom_only, args.dispatch)
