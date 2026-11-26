"""Offline checks of the Mercury backbone plan (tests/mercury_layout.py) and of the
device harness flow (tests/hardware_mercury.py) against the lane simulator.

Needs NumPy and the pinned model in FNPU_MERCURY_MODEL; no device, no corpus.
"""

import os
from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "src"), str(ROOT / "tests"), str(ROOT / "tools")]

MODEL = os.environ.get("FNPU_MERCURY_MODEL")

try:
    import numpy as np
except ImportError:  # pragma: no cover
    np = None


def synthetic_case(model, seed, hinted=True):
    from framenpu import mercury
    import mercury_layout as ML

    rng = np.random.default_rng(seed)
    img = rng.uniform(-1, 1, (1, 1, 128, 128)).astype(np.float32)
    feeds = {
        "inputImg": img,
        "lastKeypoints": rng.uniform(0, 1, (1, 42)).astype(np.float32),
        "useLastKeypoints": np.full(1, float(hinted), np.float32),
    }
    fp16, t = mercury.run(model, feeds, "fp16", keep=True, host_prefix=ML.HOST_PREFIX)
    fp32 = mercury.run(model, feeds, "fp32")
    case = {
        "input": t[ML.INPUT].astype(np.float16),
        "hint": t[ML.HINT].astype(np.float16),
        "expected": t[ML.OUTPUT].astype(np.float16),
    }
    case |= {f"fp32/{k}": v for k, v in fp32.items()}
    case |= {f"fp16/{k}": v for k, v in fp16.items()}
    full, tf = mercury.run(model, feeds, "fp16", keep=True)
    case["image"] = tf["inputImg"].reshape(128, 128).astype(np.float16)
    case["expectedfull"] = tf[ML.OUTPUT].astype(np.float16)
    case |= {f"fp16full/{k}": v for k, v in full.items()}
    return case, t


class SimRunner:
    """MercuryRunner's interface over a simulated window (no device)."""

    def __init__(self):
        import teacher_tail_layout as T

        self.T = T
        self.window = bytearray(T.WIN_HI - T.WIN_LO)
        self.sealed = True
        self.events = []

    def put(self, off, data):
        self.window[off - self.T.WIN_LO : off - self.T.WIN_LO + len(data)] = data

    def get(self, off, n):
        return bytes(self.window[off - self.T.WIN_LO : off - self.T.WIN_LO + n])

    def install(self, mplan):
        import teacher_resident_layout as RL

        for off, data in RL.setup_writes(mplan.plan).items():
            self.put(off, data)

    def request(
        self, mplan, job, fault=False, reject=False, refused=False, crops=1, replay_words=None
    ):
        import dataclasses
        import mercury_layout as ML
        import teacher_resident_layout as RL

        one = dataclasses.replace(mplan.plan, jobs=[job])
        if fault or refused:
            self.events.append("fault" if fault else "refused")
            return None
        if replay_words is not None:
            crops, stride = replay_words
            if crops > 3 or stride & 4095 or stride > (self.T.WIN_HI - self.T.WIN_LO) // 2:
                self.events.append("reject")
                return None
            one = dataclasses.replace(one, crop_stride=stride, max_crops=3)
        if reject:
            try:
                RL.simulate(one, self.window, s2_phase=ML.S2_PHASE, crops=crops)
            except ValueError:
                self.events.append("reject")
                return None
            raise AssertionError("descriptor was not rejected")
        RL.simulate(one, self.window, s2_phase=ML.S2_PHASE, crops=crops)
        n = job.end - job.first
        return {
            "kind": "rs",
            "wall_ms": float(crops),
            "wrapper_ms": float(crops),
            "body_ms": float(crops),
            "op_us": [1.0] * n,
            **({"crop_body_ms": [1.0] * crops} if mplan.replay else {}),
        }

    def admin(self, command, reject=False):
        self.events.append(("admin", command, reject))

    tensor = None  # bound below
    raw = None


