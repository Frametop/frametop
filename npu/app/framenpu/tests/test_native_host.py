"""Host kernels (src/framenpu/host/fnpu_host.c) against the NumPy reference.

The bundled aarch64 library is checked against its manifest everywhere. The kernels
themselves are tested through a local build of the same source for this machine
(needs clang and ld.lld); the session test also needs FNPU_MERCURY_MODEL and
FNPU_MERCURY_HEADS_JOBS.
"""

import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))

from framenpu import _native  # noqa: E402

MODEL = os.environ.get("FNPU_MERCURY_MODEL")
HEADS_JOBS = os.environ.get("FNPU_MERCURY_HEADS_JOBS")
DET_MODEL = os.environ.get("FNPU_MERCURY_DETECTOR")  # Mercury's grayscale_detection_160x160.onnx
DET_JOBS = os.environ.get("FNPU_MERCURY_DET_JOBS")  # the --cbias --win-wide --s2-rev slots build
S3D_MODEL = os.environ.get("FNPU_S3D_MODEL")  # a 2-view stereo student (distill/make_stereo3d.py)
TOOLS = all(shutil.which(t) for t in ("clang", "ld.lld", "llvm-nm"))
ARCH = platform.machine()


class BundledLibraryTests(unittest.TestCase):
    def test_bundled_library_matches_manifest_and_source(self):
        import check_repository

        check_repository.verify_host_native(ROOT / "src/framenpu")
        rec = json.loads((_native.DATA / "host_aarch64.json").read_text())
        self.assertEqual(rec["target"], "aarch64-linux-gnu")
        self.assertEqual(rec["abi"], _native.ABI)

    @unittest.skipUnless(TOOLS, "clang and ld.lld required")
    def test_bundled_library_rebuilds_byte_identical(self):
        import build_host_native as B

        rec = json.loads((_native.DATA / "host_aarch64.json").read_text())
        if B._tool(["clang", "--version"]).splitlines()[0].strip() != rec["toolchain"]["clang"]:
            self.skipTest("a different clang built the bundled library")
        with tempfile.TemporaryDirectory() as d:
            got = B.build("aarch64", Path(d) / "host.so")
        self.assertEqual(got["sha256"], rec["sha256"])
        self.assertEqual(got["compiler_flags"], rec["compiler_flags"])

    def test_host_refuses_a_library_that_differs_from_its_manifest(self):
        rec = json.loads((_native.DATA / "host_aarch64.json").read_text())
        with tempfile.TemporaryDirectory() as d:
            bad = Path(d) / "host.so"
            bad.write_bytes((_native.DATA / "host_aarch64.so").read_bytes() + b"\0")
            with self.assertRaises(ValueError):
                _native.Host(bad, rec)

    def test_bundled_is_none_off_aarch64(self):
        if ARCH != "aarch64":
            self.assertIsNone(_native.bundled())


