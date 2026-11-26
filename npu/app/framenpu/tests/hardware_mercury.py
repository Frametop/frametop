"""Device run of the Mercury keypoint backbone (Conv_19 .. Conv_139) through the
linked resident image built with ``tools/build_teacher_link.py --stride2 --s2-even``.

Cases are the private set staged by tools/mercury_device_cases.py: the FP16
host-prefix output and hint plane, and the host FP16-contract expectation.

The image is sealed once into the owned reservation 0x1ff0000..0x1ff4000;
every request goes through the slot loader with the completion, image and seal
checks of tests/hardware_teacher_link.py.

Phase A (qualify): every op runs as its own request (tests/mercury_layout.py
qualify segments). Each output is read back and checked against the per-value
FP16 bound computed from the device's own input. The chain carries device
bytes forward, including across the free Concat.

Phase B (merged): one request per frame with tensors resident in VTCM. The
first frame of each case must be bit-identical to phase A's final tensor; the
remaining rounds time the request. The host then runs the FP32 heads on the
device's 160x8x8 map and decodes keypoints against the FP32 and FP16-contract
references (statistics only).

Controls on the first case: a guarded fault in the merged request, a native
descriptor rejection, a rejection for every extended stride-2 word, a second
seal refused, then a clean frame. After the cases: retire, then an unsealed
request refused by the loader.

--prefix (step 2b) also runs Conv_0 .. Conv_18 on the DSP from host im2col stem
columns of the FP16 image, against the full FP16 contract (no host prefix).
A --s2-direct build (direct stride-2 depthwise) runs the same plans; each case
records the SHA256 of its merged final tensor so runs on two images can be
compared bit for bit.
An RS_PW_PROFILE build (timing instrument) adds the 1x1 phase tick totals of
each merged frame (parameter copy, output zeroing, B gather, HMX, post).
Each merged frame records its host phases (staging, request with the
device.py submit marks, readback). --held-power powers HMX once for the
session (device._hold_power, verified read-only before every request,
released under the gate before the retire) instead of per request.
--batch 2 runs two crops per merged request. With an RS_REPLAY image, --batch
is a maximum (up to 3): requests mix 1..batch crops across rounds, with no
padding. Each crop must equal its own qualify result; crop-end ticks bound
the last crop's descriptor times and give each crop's body time.
--stop-after LAYER truncates both phases (early-layer pilot, no heads).
Protections as tests/hardware_teacher_link.py: lease, refusal record, audited
direct dispatch, no retry/recovery/restart, window and reservation restored
and the whole arena compared with its starting hash. Only statistics are
written.
"""

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "src"), str(ROOT / "tests"), str(ROOT / "tools")]
from framenpu import Device
from framenpu import RecoveryRequired, JobFailed
from framenpu.resident_gemm import _lease, RECORD
from framenpu.resident_landmark import CONTROL, ARENA_START, ARENA_END
from hardware_hmx import PowerSnapshot, audited_device
from hardware_resident import settled_health
from hardware_landmark import digest, service
from hmx_multi_run import failure_snapshot
import hardware_teacher_link as HL
import mercury_layout as ML
import teacher_tail_layout as T
import teacher_resident_layout as RL

PW_BUCKETS = (
    "param_copy",
    "zero_output",
    "b_gather",
    "hmx",
    "post",
    "gather_barrier",
    "pre_gather_barrier",
    "post_barrier",
    "hmx_store_barrier",
)
SOURCES = (
    "tests/hardware_mercury.py",
    "tests/hardware_teacher_link.py",
    "tests/mercury_layout.py",
    "tests/teacher_resident_layout.py",
    "tests/teacher_tail_layout.py",
    "src/framenpu/mercury.py",
)