def _bind():
    import hardware_mercury as HM

    SimRunner.tensor = HM.MercuryRunner.tensor
    SimRunner.raw = HM.MercuryRunner.raw


class MercuryReplayMetadataTest(unittest.TestCase):
    def test_crop_boundaries_and_partial_replay_are_checked(self):
        from types import SimpleNamespace
        import hardware_mercury as HM
        import hardware_teacher_link as HL
        import teacher_resident_layout as RL

        runner = object.__new__(HM.MercuryRunner)
        runner.image, runner.profile, runner.replay = bytes(256), False, True
        job = SimpleNamespace(first=2, end=5, ops=["test"])
        for count in (1, 2, 3):
            m = [0] * 256
            m[:3] = [RL.RS_MAGIC, HL.IMAGE_CACHED + 4, 3 * count]
            ends = [19200 * (k + 1) for k in range(count)]
            if count > 1:
                m[48 : 48 + count] = ends
            m[96:99] = [ends[-1] - 12000, ends[-1] - 6000, ends[-1]]

            def parse(words):
                res = SimpleNamespace(
                    output=struct.pack("<256I", *words), kernel_reference_ticks=60000
                )
                return runner._resident_metadata(None, job, res, 0.01, count)

            row = parse(m)
            self.assertEqual(row["crop_end_ticks"], ends)
            self.assertEqual(row["crop_body_ms"], [1.0] * count)
            self.assertEqual(sum(row["op_us"]), 1000.0)
            mutations = [
                (2, 3 * count - 1),
                (51, 1),
                (64, 1),
                (99, 1),
                (97, m[96] - 1),
                (98, 60001),
                (3, 1),
            ]
            if count > 1:
                mutations += [(48, 0), (48 + count - 1, ends[-1] - 1), (96, ends[-2] - 1)]
            else:
                mutations += [(48, ends[0])]
            for word, value in mutations:
                bad = m.copy()
                bad[word] = value
                with self.subTest(crops=count, word=word), self.assertRaises(ValueError):
                    parse(bad)


