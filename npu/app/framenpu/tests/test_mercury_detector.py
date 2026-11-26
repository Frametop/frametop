"""Offline checks of the Mercury hand detector on the resident kernel's lane simulator: the
plan's rows pass the installed image's native bounds, and the simulated device part equals
the FP16 contract (mercury.run "fp16") bit for bit, for whole and split plans.

Needs NumPy and the detector model in FNPU_MERCURY_DETECTOR; no device.
"""

import os
from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "src"), str(ROOT / "tests"), str(ROOT / "tools")]

MODEL = os.environ.get("FNPU_MERCURY_DETECTOR")
KEYPOINT = os.environ.get("FNPU_MERCURY_MODEL")
DET_JOBS = os.environ.get("FNPU_MERCURY_DET_JOBS")  # the --cbias --win-wide slots build
SLOTS_JOBS = os.environ.get("FNPU_MERCURY_JOBS")  # optional: the --res-slots build, for comparison
MULTI_JOBS = os.environ.get("FNPU_MERCURY_MULTI_JOBS")  # optional: the same build with --multi
STREAM_JOBS = os.environ.get("FNPU_MERCURY_STREAM_JOBS")  # optional: --multi --stream

try:
    import numpy as np
except ImportError:  # pragma: no cover
    np = None

# the installed --res-slots image's build (mercury_runtime: replay, 8-groups, heads image, tiles, deep8)
FLAGS = dict(s2_direct=0, pw_group=8, mheads=True, resident=True, tiles=True, deep8=True)
PLAN = dict(replay=True, pw_group=8, resident=True, tiles=True, deep8=True)
SPLIT = "Conv_123"  # the last op of the device part when the detector shares a session with ft7


def crops(n, seed):
    """Mercury-normalized 160x160 images with hand-like structure (smoothed noise + bars)."""
    from framenpu import mercury

    rng = np.random.default_rng(seed)
    out = []
    for _ in range(n):
        u = rng.random((40, 40))
        img = np.kron(u, np.ones((4, 4)))
        yy, xx = np.mgrid[:160, :160]
        for _ in range(5):
            a, c = rng.uniform(0, np.pi), rng.uniform(40, 120, 2)
            d = np.abs((xx - c[0]) * np.sin(a) - (yy - c[1]) * np.cos(a))
            img = img + 2.0 * (d < rng.uniform(4, 12))
        out.append(mercury.normalize(np.clip(img * 60, 0, 255).astype(np.uint8)))
    return out


@unittest.skipUnless(np is not None and MODEL, "NumPy and FNPU_MERCURY_DETECTOR required")
class DetectorPlanTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from framenpu import mercury

        cls.model = mercury.load_detector(MODEL, require_pin=False)

    def plan(self, batch, stop_after=None):
        from framenpu import _mercury_layout as L

        kw = dict(PLAN, batch=batch)
        if stop_after:
            kw["stop_after"] = stop_after
        return L.build_plan(self.model, **kw)

    def rows(self, mp):
        return list(struct.iter_unpack("<32I", mp.plan.table))

    def simulate(self, mp, images, *, vt=None, load=True):
        from framenpu import _mercury_common as C
        from framenpu import _mercury_layout as L
        import teacher_resident_layout as RL

        window = bytearray(C.WIN_HI - C.WIN_LO)

        def put(off, data):
            window[off - C.WIN_LO : off - C.WIN_LO + len(data)] = data

        def get(off, n):
            return bytes(window[off - C.WIN_LO : off - C.WIN_LO + n])

        for off, data in C.setup_writes(mp.plan).items():
            put(off, data)
        for k, x in enumerate(images):
            L.stage_inputs(mp.crop(k), put, {L.STEM_COLS: L.stem_columns(x, 160)})
        vt = np.full((C.RES_SPAN, 32, 2), np.nan, np.float16) if vt is None else vt
        RL.simulate(mp.plan, window, s2_phase=0, s2_direct=True, crops=len(images),
                    resident=vt, load=load, tiles=True, deep8=True)
        return [L.read_tensor(mp.crop(k), get, mp.final) for k in range(len(images))]

    def expected(self, x, final):
        from framenpu import mercury

        _, t = mercury.run(self.model, {"inputImg": x}, "fp16", keep=True)
        return t[final].astype(np.float64)

    def test_lowering(self):
        from framenpu import mercury

        self.assertTrue(self.model.detector)
        self.assertEqual(self.model.outputs, mercury.DET_OUTPUTS)
        with self.assertRaises(mercury.InvalidModel):
            mercury.load_keypoint(MODEL, require_pin=False)

    def test_native_bounds(self):
        from framenpu import _mercury_common as C

        for stop in (None, SPLIT):
            for batch in (1, 2):
                mp = self.plan(batch, stop)
                for k in range(batch):
                    for r in self.rows(mp):
                        row = C.replay_row(r, k, mp.plan.crop_stride)
                        ok = C.regrid_ok(row) if row[0] == 7 else C.native_ok(row, **FLAGS)
                        self.assertTrue(ok, (stop, batch, k, row[:16]))

    def test_whole_plan_equals_fp16_contract(self):
        from framenpu import _mercury_layout as L

        mp = self.plan(2)
        self.assertEqual(mp.final, L.DET_OUTPUT)
        xs = crops(2, 5)
        for x, (got, stray) in zip(xs, self.simulate(mp, xs)):
            self.assertEqual(stray, 0)
            np.testing.assert_array_equal(got, self.expected(x, mp.final))

    def test_split_plan_and_host_tail(self):
        from framenpu import mercury

        mp = self.plan(2, SPLIT)
        xs = crops(2, 6)
        vt = np.full((mercury_common().RES_SPAN, 32, 2), np.nan, np.float16)
        first = self.simulate(mp, xs, vt=vt)
        again = self.simulate(mp, xs[::-1], vt=vt, load=False)  # images stay resident
        for x, (got, stray), (back, _) in zip(xs, first, again[::-1]):
            self.assertEqual(stray, 0)
            np.testing.assert_array_equal(got, self.expected(x, mp.final))
            np.testing.assert_array_equal(back, got)
            out = mercury.run_tail(self.model, {mp.final: got.astype(np.float32)}, mp.ops[-1].name)
            ref = mercury.run(self.model, {"inputImg": x}, "fp16")
            for name in mercury.DET_OUTPUTS:
                np.testing.assert_allclose(out[name], ref[name], atol=2e-3)