class _SubmitHooks:
    """The device as LinkRunner sees it: every _submit records its host phase
    times (device.py marks) and, while the runner holds HMX power for the
    session, passes that owner so the request verifies instead of cycling power."""

    def __init__(self, dev, runner):
        self._dev, self._runner = dev, runner

    def __getattr__(self, name):
        return getattr(self._dev, name)

    def _submit(self, *args, **kw):
        phases = {}
        if self._runner.held_power is not None:
            kw["_held_power"] = self._runner.held_power
        self._runner.last_phases = phases
        t0 = time.perf_counter()
        try:
            return self._dev._submit(*args, _phase_times=phases, **kw)
        finally:
            phases["submit_total_ms"] = (time.perf_counter() - t0) * 1e3


class MercuryRunner(HL.LinkRunner):
    """LinkRunner's seal/admin/linked submit, without the teacher plans."""

    def __init__(self, dev, mem, loader, image, profile=False, replay=False):
        self.dev, self.mem = _SubmitHooks(dev, self), mem
        self.held_power, self.last_phases = None, None
        self.loader, self.image = loader, image
        self.reservation = image + bytes(HL.CODE_BYTES - len(image))  # zero seal vector
        self.sealed = False
        self.stride2, self.split_after, self.dw_blocks = True, None, 2
        self.profile, self.replay = profile, replay

    def submit(
        self,
        plan,
        job,
        *,
        fault=False,
        reject=False,
        linked=False,
        refused=False,
        crops=1,
        replay_words=None,
    ):
        if not linked or not (self.profile or self.replay):
            if crops != 1 or replay_words is not None:
                raise ValueError("crop replay needs an RS_REPLAY linked image")
            return super().submit(
                plan, job, fault=fault, reject=reject, linked=linked, refused=refused
            )
        if replay_words is not None and not (self.replay and reject):
            raise ValueError("raw replay words are rejection controls only")
        for off, data in job.staged.items():
            self.put(off, data)
        data = RL.payload(plan, crops)
        if replay_words is not None:
            data = bytearray(data)
            struct.pack_into("<2I", data, 12, *replay_words)
            data = bytes(data)
        sealed, seen = self.sealed, []

        def validate(result):
            HL.linked_completion(
                result,
                len(self.image),
                output_bytes=T.META_BYTES,
                fault=fault,
                reject=reject or refused,
            )
            self._check_reservation(sealed)
            seen.append(result)

        t0 = time.perf_counter()
        try:
            res = self.dev._submit(
                self.loader,
                data,
                opcode=RL.RS_OPCODE,
                params=(1, job.first, job.end, 0, 0, int(fault)),
                output_size=T.META_BYTES,
                use_direct=True,
                _completion_validator=validate,
                _prepare=lambda t: self._check_reservation(sealed),
            )
        except JobFailed:
            if (
                not seen
                or not (fault or reject or refused)
                or seen[0].workers[0][2] != (2 if fault else 1)
            ):
                raise
            return None
        wall = time.perf_counter() - t0
        if fault or reject or refused:
            raise ValueError("expected fault/rejection missing")
        return self._resident_metadata(plan, job, res, wall, crops)

    def _resident_metadata(self, plan, job, res, wall, crops):
        """Replay keeps the last crop's cumulative descriptor ticks in 96..255
        and each crop's cumulative end in 48..50 (zero for a one-crop request).
        Validate boundaries before subtracting the preceding crop's end."""
        m = struct.unpack_from("<256I", res.output)
        ref = res.kernel_reference_ticks
        n = job.end - job.first
        ticks = list(m[96 : 96 + n])
        ends = list(m[48 : 48 + crops]) if crops > 1 else [ticks[-1]]
        base = ends[-2] if crops > 1 else 0
        if (
            not HL.IMAGE_CACHED <= m[1] < HL.IMAGE_CACHED + len(self.image)
            or any(m[64:96])
            or m[0] != RL.RS_MAGIC
            or m[2] != n * crops
            or (any(m[3:12]) if not self.profile else sum(m[3:12]) > ticks[-1])
            or any(m[12:48])
            or any(m[48 + crops : 64] if crops > 1 else m[48:64])
            or any(m[96 + n :])
            or any(b < a for a, b in zip(ticks, ticks[1:]))
            or any(b <= a for a, b in zip([0] + ends, ends))
            or not base < ticks[0] <= ticks[-1] == ends[-1] <= min(ref, 384000)
        ):
            raise ValueError(f"{job.ops}: resident metadata/crop boundaries/budget differs")
        per = [b - a for a, b in zip([base] + ticks, ticks)]
        row = {
            "kind": "rs",
            "wall_ms": wall * 1e3,
            "wrapper_ms": ref / 19200,
            "body_ms": ticks[-1] / 19200,
            "op_us": [x / 19.2 for x in per],
        }
        if self.replay:
            row |= {
                "crops": crops,
                "crop_end_ticks": ends,
                "crop_body_ms": [b / 19200 - a / 19200 for a, b in zip([0] + ends, ends)],
                "op_us_crop_index": crops - 1,
            }
        if self.profile:
            row["pw_profile_us"] = dict(zip(PW_BUCKETS, (x / 19.2 for x in m[3:12])))
        return row

    def get(self, off, n):
        return bytes(self.mem[off : off + n])

    def install(self, mplan):
        for off, data in RL.setup_writes(mplan.plan).items():
            self.put(off, data)

    def request(self, mplan, job, **kw):
        return self.submit(mplan.plan, job, linked=True, **kw)

    def tensor(self, mplan, name):
        out, stray = ML.read_tensor(mplan, self.get, name)
        if stray:
            raise ValueError(f"{name}: {stray} nonzero padding halves")
        return out

    def raw(self, mplan, name):
        g = mplan.grid(name)
        return self.get(mplan.where[name], g.tensor_bytes(mplan.shape[name][0]))