@unittest.skipUnless(TOOLS and ARCH in ("x86_64", "aarch64"), "clang and ld.lld required")
class KernelTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import build_host_native as B

        cls.tmp = tempfile.TemporaryDirectory()
        lib = Path(cls.tmp.name) / f"host_{ARCH}.so"
        cls.host = _native.Host(lib, B.build(ARCH, lib))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_fp16_conversion_matches_numpy(self):
        import numpy as np

        rng = np.random.default_rng(1)
        special = np.array(
            [0x00000000, 0x80000000, 0x33000000, 0x33000001, 0x337FFFFF, 0x33800000,
             0x387FC000, 0x387FE000, 0x387FF000, 0x38800000, 0x477FE000, 0x477FEFFF,
             0x477FF000, 0x477FF001, 0x7F800000, 0xFF800000, 0x3F800000, 0x3F801000,
             0x3F802000, 0x3F803000],
            np.uint32,
        )
        u = np.concatenate([special, rng.integers(0, 1 << 32, 1 << 20, dtype=np.uint64).astype(np.uint32)])
        f = u.view(np.float32)
        f = f[~np.isnan(f)]
        out = np.empty(f.size, np.uint16)
        with np.errstate(all="ignore"):
            ref = f.astype(np.float16)
        bad = self.host.to_f16(f, out)
        np.testing.assert_array_equal(out, ref.view(np.uint16))
        self.assertEqual(bad, int(np.count_nonzero(~np.isfinite(ref))))
        h = np.arange(1 << 16, dtype=np.uint32).astype(np.uint16)
        back = np.empty(h.size, np.float32)
        self.host.from_f16(h, back)
        ref = h.view(np.float16).astype(np.float32)
        same = (back.view(np.uint32) == ref.view(np.uint32)) | (np.isnan(back) & np.isnan(ref))
        self.assertTrue(same.all())

    def test_wrappers_refuse_wrong_types_and_sizes(self):
        import numpy as np

        out = np.empty(4, np.uint16)
        with self.assertRaises(TypeError):
            self.host.to_f16(np.zeros(4, np.float64), out)
        with self.assertRaises(ValueError):
            self.host.to_f16(np.zeros(5, np.float32), out)
        with self.assertRaises(TypeError):
            self.host.to_f16(np.zeros(8, np.float32)[::2], out)
        with self.assertRaises(ValueError):
            self.host.grid_pack(
                np.zeros(4, np.uint16), 2, np.array([0, 9], np.int32), 4, np.empty(8, np.uint16)
            )

    @unittest.skipUnless(MODEL, "FNPU_MERCURY_MODEL required")
    def test_request_host_side_matches_the_numpy_path(self):
        import numpy as np
        from framenpu import mercury, _mercury_layout as L
        from framenpu._mercury_host import NativeRequest

        model = mercury.load_keypoint(MODEL, require_pin=False)
        plan = L.build_plan(model, prefix=True, batch=3, replay=True, pw_group=8, heads=True)
        nr = NativeRequest(self.host, model, plan, True)
        with self.assertRaises(ValueError):  # the NumPy-tail image is not covered
            NativeRequest(self.host, model, L.build_plan(
                model, prefix=True, batch=3, replay=True, pw_group=8), False)
        rng = np.random.default_rng(7)
        for seed in range(4):
            feed = {
                "inputImg": rng.uniform(-1, 1, (1, 1, 128, 128)).astype(np.float32),
                "lastKeypoints": rng.uniform(-1, 1, (1, 42)).astype(np.float32),
                "useLastKeypoints": np.array([seed % 2], np.float32),
            }
            ts = {
                n: np.asarray(feed[n], np.float32).reshape(s[1:] if len(s) > 1 else s)
                for n, s in mercury.INPUTS.items()
            }
            for n in L.HOST_HINT_OPS[:3]:
                op = model.op(n)
                ts[op.output] = mercury._step(op, ts, "fp16", "fp16")
            w = nr.prepare([feed])
            got = {L.STEM_COLS: w.stem[0].copy(), L.HINT: w.hint_grid[0].copy()}
            for name, ref in ((L.STEM_COLS, L.stem_columns(ts["inputImg"])), (L.HINT, ts[L.HINT])):
                self.assertEqual(got[name].tobytes(), L.tensor_bytes(plan.crop(0), name, ref), name)

            full = mercury.run(model, feed, "fp16", keep=True)[1]
            o = full[L.OUTPUT].astype(np.float32)
            hm = np.zeros((22, 22, 22), np.float32)
            hm[:21] = full["heatmap_xy"].reshape(21, 22, 22)
            want = mercury.run_tail(model, {L.OUTPUT: o, "heatmap_xy": hm[:21]}, L.LAST, skip=L.HEAD_OPS)
            raw = {n: L.tensor_bytes(plan.crop(1), n, t) for n, t in ((L.OUTPUT, o), (L.HEATMAP, hm))}
            where = plan.crop(1).where
            w = nr.prepare([feed, feed])
            self.assertTrue(nr.read(w, lambda at, n: raw[next(k for k in raw if where[k] == at)], 1))
            w.maps[L.OUTPUT][0] = w.maps[L.OUTPUT][1]
            w.maps[L.HEATMAP][0] = w.maps[L.HEATMAP][1]
            out = nr.finish(w)[1]
            for name in ("heatmap_xy", "scalar_extras", "curls"):
                np.testing.assert_array_equal(out[name], want[name], name)
            # Conv_143: the reference's FP32 BLAS sum vs the kernel's FP64 sum
            np.testing.assert_allclose(out["heatmap_depth"], want["heatmap_depth"], rtol=0, atol=1e-5)
            # a nonzero half in the padding is refused
            bad = bytearray(raw[L.OUTPUT])
            bad[-2:] = b"\x00\x3c"
            raw[L.OUTPUT] = bytes(bad)
            self.assertFalse(nr.read(w, lambda at, n: raw[next(k for k in raw if where[k] == at)], 1))
        with self.assertRaises(ValueError):
            nr.prepare([feed, dict(feed, inputImg=np.full((1, 1, 128, 128), 7e4, np.float32))])
        with self.assertRaises(ValueError):
            nr.prepare([dict(feed, lastKeypoints=np.full((1, 42), np.nan, np.float32))])
        with self.assertRaises(ValueError):
            nr.prepare([feed] * 4)

    @unittest.skipUnless(DET_MODEL, "FNPU_MERCURY_DETECTOR required")
    def test_detector_host_side_matches_the_numpy_path(self):
        import numpy as np
        from framenpu import mercury, _mercury_common as C, _mercury_layout as L
        from framenpu._mercury_host import NativeDetector, NativeRequest

        model = mercury.load_detector(DET_MODEL, require_pin=False)
        plan = L.build_plan(model, batch=2, replay=True, pw_group=8, resident=True, tiles=True, deep8=True,
                            cbias=True, win_hi=C.WIN_HI_WIDE)
        nd = NativeDetector(self.host, model, plan)
        with self.assertRaises(ValueError):  # the keypoint request class refuses the detector
            NativeRequest(self.host, model, plan, False)
        with self.assertRaises(ValueError):  # a split device part is not covered
            NativeDetector(self.host, model, L.build_plan(
                model, batch=2, replay=True, pw_group=8, resident=True, tiles=True, deep8=True,
                stop_after="Conv_123"))
        rng = np.random.default_rng(3)
        for seed in range(3):
            # values off the FP16 grid, edges included: the conversion rounds as NumPy does
            xs = [rng.uniform(-2, 2, (1, 1, 160, 160)).astype(np.float32) for _ in range(2)]
            w = nd.prepare([{"inputImg": x} for x in xs])
            mem = {}
            for k, x in enumerate(xs):
                want = {}
                L.stage_inputs(plan.crop(k), want.__setitem__, {L.STEM_COLS: L.stem_columns(x[0, 0], 160)})
                self.assertEqual(bytes(w.staged[k]), want[plan.crop(k).where[L.STEM_COLS]])
            # read back the FP16 contract's final maps and finish both ways
            fins = [mercury.run(model, {"inputImg": x}, "fp16", keep=True)[1][L.DET_OUTPUT] for x in xs]
            for k, f in enumerate(fins):
                mem[plan.crop(k).where[L.DET_OUTPUT]] = L.tensor_bytes(plan.crop(k), L.DET_OUTPUT, f)
            for k in range(2):
                self.assertTrue(nd.read(w, lambda at, n: mem[at], k))
            outs = nd.finish(w)
            for f, out in zip(fins, outs):
                want = mercury.run_tail(model, {L.DET_OUTPUT: np.asarray(f, np.float32)}, L.DET_LAST)
                for name in mercury.DET_OUTPUTS:
                    self.assertEqual(out[name].shape, want[name].shape)
                    np.testing.assert_allclose(out[name], want[name], rtol=0, atol=1e-6, err_msg=name)
            # a nonzero half in the padding is refused
            k0 = plan.crop(0).where[L.DET_OUTPUT]
            bad = bytearray(mem[k0])
            bad[-2:] = b"\x00\x3c"
            mem[k0] = bytes(bad)
            self.assertFalse(nd.read(w, lambda at, n: mem[at], 0))
        with self.assertRaises(ValueError):
            nd.prepare([{"inputImg": np.full((1, 1, 160, 160), 7e4, np.float32)}])
        with self.assertRaises(ValueError):
            nd.prepare([{"inputImg": np.full((1, 1, 160, 160), np.nan, np.float32)}])
        with self.assertRaises(ValueError):
            nd.prepare([{"inputImg": np.zeros((1, 1, 128, 128), np.float32)}])
        with self.assertRaises(ValueError):
            nd.prepare([{"inputImg": xs[0]}] * 4)

    @unittest.skipUnless(S3D_MODEL, "FNPU_S3D_MODEL required")
    def test_stereo_student_host_side_matches_the_numpy_path(self):
        import numpy as np
        from framenpu import mercury, _mercury_layout as L
        from framenpu._mercury_host import NativeRequest

        model = mercury.load_keypoint(S3D_MODEL, require_pin=False)
        plan = L.build_plan(model, prefix=True, batch=2, replay=True, pw_group=8, heads=True, resident=True)
        outs = ("joints3d", "scalar_extras")
        nr = NativeRequest(self.host, model, plan, True, outs)
        rng = np.random.default_rng(11)
        feeds, wants, maps = [], [], []
        for seed in range(3):
            feed = {
                "inputImg": rng.uniform(-1, 1, (1, 2, 128, 128)).astype(np.float32),
                "lastKeypoints": rng.uniform(-1, 1, (1, 84)).astype(np.float32),
                "useLastKeypoints": np.array([seed % 2], np.float32),
                "geom": rng.uniform(-1, 1, (1, 16)).astype(np.float32),
            }
            ts = {n: np.asarray(feed[n], np.float32).reshape(s[1:] if len(s) > 1 else s) for n, s in model.inputs.items()}
            for n in L.HOST_HINT_OPS[:3]:
                op = model.op(n)
                ts[op.output] = mercury._step(op, ts, "fp16", "fp16")
            w = nr.prepare([feed])
            self.assertEqual(w.stem[0].tobytes(), L.tensor_bytes(plan.crop(0), L.STEM_COLS, L.stem_columns(ts["inputImg"])))
            self.assertEqual(w.hint_grid[0].tobytes(), L.tensor_bytes(plan.crop(0), L.HINT, ts[L.HINT]))
            full = mercury.run(model, feed, "fp16", keep=True)[1]
            o = full[L.OUTPUT].astype(np.float32)
            want = mercury.run_tail(model, {L.OUTPUT: o, "geom": ts["geom"]}, L.LAST, skip=L.HEAD_OPS, outputs=outs)
            for k in outs:  # the emulator's own end to end run agrees
                np.testing.assert_array_equal(want[k], full[k].reshape(want[k].shape))
            feeds.append(feed), wants.append(want), maps.append(o)
            w.maps[L.OUTPUT][0] = o
            got = nr.finish(w)[0]
            self.assertEqual(sorted(got), sorted(outs))
            # FP64 sums in a fixed order vs NumPy's BLAS order: at most an FP32 ulp or so
            for k in outs:
                np.testing.assert_allclose(got[k], want[k], rtol=1e-6, atol=1e-6, err_msg=k)
        # a batch of two equals its pairs one at a time, bit for bit
        singles = []
        for f, o in zip(feeds[:2], maps[:2]):
            w = nr.prepare([f])
            w.maps[L.OUTPUT][0] = o
            singles.append(nr.finish(w)[0])
        w = nr.prepare(feeds[:2])
        w.maps[L.OUTPUT][:] = maps[:2]
        for got, one in zip(nr.finish(w), singles):
            for k in outs:
                np.testing.assert_array_equal(got[k], one[k])
        with self.assertRaises(ValueError):
            nr.prepare([dict(feeds[0], geom=np.full((1, 16), np.inf, np.float32))])

    @unittest.skipUnless(MODEL, "FNPU_MERCURY_MODEL required")
    def test_a_batch_equals_its_crops_one_at_a_time(self):
        import numpy as np
        from framenpu import mercury, _mercury_layout as L
        from framenpu._mercury_host import NativeRequest

        model = mercury.load_keypoint(MODEL, require_pin=False)
        plan = L.build_plan(model, prefix=True, batch=3, replay=True, pw_group=8, heads=True)
        nr = NativeRequest(self.host, model, plan, True)
        rng = np.random.default_rng(3)
        feeds = [
            {
                "inputImg": rng.uniform(-1, 1, (1, 1, 128, 128)).astype(np.float32),
                "lastKeypoints": rng.uniform(-1, 1, (1, 42)).astype(np.float32),
                "useLastKeypoints": np.array([k % 2], np.float32),
            }
            for k in range(3)
        ]
        maps = [
            {
                L.OUTPUT: rng.uniform(0, 6, (160, 8, 8)).astype(np.float16).astype(np.float32),
                L.HEATMAP: rng.uniform(0, 1, (22, 22, 22)).astype(np.float32),
            }
            for _ in range(3)
        ]
        def staged(w, k):
            return w.stem[k].copy(), w.hint_grid[k].copy()

        for n in (2, 3):
            w = nr.prepare(feeds[:n])
            got = [staged(w, k) for k in range(n)]
            nr.load_maps(w, maps[:n])
            out = nr.finish(w)
            for k in range(n):
                w1 = nr.prepare([feeds[k]])
                for a, b in zip(got[k], staged(w1, 0)):
                    np.testing.assert_array_equal(a, b)
                nr.load_maps(w1, [maps[k]])
                single = nr.finish(w1)[0]
                for name in single:
                    np.testing.assert_array_equal(out[k][name], single[name])
        # results are the caller's own: a later request does not change them
        first = nr.finish(nr.prepare(feeds[:1]))[0]
        keep = {k: v.copy() for k, v in first.items()}
        nr.load_maps(nr._work(1), maps[1:2])
        nr.finish(nr._work(1))
        for name in keep:
            np.testing.assert_array_equal(first[name], keep[name])

    @unittest.skipUnless(MODEL and HEADS_JOBS, "FNPU_MERCURY_MODEL and FNPU_MERCURY_HEADS_JOBS required")
    def test_session_on_native_host_matches_the_contract(self):
        import numpy as np
        from unittest.mock import patch
        from framenpu import mercury, mercury_runtime as M
        import test_mercury_runtime as T

        lease = T._Lease()
        with patch.object(M, "_lease", return_value=lease):
            session = M.MercurySession(
                MODEL, HEADS_JOBS, native=self.host, _device_factory=T._Device,
                _lease_factory=lambda: lease,
            )
        self.assertIsNotNone(session.native)
        session.__enter__()
        session.device.owner = session
        with session:
            for count in (1, 3):
                fs = [T.MercurySessionTests.feeds(k + 11) for k in range(count)]
                for feed, out in zip(fs, session.infer(fs)):
                    want = mercury.run(session.model, feed, "fp16")
                    for name in ("heatmap_xy", "scalar_extras", "curls"):
                        np.testing.assert_array_equal(out[name], want[name])
                    np.testing.assert_allclose(
                        out["heatmap_depth"], want["heatmap_depth"], rtol=0, atol=1e-5
                    )
        self.assertFalse(any(session.device._transport.memory))
        with patch.object(M, "_lease", return_value=lease):
            plain = M.MercurySession(MODEL, HEADS_JOBS, native=False, _device_factory=T._Device,
                                     _lease_factory=lambda: lease)
        self.assertIsNone(plain.native)

    @unittest.skipUnless(MODEL and DET_MODEL and DET_JOBS,
                         "FNPU_MERCURY_MODEL, FNPU_MERCURY_DETECTOR and FNPU_MERCURY_DET_JOBS required")
    def test_detector_session_on_native_host(self):
        """keypoint (3 crops) + detector (2 crops) on the packed-bias wide-window image, both
        on the native host path, through the lane-simulator device: the same outputs as the
        NumPy host path."""
        import numpy as np
        from unittest.mock import patch
        from framenpu import mercury_runtime as M
        from framenpu._mercury_host import NativeDetector, NativeRequest
        import test_mercury_runtime as T

        rng = np.random.default_rng(5)
        det_feeds = [{"inputImg": rng.uniform(-1, 2, (1, 1, 160, 160)).astype(np.float32)} for _ in range(3)]
        kp_feeds = [T.MercurySessionTests.feeds(k + 21) for k in range(3)]
        results = {}
        for native in (self.host, False):
            lease = T._Lease()
            with patch.object(M, "_lease", return_value=lease):
                s = M.MercurySession(MODEL, DET_JOBS, require_pin=False, native=native,
                                     extra_models=[(DET_MODEL, 2)], _device_factory=T._Device,
                                     _lease_factory=lambda: lease)
            if native is not False:
                self.assertIsInstance(s.models[0].native, NativeRequest)
                self.assertIsInstance(s.models[1].native, NativeDetector)
            s.__enter__()
            s.device.owner = s
            with s:
                results[native is False] = (s.infer(kp_feeds), s.infer(det_feeds[:1], model=1),
                                            s.infer(det_feeds[1:], model=1))
        for a, b in zip(results[False], results[True]):
            for oa, ob in zip(a, b):
                self.assertEqual(oa.keys(), ob.keys())
                for name in oa:
                    np.testing.assert_allclose(oa[name], ob[name], rtol=0, atol=1e-5, err_msg=name)