@unittest.skipUnless(np is not None and MODEL, "needs NumPy and FNPU_MERCURY_MODEL")
class MercuryLayoutTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from framenpu import mercury

        cls.model = mercury.load_keypoint(MODEL)

    def test_merged_plan_fits_and_is_native(self):
        import mercury_layout as ML
        import teacher_resident_layout as RL
        import teacher_tail_layout as T

        mp = ML.build_plan(self.model)
        self.assertEqual(len(mp.ops), 36)
        self.assertEqual(mp.descriptors, 39)  # load, 36 ops, hint load, store
        self.assertLessEqual(mp.vtcm_peak, RL.SPAN)
        self.assertLessEqual(mp.plan.end, T.WIN_HI)
        self.assertEqual(mp.external, [ML.INPUT])
        self.assertTrue(mp.needs_hint)
        # hint plane sits directly after the 32 feature pairs in the window
        g = mp.grid(ML.FEATURES)
        self.assertEqual(mp.where[ML.HINT] - mp.where[ML.CONCAT], 128 * 32 * g.nbin)
        tab = np.frombuffer(mp.plan.table, "<u4").reshape(-1, 32)
        self.assertTrue(all(RL.native_ok(r) for r in tab))
        kinds = [int(r[0]) for r in tab]
        self.assertEqual(kinds.count(RL.KIND["s2"]), 2)
        self.assertEqual((kinds[0], kinds[-1]), (RL.KIND["load"], RL.KIND["store"]))

    def test_merged_simulation_bit_exact(self):
        import mercury_layout as ML

        mp = ML.build_plan(self.model)
        for seed, hinted in ((1, True), (2, False)):
            case, t = synthetic_case(self.model, seed, hinted)
            out, stray = ML.simulate(mp, t)[ML.OUTPUT]
            self.assertEqual(stray, 0)
            np.testing.assert_array_equal(out, t[ML.OUTPUT])

    def test_even_phase_matters(self):
        import mercury_layout as ML
        import teacher_resident_layout as RL
        import teacher_tail_layout as T

        mp = ML.build_plan(self.model, stop_after="Conv_33")
        _, t = synthetic_case(self.model, 3)
        window = bytearray(T.WIN_HI - T.WIN_LO)
        for off, data in RL.setup_writes(mp.plan).items():
            window[off - T.WIN_LO : off - T.WIN_LO + len(data)] = data
        ML.stage_inputs(
            mp, lambda o, d: window.__setitem__(slice(o - T.WIN_LO, o - T.WIN_LO + len(d)), d), t
        )
        RL.simulate(mp.plan, window, s2_phase=1)  # teacher phase: wrong for Mercury
        off = mp.where[mp.final] - T.WIN_LO
        g = mp.grid(mp.final)
        odd, _ = g.from_bytes(bytes(window[off : off + g.tensor_bytes(192)]), 192)
        self.assertFalse(np.array_equal(odd, t[mp.final]))

    def test_qualify_segments_cover_range(self):
        import mercury_layout as ML
        import teacher_tail_layout as T

        segs = ML.qualify_segments(self.model)
        names = [op.name for s in segs for op in s.ops]
        self.assertEqual(names, [op.name for op in ML.device_ops(self.model)])
        self.assertTrue(all(s.plan.end <= T.WIN_HI and not s.needs_hint for s in segs))
        self.assertEqual(segs[0].ops[-1].output, ML.FEATURES)
        self.assertEqual(segs[1].external, [ML.CONCAT])

    def test_prefix_plan_fits_and_is_native(self):
        import mercury_layout as ML
        import teacher_resident_layout as RL
        import teacher_tail_layout as T

        mp = ML.build_plan(self.model, prefix=True)
        self.assertEqual(len(mp.ops), 52)  # 6 prefix ops, Conv_10/14 as 6 groups each
        self.assertEqual(mp.descriptors, 55)
        self.assertLessEqual(mp.vtcm_peak, RL.SPAN)
        self.assertLessEqual(mp.plan.end, T.WIN_HI)
        self.assertEqual((mp.external, mp.host_parts), ([ML.STEM_COLS], [ML.HINT]))
        self.assertNotIn(ML.EXPAND, mp.shape.keys() & {i for op in mp.ops for i in op.inputs})
        tab = np.frombuffer(mp.plan.table, "<u4").reshape(-1, 32)
        self.assertTrue(all(RL.native_ok(r) for r in tab))
        self.assertEqual([int(r[0]) for r in tab].count(RL.KIND["s2"]), 8)

    def test_prefix_simulation_bit_exact(self):
        """Conv_0 .. Conv_139 in one request against the full FP16 contract."""
        import mercury_layout as ML

        mp = ML.build_plan(self.model, prefix=True)
        for seed, hinted in ((5, True), (6, False)):
            case, _ = synthetic_case(self.model, seed, hinted)
            out, stray = ML.simulate(
                mp, {ML.STEM_COLS: ML.stem_columns(case["image"]), ML.HINT: case["hint"]}
            )[ML.OUTPUT]
            self.assertEqual(stray, 0)
            np.testing.assert_array_equal(out, case["expectedfull"].astype(np.float64))

    def test_stem_columns_match_dense_stem(self):
        from framenpu import mercury
        import mercury_layout as ML

        case, _ = synthetic_case(self.model, 7)
        stem = self.model.op("Conv_0")
        rewritten = ML.device_ops(self.model, "Conv_0", prefix=True)[0]
        cols = ML.stem_columns(case["image"]).astype(np.float32)
        img = case["image"].astype(np.float32)[None]
        for numerics in ("fp32", "fp16"):
            np.testing.assert_array_equal(
                mercury._conv(rewritten, cols, None, numerics),
                mercury._conv(stem, img, None, numerics),
            )

    def test_prefix_qualify_segments(self):
        import mercury_layout as ML

        segs = ML.qualify_segments(self.model, prefix=True)
        names = [op.name for s in segs for op in s.ops]
        self.assertEqual(names, [op.name for op in ML.device_ops(self.model, prefix=True)])
        self.assertEqual(segs[0].ops[-1].name, "Conv_14.g5")
        self.assertEqual(segs[1].external, [ML.DOWN])

    def test_prefix_harness_flow_on_simulator(self):
        import hardware_mercury as HM
        import mercury_layout as ML

        _bind()
        case, _ = synthetic_case(self.model, 8)
        runner = SimRunner()
        manifest = {"cases": [{"name": "s", "split": "synthetic", "hinted": True, "sha256": ""}]}
        orig = HM.load_case
        HM.load_case = lambda cases, entry: case
        try:
            rows, plans = HM.run(runner, self.model, None, manifest, 2, ML.LAST, prefix=True)
        finally:
            HM.load_case = orig
        row = rows["case00"]
        self.assertTrue(row["qualify"]["matches_host_emulator"])
        self.assertLess(row["qualify"]["max_bound_ratio"], 1)
        self.assertEqual(row["qualify"]["requests"], 52)
        self.assertTrue(row["outputs_equal_fp16_contract"])
        self.assertEqual(row["controls"]["stride2_descriptor_rejections"], 48)
        self.assertEqual(len(plans["qualify_segments"]), 4)

    def test_direct_stride2_model_matches(self):
        """RS_S2_DIRECT data path (word-level model) equals stride-1 + decimation."""
        import mercury_layout as ML

        for prefix in (False, True):
            mp = ML.build_plan(self.model, prefix=prefix)
            case, t = synthetic_case(self.model, 12 + prefix)
            ins = (
                {ML.STEM_COLS: ML.stem_columns(case["image"]), ML.HINT: case["hint"]}
                if prefix
                else t
            )
            a, _ = ML.simulate(mp, ins)[ML.OUTPUT]
            b, stray = ML.simulate(mp, ins, s2_direct=True)[ML.OUTPUT]
            self.assertEqual(stray, 0)
            np.testing.assert_array_equal(a, b)

    def test_pw_group_scratch(self):
        """An RS_PW_GROUP build's 1x1 scratch (B tiles + output tiles per block of the
        group) fits every op of the plan, and the mirrored native check rejects a
        scratch that would run past the span."""
        import mercury_layout as ML
        import teacher_resident_layout as RL

        for group in (1, 8):
            for batch in (1, 2):
                mp = ML.build_plan(self.model, prefix=True, batch=batch, pw_group=group)
                for n in range(mp.descriptors):
                    row = struct.unpack_from("<32I", mp.plan.table, 128 * n)
                    self.assertTrue(RL.native_ok(row, s2_direct=0, pw_group=group))
        op = next(o for o in ML.device_ops(self.model, prefix=True) if o.conv_class == "pointwise")
        gin = ML.grid(ML.shapes(self.model, True)[op.inputs[0]])
        img, one = ML.pw_image(op, gin)
        img8, eight = ML.pw_image(op, gin, 8)
        self.assertEqual(img, img8)
        self.assertGreater(eight, 8 * one)
        rows = [
            struct.unpack_from("<32I", ML.build_plan(self.model, prefix=True).plan.table, 128 * n)
            for n in range(ML.build_plan(self.model, prefix=True).descriptors)
        ]
        self.assertFalse(all(RL.native_ok(r, s2_direct=0, pw_group=1024) for r in rows))

    def test_batch_plan_crops_independent(self):
        """Two crops in one request: shared parameter images, own slots, and each
        crop bit-identical to its batch-1 result (different inputs, so crop 1
        cannot pass by reading crop 0's leftover VTCM)."""
        import mercury_layout as ML
        import teacher_resident_layout as RL

        one = ML.build_plan(self.model, prefix=True)
        two = ML.build_plan(self.model, prefix=True, batch=2)
        self.assertEqual(two.descriptors, 2 * one.descriptors)
        self.assertEqual(two.vtcm_peak, one.vtcm_peak)
        self.assertEqual(two.plan.fixed, one.plan.fixed)
        self.assertEqual(two.crop(0).where, one.where)
        a, b = two.crops
        self.assertFalse(set(a.values()) & set(b.values()))
        for n in range(two.descriptors):
            self.assertTrue(
                RL.native_ok(struct.unpack_from("<32I", two.plan.table, 128 * n), s2_direct=0)
            )
        ins = []
        for seed in (21, 22):
            case, _ = synthetic_case(self.model, seed, hinted=seed % 2 == 0)
            ins.append({ML.STEM_COLS: ML.stem_columns(case["image"]), ML.HINT: case["hint"]})
        for direct in (False, True):
            got = ML.simulate(two, ins, s2_direct=direct)
            for k in range(2):
                want, _ = ML.simulate(one, ins[k], s2_direct=direct)[ML.OUTPUT]
                out, stray = got[k][ML.OUTPUT]
                self.assertEqual(stray, 0)
                np.testing.assert_array_equal(out, want)
            self.assertFalse(np.array_equal(got[0][ML.OUTPUT][0], got[1][ML.OUTPUT][0]))
        with self.assertRaises(ValueError):
            ML.build_plan(self.model, prefix=True, batch=3)  # 165 descriptors
        with self.assertRaises(ValueError):
            ML.build_plan(self.model, "qualify", prefix=True, batch=2)

    def test_replay_plan_runs_one_to_three_crops(self):
        """RS_REPLAY: one crop's 55 descriptors, run 1, 2 or 3 times per request with
        the crop stride added to load/store addresses. Each crop (different inputs,
        VTCM carried over from the previous crop) is bit-identical to its batch-1
        result; the payload and the crop count are checked."""
        import mercury_layout as ML
        import teacher_resident_layout as RL

        one = ML.build_plan(self.model, prefix=True)
        rp = ML.build_plan(self.model, prefix=True, batch=3, replay=True, pw_group=8)
        self.assertEqual(rp.descriptors, one.descriptors)
        self.assertEqual(rp.plan.max_crops, 3)
        self.assertEqual(rp.plan.crop_stride % 4096, 0)
        self.assertEqual(rp.crop(0).where, one.where)
        for k in range(3):
            for b, at in rp.crops[k].items():
                self.assertEqual(at, one.where[b] + k * rp.plan.crop_stride)
        for n in range(rp.descriptors):
            row = struct.unpack_from("<32I", rp.plan.table, 128 * n)
            for k in range(3):
                self.assertTrue(
                    RL.native_ok(
                        RL.replay_row(row, k, rp.plan.crop_stride), s2_direct=0, pw_group=8
                    )
                )
        w = np.frombuffer(RL.payload(rp.plan, 3), "<u4")
        self.assertEqual(list(w[:5]), [RL.RS_MAGIC, rp.plan.table_at, 55, 3, rp.plan.crop_stride])
        self.assertFalse(np.frombuffer(RL.payload(rp.plan, 1), "<u4")[3:].any())
        for bad in (0, 4):
            with self.assertRaises(ValueError):
                RL.payload(rp.plan, bad)
        with self.assertRaises(ValueError):
            RL.payload(one.plan, 2)
        ins = []
        for seed in (31, 32, 33):
            case, _ = synthetic_case(self.model, seed, hinted=seed % 2 == 1)
            ins.append({ML.STEM_COLS: ML.stem_columns(case["image"]), ML.HINT: case["hint"]})
        want = [ML.simulate(one, x, s2_direct=True)[ML.OUTPUT][0] for x in ins]
        for n in (1, 2, 3):
            got = ML.simulate(rp, ins[:n], s2_direct=True)
            self.assertEqual(len(got), n)
            for k in range(n):
                out, stray = got[k][ML.OUTPUT]
                self.assertEqual(stray, 0)
                np.testing.assert_array_equal(out, want[k])
        self.assertFalse(np.array_equal(want[0], want[1]))
        with self.assertRaises(ValueError):
            ML.simulate(rp, ins + ins[:1], s2_direct=True)
        with self.assertRaises(ValueError):
            ML.build_plan(self.model, prefix=True, batch=4, replay=True)

    def test_batch_harness_flow_on_simulator(self):
        import hardware_mercury as HM
        import mercury_layout as ML

        _bind()
        cases = [synthetic_case(self.model, s)[0] for s in (31, 32)]
        runner = SimRunner()
        manifest = {
            "cases": [
                {"name": f"s{i}", "split": "synthetic", "hinted": True, "sha256": ""}
                for i in range(2)
            ]
        }
        orig = HM.load_case
        HM.load_case = lambda c, entry: cases[int(entry["name"][1:])]
        try:
            rows, plans = HM.run(runner, self.model, None, manifest, 2, ML.LAST, True, None, 2)
        finally:
            HM.load_case = orig
        self.assertEqual(plans["merged"]["batch"], 2)
        self.assertEqual(rows["case01"]["batch_cases"], [1, 0])
        self.assertTrue(all(r["outputs_equal_fp16_contract"] for r in rows.values()))
        s = HM.summarize(rows, len(ML.build_plan(self.model, prefix=True).ops), 2)
        self.assertEqual(s["batch"], 2)

    def test_replay_harness_mixes_request_counts(self):
        import hardware_mercury as HM
        from unittest.mock import patch

        _bind()
        cases = [synthetic_case(self.model, s)[0] for s in (31, 32, 33)]
        runner = SimRunner()
        manifest = {
            "cases": [
                {"name": f"s{i}", "split": "synthetic", "hinted": True, "sha256": ""}
                for i in range(3)
            ]
        }
        with patch.object(HM, "load_case", side_effect=lambda c, e: cases[int(e["name"][1:])]):
            rows, plans = HM.run(
                runner,
                self.model,
                None,
                manifest,
                4,
                "Conv_18",
                prefix=True,
                batch=3,
                pw_group=8,
                replay=True,
            )
        self.assertTrue(plans["merged"]["replay"])
        for i, row in enumerate(rows.values()):
            fs = row["frames"]
            self.assertEqual([f["crops"] for f in fs], [1 + (r + i) % 3 for r in range(4)])
            for f in fs:
                self.assertEqual(f["batch_cases"], [(i + k) % 3 for k in range(f["crops"])])
        self.assertEqual(len(rows["case00"]["controls"]["replay_rejections"]), 3)
        self.assertEqual(runner.events.count("reject"), 40)  # 36 s2 fields + descriptor + 3 replay
        summary = HM.summarize(rows, plans["merged"]["ops"], 3)
        self.assertEqual(summary["body_ms_per_crop_median"], 1.0)
        for n in (1, 2, 3):
            got = summary["by_crop_count"][str(n)]
            self.assertEqual(got["requests"], 3)
            self.assertEqual(got["body_ms_median"], float(n))
            self.assertEqual(got["crop_body_ms_median"], [1.0] * n)

    def test_direct_stride2_spill_check(self):
        """The direct build rejects an output plane too short for its zero-lane spill."""
        import mercury_layout as ML
        import teacher_resident_layout as RL

        mp = ML.build_plan(self.model, prefix=True)
        tab = np.frombuffer(mp.plan.table, "<u4").reshape(-1, 32).copy()
        row = next(r for r in tab if r[0] == RL.KIND["s2"] and r[19] == 8)  # 16 -> 8
        oblk, oh, ows, ow = int(row[16]), int(row[18]), int(row[17]), int(row[19])
        need = 32 * oblk + 2 + (oh - 1) * ows + 32
        row[15] = -(-(32 * oblk + (oh - 1) * ows + 2 + ow) // 32)  # enough for stride-1 only
        self.assertTrue(RL.native_ok(row))
        self.assertLess(32 * int(row[15]), need)
        self.assertFalse(RL.native_ok(row, s2_direct=0))

    def test_harness_flow_on_simulator(self):
        import hardware_mercury as HM
        import mercury_layout as ML

        _bind()
        case, t = synthetic_case(self.model, 4)
        runner = SimRunner()
        manifest = {"cases": [{"name": "s", "split": "synthetic", "hinted": True, "sha256": ""}]}
        orig = HM.load_case
        HM.load_case = lambda cases, entry: case
        try:
            rows, plans = HM.run(runner, self.model, None, manifest, 3, ML.LAST)
        finally:
            HM.load_case = orig
        row = rows["case00"]
        self.assertTrue(row["qualify"]["matches_host_emulator"])
        self.assertLess(row["qualify"]["max_bound_ratio"], 1)
        self.assertTrue(row["outputs_equal_fp16_contract"])
        self.assertEqual(row["vs_fp16_contract"]["worst_joint_px"], 0)
        c = row["controls"]
        self.assertEqual(c["stride2_descriptor_rejections"], 12)
        self.assertEqual(runner.events.count("reject"), 13)
        self.assertEqual(runner.events.count("fault"), 1)
        s = HM.summarize(rows, 36)
        self.assertEqual(s["outputs_equal_fp16_contract"], 1)
        self.assertEqual(len(plans["qualify_segments"]), 2)

    def test_pilot_plan(self):
        import mercury_layout as ML

        mp = ML.build_plan(self.model, stop_after="Conv_33")
        self.assertEqual(mp.ops[-1].name, "Conv_33")
        self.assertEqual(len(mp.ops), 5)
        self.assertFalse(mp.needs_hint)
        with self.assertRaises(ValueError):
            ML.build_plan(self.model, stop_after="Gemm_58")

    def test_depthwise_bound_covers_qf16_flush(self):
        """The resident depthwise accumulates in qf16, which drops sub-2^-14 products.
        Reconstructs the observed device failure: a small positive bias plus one product
        just under 2^-14, which the device returns as the bias alone."""
        import mercury_layout as ML

        op = next(o for o in ML.device_ops(self.model, prefix=True) if o.name == "Conv_14.g1")
        w = np.asarray(op.params["weight"][:, 0], np.float16).astype(np.float64)
        b = op.params["bias"]
        c = int(np.argmin(np.where(b > 0, b, np.inf)))
        self.assertLess(b[c], 0.01)
        x = np.zeros((32, 64, 64))
        x[c, 20, 20] = 0.9 * 2.0**-14 / abs(w[c, 1, 1])  # centre tap of output (10, 10)
        exact = ML.exact(op, x)
        device = float(np.float16(b[c]))  # the product is flushed
        allow = ML.bound(op, x)
        self.assertGreater(abs(device - exact[c, 10, 10]), 3 * 2.0**-10 * b[c] + 2.0**-20)
        self.assertLessEqual(abs(device - exact[c, 10, 10]), allow[c, 10, 10])

    def test_descriptor_rejection_word(self):
        """The pw parameter-source word the harness corrupts is word 9 (byte 36)."""
        import mercury_layout as ML
        import teacher_resident_layout as RL
        import teacher_tail_layout as T

        mp = ML.build_plan(self.model)
        table = bytearray(mp.plan.table)
        k = next(
            n for n in range(mp.descriptors) if struct.unpack_from("<I", table, 128 * n)[0] == 1
        )
        struct.pack_into("<I", table, 128 * k + 36, T.WIN_HI)
        self.assertFalse(RL.native_ok(np.frombuffer(bytes(table[128 * k : 128 * k + 128]), "<u4")))


if __name__ == "__main__":
    unittest.main()