def load_case(cases, entry):
    import numpy as np

    data = (cases / f"{entry['name']}.npz").read_bytes()
    if hashlib.sha256(data).hexdigest() != entry["sha256"]:
        raise ValueError(f"case hash differs: {entry['name']}")
    with np.load(io.BytesIO(data), allow_pickle=False) as z:
        return {k: z[k] for k in z.files}


def host_inputs(case, prefix):
    """The host-staged tensors of a case: stem columns (prefix) or the FP32-prefix
    output, plus the hint plane."""
    if prefix:
        return {ML.STEM_COLS: ML.stem_columns(case["image"]), ML.HINT: case["hint"]}
    return {ML.INPUT: case["input"], ML.HINT: case["hint"]}


def qualify(runner, model, segments, case, dump=None):
    """Per-op requests; returns (device tensors, raw bytes, bound ratios, request rows).
    On a bound failure with ``dump`` set, the op's device input/output, exact value and
    allowance go to a private ``dump/<op>.npz`` (never into the report)."""
    import numpy as np

    host = host_inputs(case, segments[0].prefix)
    t = {k: np.asarray(v, np.float64) for k, v in host.items()}
    raw, ratios, rows = {}, {}, []
    for seg in segments:
        runner.install(seg)
        staged = {}
        for b in seg.external:
            if b in seg.composites:
                parts = {n: raw[n] if src == "device" else host[n] for n, src in seg.composites[b]}
                staged[b] = ML.compose(seg, b, parts)
                t[b] = np.concatenate([t[n] for n, _ in seg.composites[b]], axis=0)
            elif b in host:
                staged[b] = ML.tensor_bytes(seg, b, host[b])
            else:
                staged[b] = raw[b]
        ML.stage_inputs(seg, runner.put, staged)
        for b in seg.external:
            if runner.get(seg.where[b], len(staged[b])) != staged[b]:
                raise ValueError(f"staged {b} readback differs")
        for job, op in zip(seg.plan.jobs, seg.ops):
            rows.append(runner.request(seg, job))
            out = runner.tensor(seg, op.output)
            raw[op.output] = runner.raw(seg, op.output)
            if not np.isfinite(out).all():
                raise ValueError(f"{op.name}: non-finite output")
            x = t[op.inputs[0]]
            res = t[op.inputs[1]] if len(op.inputs) > 1 else None
            exact, allow = ML.exact(op, x, res), ML.bound(op, x, res)
            ratio = float(np.max(np.abs(out - exact) / allow))
            if ratio > 1 and dump is not None:
                dump.mkdir(parents=True, exist_ok=True)
                extra = {} if res is None else {"res": res}
                np.savez(dump / f"{op.name}.npz", x=x, out=out, exact=exact, allow=allow, **extra)
            if ratio > 1:
                raise ValueError(f"{op.name}: outside the FP16 bound (ratio {ratio:.3g})")
            ratios[op.name] = ratio
            t[op.output] = out
    return t, raw, ratios, rows