@unittest.skipUnless(TOOLS and ARCH in ("x86_64", "aarch64"), "clang and ld.lld required")
class NativeVerifyTests(unittest.TestCase):
    """HmxPower.verify() through fnpu_read32 on simulated register pages."""

    @classmethod
    def setUpClass(cls):
        import build_host_native as B

        cls.tmp = tempfile.TemporaryDirectory()
        lib = Path(cls.tmp.name) / f"host_{ARCH}.so"
        cls.host = _native.Host(lib, B.build(ARCH, lib))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def powered(self, ro=None):
        import test_library as TL

        power = TL.PowerTests.make_power()
        power._native = self.host
        power._ro_factory = ro or (lambda address: power.views[address])
        power.start()
        power.hold()
        return power

    def test_native_verify_reads_the_held_words_and_detects_a_change(self):
        import struct
        from framenpu.errors import RecoveryRequired

        power = self.powered()
        self.assertIsNotNone(power._fast)
        self.assertIsNone(power.fast_verify_error)
        power.verify()
        self.assertEqual(power._fast.read(), power._fast.expected)
        for address, off in ((0x323C0000, 0x550 + 4 * 75), (0x32348000, 0x184), (0x32341000, 0x18)):
            mm = power.views[address]
            (old,) = struct.unpack_from("<I", mm, off)
            struct.pack_into("<I", mm, off, old ^ 1)
            with self.assertRaises(RecoveryRequired):
                power.verify()
            struct.pack_into("<I", mm, off, old)
            power.verify()
        power.close()
        self.assertIsNone(power._fast)
        self.assertTrue(all(mm.closed for mm in power.views.values()))

    def test_a_disagreeing_first_read_keeps_the_python_verify(self):
        import test_library as TL

        blank = TL.Memory(4096)
        power = self.powered(ro=lambda address: blank)
        self.assertIsNone(power._fast)
        self.assertIn("differ", power.fast_verify_error)
        power.verify()  # the Python path, on the real views
        power.close()

    def test_native_false_and_simulated_registers_keep_python(self):
        import test_library as TL

        power = TL.PowerTests.make_power()
        power.start()
        power.hold()  # simulated registers, no native host given
        self.assertIsNone(power._fast)
        power.close()
        power = TL.PowerTests.make_power()
        power._native = False
        power.start()
        power.hold()
        self.assertIsNone(power._fast)
        power.close()


if __name__ == "__main__":
    unittest.main()