@unittest.skipUnless(np is not None and MODEL, "NumPy and FNPU_MERCURY_DETECTOR required")
class PackedBiasPlanTests(unittest.TestCase):
    """RS_CBIAS + RS_WIN_WIDE: packed biases expanded per op; same outputs bit for bit."""

    def test_detector_packed_wide_equals_fp16_contract(self):
        from framenpu import _mercury_common as C
        from framenpu import _mercury_layout as L
        from framenpu import mercury
        import teacher_resident_layout as RL

        model = mercury.load_detector(MODEL, require_pin=False)
        mp = L.build_plan(model, batch=2, **PLAN, cbias=True, win_hi=C.WIN_HI_WIDE)
        plain = L.build_plan(model, batch=2, **PLAN)
        self.assertLess(mp.win_end - C.WIN_LO, plain.win_end - C.WIN_LO - (900 << 10))
        self.assertLessEqual(mp.res_end, C.RES_SPAN - C.XB_N)
        for k in range(2):
            for r in struct.iter_unpack("<32I", mp.plan.table):
                row = C.replay_row(r, k, mp.plan.crop_stride, C.WIN_HI_WIDE)
                ok = C.regrid_ok(row) if row[0] == 7 else C.native_ok(row, **FLAGS, cbias=True, win_hi=C.WIN_HI_WIDE)
                self.assertTrue(ok, row[:16])
        window = bytearray(C.WIN_HI_WIDE - C.WIN_LO)

        def put(off, data):
            window[off - C.WIN_LO : off - C.WIN_LO + len(data)] = data

        for off, data in C.setup_writes(mp.plan).items():
            put(off, data)
        xs = crops(2, 9)
        for k, x in enumerate(xs):
            L.stage_inputs(mp.crop(k), put, {L.STEM_COLS: L.stem_columns(x, 160)})
        vt = np.full((C.RES_SPAN, 32, 2), np.nan, np.float16)
        RL.simulate(mp.plan, window, s2_phase=0, s2_direct=True, crops=2, resident=vt, load=True,
                    tiles=True, deep8=True, cbias=True, win_hi=C.WIN_HI_WIDE, s2_order="pairs_rev")
        get = lambda off, n: bytes(window[off - C.WIN_LO : off - C.WIN_LO + n])  # noqa: E731
        for k, x in enumerate(xs):
            got, stray = L.read_tensor(mp.crop(k), get, mp.final)
            self.assertEqual(stray, 0)
            _, t = mercury.run(model, {"inputImg": x}, "fp16", keep=True)
            np.testing.assert_array_equal(got, t[mp.final].astype(np.float64))


    def test_post_batch_store_order(self):
        """The post-batch stride-2 kernel stores two rows per column block. Ascending blocks
        (382fe1e6) zero the next row's first 22 columns of the 80 -> 40 layer (Conv_4), as
        the device returned; last to first (RS_S2_REV) equals the FP16 contract."""
        from framenpu import _mercury_common as C
        from framenpu import _mercury_layout as L
        from framenpu import mercury
        import teacher_resident_layout as RL

        model = mercury.load_detector(MODEL, require_pin=False)
        mp = L.build_plan(model, batch=1, **PLAN, cbias=True, win_hi=C.WIN_HI_WIDE, stop_after="Conv_4")
        x = crops(1, 12)[0]
        _, t = mercury.run(model, {"inputImg": x}, "fp16", keep=True)
        want = t[mp.final].astype(np.float64).reshape(32, 40, 40)
        got = {}
        for order in ("pairs", "pairs_rev"):
            window = bytearray(C.WIN_HI_WIDE - C.WIN_LO)

            def put(off, data):
                window[off - C.WIN_LO : off - C.WIN_LO + len(data)] = data

            for off, data in C.setup_writes(mp.plan).items():
                put(off, data)
            L.stage_inputs(mp.crop(0), put, {L.STEM_COLS: L.stem_columns(x, 160)})
            vt = np.full((C.RES_SPAN, 32, 2), np.nan, np.float16)
            RL.simulate(mp.plan, window, s2_phase=0, s2_direct=True, resident=vt, tiles=True, deep8=True,
                        cbias=True, win_hi=C.WIN_HI_WIDE, s2_order=order)
            get = lambda off, n: bytes(window[off - C.WIN_LO : off - C.WIN_LO + n])  # noqa: E731
            out, stray = L.read_tensor(mp.crop(0), get, mp.final)
            self.assertEqual(stray, 0)
            got[order] = out.reshape(32, 40, 40)
        np.testing.assert_array_equal(got["pairs_rev"], want)
        bad = np.zeros((40, 40), bool)
        bad[1::2, :22] = True
        np.testing.assert_array_equal(got["pairs"][:, ~bad], want[:, ~bad])
        self.assertTrue((got["pairs"][:, bad] == 0).all())
        self.assertTrue((want[:, bad] != 0).any())