def frame(runner, merged, cases):
    """One merged request over 1..merged.batch crops on replay; returns
    (request row, final bytes per crop, host wall ms)."""
    count = merged.table_crops(len(cases))
    t0 = time.perf_counter()
    for k, case in enumerate(cases):
        ML.stage_inputs(merged.crop(k), runner.put, host_inputs(case, merged.prefix))
    t1 = time.perf_counter()
    row = runner.request(merged, merged.plan.jobs[0], **({"crops": count} if merged.replay else {}))
    t2 = time.perf_counter()
    finals = [runner.raw(merged.crop(k), merged.final) for k in range(count)]
    t3 = time.perf_counter()
    row["crops"] = count
    row["host_ms"] = {
        "stage": (t1 - t0) * 1e3,
        "request": (t2 - t1) * 1e3,
        "readback": (t3 - t2) * 1e3,
    } | (getattr(runner, "last_phases", None) or {})
    return row, finals, (t3 - t0) * 1e3


def keypoint_drift(model, out, ref):
    """Worst/mean joint distance (128 px crop) and scalar drift between two output sets."""
    import numpy as np
    from framenpu import mercury

    a, b = mercury.decode(out), mercury.decode(ref)
    d = np.linalg.norm(a["crop_px"] - b["crop_px"], axis=1)
    return {
        "worst_joint_px": float(d.max()),
        "mean_joint_px": float(d.mean()),
        "is_hand": float(abs(a["is_hand"] - b["is_hand"])),
        "curls": float(np.max(np.abs(out["curls"] - ref["curls"]))),
        "is_hand_flip": bool((a["is_hand"] >= 0.97) != (b["is_hand"] >= 0.97)),
    }


def controls(runner, merged, case, expect):
    plan = merged.plan
    rs = plan.jobs[0]
    runner.install(merged)
    for k in range(merged.batch):
        ML.stage_inputs(merged.crop(k), runner.put, host_inputs(case, merged.prefix))
    runner.request(merged, rs, fault=True)
    table = bytearray(plan.table)
    k = next(n for n in range(rs.end) if struct.unpack_from("<I", table, 128 * n)[0] == 1)
    struct.pack_into("<I", table, 128 * k + 36, T.WIN_HI)  # pw parameter source outside
    runner.put(plan.table_at, bytes(table))
    runner.request(merged, rs, reject=True)
    count = 0
    for n in range(rs.end):
        if struct.unpack_from("<I", plan.table, 128 * n)[0] != RL.KIND["s2"]:
            continue
        for word in range(15, 21):
            table = bytearray(plan.table)
            struct.pack_into("<I", table, 128 * n + 4 * word, 0x4000)
            runner.put(plan.table_at, bytes(table))
            runner.request(merged, rs, reject=True)
            count += 1
    runner.install(merged)
    replay_controls = {}
    if merged.replay:
        # The aligned stride passes the header bound, but crop 2's window
        # addresses leave WIN_HI. This exercises shifted descriptor validation.
        for name, words in {
            "four_crops": (4, plan.crop_stride),
            "misaligned_stride": (2, plan.crop_stride + 1),
            "out_of_window_stride": (3, (T.WIN_HI - T.WIN_LO) // 2),
        }.items():
            runner.request(merged, rs, reject=True, replay_words=words)
            replay_controls[name] = True
    runner.admin(7, reject=True)  # already sealed
    _, finals, _ = frame(runner, merged, [case] * merged.batch)
    if any(f != expect for f in finals):
        raise ValueError("clean merged frame after controls differs")
    return {
        "guarded_fault_linked": 1,
        "descriptor_rejection_linked": 1,
        "stride2_descriptor_rejections": count,
        "second_seal_refused": 1,
        "clean_after_controls": True,
        **({"replay_rejections": replay_controls} if merged.replay else {}),
    }


def run(
    runner,
    model,
    cases,
    manifest,
    rounds,
    stop_after,
    prefix=False,
    dump=None,
    batch=1,
    pw_group=1,
    replay=False,
):
    """Phase A qualifies every case; phase B then runs the merged frames. With
    batch > 1, case i's frames put cases i, i+1, ... (cyclic) into the crops of one
    request, and every crop's final must equal its own case's qualify result."""
    import numpy as np
    from framenpu import mercury

    segments = ML.qualify_segments(model, stop_after, prefix, pw_group=pw_group)
    merged = ML.build_plan(
        model, "merged", stop_after, prefix=prefix, batch=batch, pw_group=pw_group, replay=replay
    )
    full = merged.final == ML.OUTPUT
    contract = "full" if prefix else ""  # FP16 contract with / without the FP32 host prefix
    entries = manifest["cases"]
    loaded, raws, devs, out = [], [], [], {}
    for i, entry in enumerate(entries):
        case = load_case(cases, entry)
        t, raw, ratios, qrows = qualify(runner, model, segments, case, dump)
        expected = (
            ML.tensor_bytes(merged, merged.final, case["expected" + contract]) if full else None
        )
        out[f"case{i:02d}"] = {
            "split": entry["split"],
            "hinted": entry["hinted"],
            "qualify": {
                "max_bound_ratio": max(ratios.values()),
                "bound_ratio": ratios,
                "requests": len(qrows),
                "matches_host_emulator": (raw[merged.final] == expected) if full else None,
            },
        }
        loaded.append(case)
        raws.append(raw[merged.final])
        devs.append(t.get(ML.OUTPUT))
    for i, entry in enumerate(entries):
        row, case = out[f"case{i:02d}"], loaded[i]
        group = [(i + k) % len(entries) for k in range(batch)]
        runner.install(merged)  # setup, excluded from frame timing
        frames = []
        for r in range(rounds):
            count = 1 + (r + i) % batch if replay else batch
            actual = group[:count]
            req, finals, wall = frame(runner, merged, [loaded[g] for g in actual])
            for k, (g, fin) in enumerate(zip(actual, finals)):
                if fin != raws[g]:
                    raise ValueError(
                        f"{entry['name']} round {r} crop {k}: merged final differs from qualify"
                    )
            frames.append(req | {"host_wall_ms": wall, "batch_cases": actual})
            row["final_sha256"] = hashlib.sha256(finals[0]).hexdigest()  # A/B across images
            print(
                json.dumps(
                    {
                        "case": i,
                        "round": r,
                        "crops": count,
                        "wrapper_ms": req["wrapper_ms"],
                        "body_ms": req["body_ms"],
                        "host_wall_ms": wall,
                    }
                ),
                flush=True,
            )
        row["frames"] = frames
        if batch > 1:
            row["batch_cases"] = group
        if full:
            t0 = time.perf_counter()
            outs = mercury.run_tail(model, {ML.OUTPUT: devs[i]}, ML.LAST)
            row["host_tail_ms"] = (time.perf_counter() - t0) * 1e3
            fp32 = {k: case[f"fp32/{k}"] for k in mercury.OUTPUTS}
            fp16 = {k: case[f"fp16{contract}/{k}"] for k in mercury.OUTPUTS}
            row["vs_fp32"] = keypoint_drift(model, outs, fp32)
            row["vs_fp16_contract"] = keypoint_drift(model, outs, fp16)
            row["outputs_equal_fp16_contract"] = all(np.array_equal(outs[k], fp16[k]) for k in fp16)
        if i == 0:
            row["controls"] = controls(runner, merged, case, raws[0])
    plans = {
        "merged": {
            "batch": merged.batch,
            "replay": merged.replay,
            "crop_stride": merged.plan.crop_stride,
            "descriptors": merged.descriptors,
            "vtcm_peak_vectors": merged.vtcm_peak,
            "window_end": f"{merged.plan.end:#x}",
            "ops": len(merged.ops),
        },
        "qualify_segments": [
            {
                "first": s.ops[0].name,
                "last": s.ops[-1].name,
                "requests": len(s.plan.jobs),
                "window_end": f"{s.plan.end:#x}",
            }
            for s in segments
        ],
    }
    return out, plans


def summarize(crops, merged_ops, batch=1):
    import numpy as np

    rows = list(crops.values())
    frames = [f for r in rows for f in r["frames"][1:]]
    counts = [f.get("crops", batch) for f in frames]
    body = [f["body_ms"] for f in frames]
    wrap = [f["wrapper_ms"] for r in rows for f in r["frames"][1:]]
    wall = [f["host_wall_ms"] for r in rows for f in r["frames"][1:]]
    per_op = np.median([f["op_us"] for r in rows for f in r["frames"][1:]], axis=0)
    prof = [f["pw_profile_us"] for r in rows for f in r["frames"][1:] if "pw_profile_us" in f]
    host = [f["host_ms"] for r in rows for f in r["frames"][1:]]
    s = {
        "host_ms_median": {k: float(np.median([h[k] for h in host])) for k in host[0]},
        "pw_profile_us_median": {k: float(np.median([p[k] for p in prof])) for k in PW_BUCKETS}
        if prof
        else None,
        "cases": len(rows),
        "max_bound_ratio": max(r["qualify"]["max_bound_ratio"] for r in rows),
        "merged_bit_identical_to_qualify": True,
        "matches_host_emulator": [r["qualify"]["matches_host_emulator"] for r in rows].count(True),
        "body_ms_median": float(np.median(body)),
        "wrapper_ms_median": float(np.median(wrap)),
        "host_wall_ms_median": float(np.median(wall)),
        "batch": batch,
        "body_ms_per_crop_median": float(np.median([v / n for v, n in zip(body, counts)])),
        "host_wall_ms_per_crop_median": float(np.median([v / n for v, n in zip(wall, counts)])),
        "descriptor_us_median": [round(float(v), 2) for v in per_op],
        "ops": merged_ops,
        "by_crop_count": {
            str(n): {
                "requests": len(fs),
                "body_ms_median": float(np.median([f["body_ms"] for f in fs])),
                "body_ms_per_crop_median": float(np.median([f["body_ms"] / n for f in fs])),
                "crop_body_ms_median": [
                    float(np.median([f["crop_body_ms"][k] for f in fs])) for k in range(n)
                ]
                if "crop_body_ms" in fs[0]
                else None,
                "host_wall_ms_median": float(np.median([f["host_wall_ms"] for f in fs])),
                "host_wall_ms_per_crop_median": float(
                    np.median([f["host_wall_ms"] / n for f in fs])
                ),
            }
            for n in sorted(set(counts))
            for fs in [[f for f in frames if f.get("crops", batch) == n]]
        },
    }
    if "vs_fp32" in rows[0]:
        w = [r["vs_fp32"]["worst_joint_px"] for r in rows]
        s |= {
            "vs_fp32_worst_joint_px": {
                "median": float(np.median(w)),
                "p95": float(np.percentile(w, 95)),
                "max": float(max(w)),
            },
            "vs_fp32_is_hand_flips": sum(r["vs_fp32"]["is_hand_flip"] for r in rows),
            "vs_fp32_curls_max": max(r["vs_fp32"]["curls"] for r in rows),
            "outputs_equal_fp16_contract": sum(r["outputs_equal_fp16_contract"] for r in rows),
            "host_tail_ms_median": float(np.median([r["host_tail_ms"] for r in rows])),
        }
    return s


def main():
    from framenpu import mercury

    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--run-on-device", action="store_true")
    for name in ("jobs", "model", "cases", "output"):
        p.add_argument("--" + name, required=True, type=Path)
    p.add_argument("--rounds", type=int, default=5)
    p.add_argument("--stop-after", default=ML.LAST, help="early-layer pilot: last op to run")
    p.add_argument("--max-cases", type=int, default=0, help="first N cases only (0 = all)")
    p.add_argument("--prefix", action="store_true", help="step 2b: Conv_0 .. on the DSP too")
    p.add_argument("--dump", type=Path, help="private directory for bound-failure tensors")
    p.add_argument(
        "--batch",
        type=int,
        default=1,
        help="crops per request; replay image: maximum, 1..3, mixed across rounds",
    )
    p.add_argument(
        "--held-power",
        action="store_true",
        help="hold HMX power for the session (device._hold_power) instead of per request",
    )
    args = p.parse_args()
    if not args.run_on_device or os.geteuid() != 0 or not 2 <= args.rounds <= 20:
        p.error("Frame root, --run-on-device and rounds in 2..20 required")
    loader_job, image, link_record = HL.load_link(args.jobs)
    if (
        not link_record.get("stride2")
        or not link_record.get("s2_even")
        or link_record.get("heads")
        or link_record.get("stem")
        or link_record.get("dw_blocks", 2) != 2
    ):
        p.error("--jobs must be a --stride2 --s2-even linked build without heads/stem")
    replay = bool(link_record.get("replay"))
    if not 1 <= args.batch <= (RL.MAX_CROPS if replay else 2):
        p.error("--batch is 1..3 for a replay image, otherwise 1 or 2")
    s2_direct = bool(link_record.get("s2_direct"))
    profile = bool(link_record.get("pw_profile"))
    pw_group = int(link_record.get("pw_group", 1))  # the build's 1x1 scratch layout
    model = mercury.load_keypoint(args.model)
    manifest = json.loads((args.cases / "manifest.json").read_text())
    if manifest["model_sha256"] != model.sha256 or manifest["host_prefix"] != ML.HOST_PREFIX:
        p.error("cases/model mismatch")
    if args.max_cases:
        manifest["cases"] = manifest["cases"][: args.max_cases]
    # validate first, against the checks of the build actually loaded
    for mp in [
        ML.build_plan(
            model,
            "merged",
            args.stop_after,
            prefix=args.prefix,
            batch=args.batch,
            pw_group=pw_group,
            replay=replay,
        )
    ] + list(ML.qualify_segments(model, args.stop_after, args.prefix, pw_group=pw_group)):
        for n in range(len(mp.plan.table) // 128):
            row = struct.unpack_from("<32I", mp.plan.table, 128 * n)
            if any(
                not RL.native_ok(
                    RL.replay_row(row, k, mp.plan.crop_stride),
                    s2_direct=ML.S2_PHASE if s2_direct else None,
                    pw_group=pw_group,
                )
                for k in range(mp.plan.max_crops)
            ):
                p.error(f"plan descriptor {n} would be rejected by this build")
    if args.prefix and "image" not in load_case(args.cases, manifest["cases"][0]):
        p.error("cases lack the FP16 image; regenerate with tools/mercury_device_cases.py")
    report = {
        "format": "framenpu-mercury-backbone-run",
        "scope": f"{ML.DSP_FIRST if args.prefix else ML.FIRST} .. {args.stop_after} on the DSP; "
        + ("host im2col stem columns" if args.prefix else "FP32 prefix staged")
        + ", host hint and heads",
        "prefix": args.prefix,
        "s2_direct": s2_direct,
        "pw_profile": profile,
        "pw_fast": bool(link_record.get("pw_fast")),
        "held_power": args.held_power,
        "batch": args.batch,
        "replay": replay,
        "pw_group": pw_group,
        "stop_after": args.stop_after,
        "builds": {"loader": link_record["loader"], "linked": link_record["linked"]},
        "model_sha256": model.sha256,
        "source_sha256": {n: digest(ROOT / n) for n in SOURCES},
        "cases_manifest_sha256": digest(args.cases / "manifest.json"),
        "window": [f"{T.WIN_LO:#x}", f"{T.WIN_HI:#x}"],
        "reservation": [f"{HL.CODE:#x}", f"{HL.CODE + HL.CODE_BYTES:#x}"],
        "audits": {"restored_jobs": 0, "restored_hmx_jobs": 0},
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
                    mem = observer._transport.memory
                    if bytes(mem[CONTROL : CONTROL + 8]) == RECORD:
                        raise ValueError("resident refusal record present")
                    before = bytes(mem[ARENA_START:ARENA_END])
                report["arena_before_sha256"] = hashlib.sha256(before).hexdigest()
                window = before[T.WIN_LO - ARENA_START : T.WIN_HI - ARENA_START]
                reservation = before[HL.CODE - ARENA_START : HL.CODE + HL.CODE_BYTES - ARENA_START]
                retired, unsafe = True, False
                try:
                    with audited_device(power, report["audits"])(dispatch="direct") as dev:
                        runner = MercuryRunner(
                            dev, dev._transport.memory, loader_job, image, profile, replay
                        )
                        try:
                            report["seal_wrapper_ms"] = runner.seal()
                            if args.held_power:
                                runner.held_power = dev._hold_power()
                            report["cases"], report["plans"] = run(
                                runner,
                                model,
                                args.cases,
                                manifest,
                                args.rounds,
                                args.stop_after,
                                args.prefix,
                                args.dump,
                                args.batch,
                                pw_group,
                                replay,
                            )
                        except RecoveryRequired:
                            unsafe = True
                            raise
                        finally:
                            if runner.held_power is not None:
                                held, runner.held_power = runner.held_power, None
                                if unsafe or dev._transport.poisoned:
                                    held.abandon()  # never gate clocks in an unknown state
                                else:
                                    dev._release_power(held)
                            if runner.sealed:
                                retired = False
                                if not unsafe and not dev._transport.poisoned:
                                    report["retire_wrapper_ms"] = runner.admin(8)
                                    retired = True
                        merged = ML.build_plan(
                            model,
                            "merged",
                            args.stop_after,
                            prefix=args.prefix,
                            batch=args.batch,
                            pw_group=pw_group,
                            replay=replay,
                        )
                        runner.install(merged)
                        runner.request(merged, merged.plan.jobs[0], refused=True)
                        report["unsealed_request_refused"] = True
                        report["summary"] = summarize(
                            report["cases"], len(merged.ops), merged.batch
                        )
                except RecoveryRequired:
                    unsafe = True
                    raise
                finally:
                    report["window_restored"] = False
                    if not unsafe and retired:
                        with Device() as observer:
                            mem = observer._transport.memory
                            mem[T.WIN_LO : T.WIN_HI] = window
                            mem[HL.CODE : HL.CODE + HL.CODE_BYTES] = reservation
                            after = bytes(mem[ARENA_START:ARENA_END])
                        report["window_restored"] = True
                        report["arena_after_sha256"] = hashlib.sha256(after).hexdigest()
                    report["reservation_restored"] = not unsafe and retired
                if after != before:
                    raise ValueError("arena differs after window/reservation restoration")
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
        args.output.write_text(json.dumps(report, indent=1) + "\n")
    print(
        json.dumps(
            {
                "all_checks_passed": report.get("all_checks_passed", False),
                "error": report.get("error"),
                "summary": report.get("summary"),
            },
            indent=1,
        ),
        flush=True,
    )
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