@unittest.skipUnless(np is not None and MODEL and KEYPOINT and DET_JOBS,
                     "FNPU_MERCURY_DETECTOR, FNPU_MERCURY_MODEL and FNPU_MERCURY_DET_JOBS required")
class DetectorSessionTests(unittest.TestCase):
    """ft7-style keypoint model (3 crops) + the detector (2 crops) in one session on the
    packed-bias wide-window image, through the runtime tests' lane-simulator device (which
    models that image's last-to-first stride-2 store order)."""

    def session(self, jobs, extra):
        from unittest.mock import patch
        import test_mercury_runtime as TR
        from framenpu import mercury_runtime as M

        lease = TR._Lease()
        with patch.object(M, "_lease", return_value=lease):
            s = M.MercurySession(KEYPOINT, jobs, require_pin=False, extra_models=extra,
                                 _device_factory=TR._Device, _lease_factory=lambda: lease)
        s.__enter__()
        s.device.owner = s
        return s

    @staticmethod
    def kp_feed(seed):
        rng = np.random.default_rng(seed)
        return {"inputImg": rng.uniform(-1, 1, (1, 1, 128, 128)).astype(np.float32),
                "lastKeypoints": rng.uniform(-1, 1, (1, 42)).astype(np.float32),
                "useLastKeypoints": np.array([seed % 2], np.float32)}

    def test_keypoint_and_detector_share_the_session(self):
        from framenpu import mercury

        s = self.session(DET_JOBS, [(MODEL, 2)])
        self.assertTrue(s.cbias and s.res_slots)
        det = s.models[1]
        self.assertTrue(det.model.detector and det.plan.cbias)
        imgs = crops(3, 11)
        with s:
            for rnd in range(2):
                kf = [self.kp_feed(rnd * 10 + k) for k in range(3)]
                for f, out in zip(kf, s.infer(kf)):
                    want = mercury.run(s.model, f, "fp16")
                    for name in mercury.OUTPUTS:
                        np.testing.assert_array_equal(out[name], want[name])
                df = [{"inputImg": x[None, None]} for x in imgs[rnd : rnd + 2]]
                for f, out in zip(df, s.infer(df, model=1)):
                    want = mercury.run(det.model, f, "fp16")
                    for name in mercury.DET_OUTPUTS:
                        np.testing.assert_allclose(out[name], want[name], atol=2e-3)
            # each model's images were copied once, on its first request
            self.assertEqual(s.device.copied, [True, True, False, False])

    @unittest.skipUnless(MULTI_JOBS, "FNPU_MERCURY_MULTI_JOBS (the --multi build) required")
    def test_one_submission_per_frame(self):
        """infer_frame on the --multi image: detector + keypoint segments of one job give the
        separate requests' outputs bit for bit; each model's images are copied once."""
        s = self.session(MULTI_JOBS, [(MODEL, 2)])
        self.assertTrue(s.multi and s.cbias and s.res_slots)
        imgs = crops(3, 12)
        with s:
            for rnd in range(3):
                kf = [self.kp_feed(30 + rnd * 10 + k) for k in range(1 + rnd % 3)]
                df = [{"inputImg": x[None, None]} for x in imgs[: 1 + rnd % 2]]
                order = [(1, df), (0, kf)] if rnd % 2 == 0 else [(0, kf), (1, df)]
                got = s.infer_frame(order)
                self.assertEqual(s.last_timing["segments"], 2)
                self.assertEqual(s.device.jobs[-1], 2)  # one submission of two segments
                self.assertEqual([len(g) for g in got], [len(f) for _, f in order])
                want = [s.infer(f, model=m) for m, f in order]
                for g, w, (m, _) in zip(got, want, order):
                    names = mercury_outputs(m)
                    for a, b in zip(g, w):
                        for name in names:
                            np.testing.assert_array_equal(a[name], b[name])
            # one copy per model: the frame's first request for each
            self.assertEqual(s.device.copied[:2], [True, True])
            self.assertFalse(any(s.device.copied[2:]))
            with self.assertRaises(ValueError):
                s.infer_frame([(0, kf), (0, kf)])  # one workspace per model
            with self.assertRaises(ValueError):
                s.infer_frame([])

    @unittest.skipUnless(STREAM_JOBS, "FNPU_MERCURY_STREAM_JOBS (the --multi --stream build) required")
    def test_streamed_frame(self):
        """--stream: segment 1 prepared and staged, segment 0 read and finished while the DSP
        runs; outputs equal the separate requests' bit for bit."""
        s = self.session(STREAM_JOBS, [(MODEL, 2)])
        self.assertTrue(s.stream and s.multi)
        imgs = crops(3, 13)
        with s:
            for rnd in range(4):
                kf = [self.kp_feed(50 + rnd * 10 + k) for k in range(1 + rnd % 3)]
                df = [{"inputImg": x[None, None]} for x in imgs[: 1 + rnd % 2]]
                order = [(1, df), (0, kf)] if rnd % 2 == 0 else [(0, kf), (1, df)]
                got = s.infer_frame(order)
                self.assertTrue(s.last_timing["streamed"] and s.last_timing["early_finish"])
                self.assertEqual(s.device.jobs[-1], 2)
                want = [s.infer(f, model=m) for m, f in order]
                for g, w, (m, _) in zip(got, want, order):
                    self.assertEqual(len(g), len(w))
                    for a, b in zip(g, w):
                        for name in mercury_outputs(m):
                            np.testing.assert_array_equal(a[name], b[name])
            self.assertEqual(s.device.copied[:2], [True, True])
            self.assertFalse(any(s.device.copied[2:]))

            # segment 1's prepare fails in the wait hook: the job still completes cleanly (no
            # recovery), the original error is raised, and the next frame runs
            bad = dict(kf[0], inputImg=np.full((1, 1, 128, 128), np.nan, np.float32))
            with self.assertRaises(ValueError):
                s.infer_frame([(1, df), (0, [bad])])
            self.assertFalse(s.device._transport.poisoned)
            got = s.infer_frame([(1, df), (0, kf)])
            for a, b in zip(got[1], s.infer(kf)):
                for name in mercury_outputs(0):
                    np.testing.assert_array_equal(a[name], b[name])

            # segment 1 never released: the kernel's error requires recovery
            from framenpu.errors import RecoveryRequired

            put = s.device._transport.put
            s.device._transport.put = lambda off, v: None
            with self.assertRaises(RecoveryRequired):
                s.infer_frame([(1, df), (0, kf)])
            s.device._transport.put = put
            self.assertTrue(s.device._transport.poisoned)

    def test_three_models_do_not_fit(self):
        from framenpu.errors import InvalidModel

        # a second keypoint model at 3 crops + the detector exceed the blob region
        with self.assertRaises(InvalidModel):
            self.session(DET_JOBS, [(KEYPOINT, 3), (MODEL, 2)])

    @unittest.skipUnless(SLOTS_JOBS, "FNPU_MERCURY_JOBS (the --res-slots build) required")
    def test_detector_does_not_fit_the_slots_image(self):
        from framenpu.errors import InvalidModel

        with self.assertRaises(InvalidModel):
            self.session(SLOTS_JOBS, [(MODEL, 1)])


def mercury_outputs(model):
    from framenpu import mercury

    return mercury.DET_OUTPUTS if model == 1 else mercury.OUTPUTS


def mercury_common():
    from framenpu import _mercury_common as C

    return C


if __name__ == "__main__":
    unittest.main()
