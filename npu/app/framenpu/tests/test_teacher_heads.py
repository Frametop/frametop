"""Offline bounds, arithmetic, planning and build checks for resident heads."""

import ctypes
import importlib.util
import math
import os
from pathlib import Path
import random
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / n) for n in ("src", "tests", "tools")]
import teacher_heads_layout as H
import teacher_resident_layout as R
import teacher_tail_layout as T

MODEL = Path(os.environ.get("FNPU_TEACHER_MODEL", "/nonexistent"))


@unittest.skipUnless(shutil.which("cc"), "host C compiler required")
class HeadsNativeChecks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        folder = Path(cls.tmp.name)
        source = (ROOT / "tests/teacher_heads.c").read_text()
        sigmoid = source[
            source.index("static __attribute__((noinline)) float th_sigmoid") : source.index(
                "static __attribute__((noinline)) void th_fault"
            )
        ]
        code = '#include <stdint.h>\ntypedef uint32_t fnpu_u32;\n#include "teacher_heads_check.h"\n'
        code += sigmoid + "\nint accepted(const fnpu_u32 *r) { return !th_bad(r); }\n"
        code += "float sigmoid_test(float x) { return th_sigmoid(x); }\n"
        (folder / "check.c").write_text(code)
        subprocess.run(
            [
                shutil.which("cc"),
                "-shared",
                "-fPIC",
                "-O2",
                "-I",
                str(ROOT / "tests"),
                str(folder / "check.c"),
                "-o",
                str(folder / "check.so"),
            ],
            check=True,
            capture_output=True,
        )
        cls.lib = ctypes.CDLL(str(folder / "check.so"))
        cls.lib.accepted.argtypes = [ctypes.POINTER(ctypes.c_uint32)]
        cls.lib.accepted.restype = ctypes.c_int
        cls.lib.sigmoid_test.argtypes = [ctypes.c_float]
        cls.lib.sigmoid_test.restype = ctypes.c_float
        cls.row = [
            6,
            336,
            4,
            672,
            49,
            7,
            1,
            7,
            9,
            T.WIN_LO,
            H.PARAM_VECTORS,
            4,
            2356,
            2360,
            2381,
        ] + [0] * 17

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def accepted(self, row):
        return bool(self.lib.accepted((ctypes.c_uint32 * 32)(*row)))

    def test_bounds_checker_matches_mirror_on_mutations(self):
        self.assertTrue(self.accepted(self.row))
        self.assertTrue(H.native_ok(self.row))
        rng = random.Random(0x48454144)
        for i in range(4000):
            r = self.row.copy()
            field = rng.randrange(32)
            r[field] = rng.choice(
                (
                    0,
                    1,
                    2,
                    3,
                    0x3FFF,
                    0x4000,
                    0xFFFFFFFF,
                    R.SPAN,
                    (r[field] - 1) & 0xFFFFFFFF,
                    (r[field] + 1) & 0xFFFFFFFF,
                )
            )
            with self.subTest(case=i, field=field):
                self.assertEqual(self.accepted(r), H.native_ok(r))

    def test_all_aliases_and_window_underflow_rejected(self):
        for a in range(11, 15):
            for b in range(a + 1, 15):
                r = self.row.copy()
                r[b] = r[a]
                self.assertFalse(self.accepted(r))
        for field, value in (
            (9, T.WIN_LO - 128),
            (9, T.WIN_HI),
            (11, R.SPAN),
            (12, R.SPAN),
            (13, R.SPAN),
            (14, R.SPAN),
            (15, 3),
            (16, 1),
        ):
            r = self.row.copy()
            r[field] = value
            self.assertFalse(self.accepted(r))

    def test_scalar_sigmoid_matches_reference(self):
        for i in range(-1600, 1601):
            x = i / 20
            self.assertAlmostEqual(self.lib.sigmoid_test(x), 1 / (1 + math.exp(-x)), delta=2e-7)
        for x, value in ((-100, 0), (0, 0.5), (100, 1)):
            self.assertEqual(self.lib.sigmoid_test(x), value)


@unittest.skipUnless(importlib.util.find_spec("numpy"), "NumPy required")
class HeadsHarnessTests(unittest.TestCase):
    def test_uncertain_completion_retains_window_and_sealed_image(self):
        import json
        from types import SimpleNamespace
        from unittest.mock import Mock, patch
        import hardware_teacher_link as L
        from framenpu import RecoveryRequired

        class ReadOnlyMemory:
            def __getitem__(self, key):
                return bytes(key.stop - key.start)

            def __setitem__(self, key, value):
                raise AssertionError("uncertain device memory must not be restored")

        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            (folder / "manifest.json").write_text(
                json.dumps({"param_sha256": "p", "weights_sha256": "w", "crops": []})
            )
            output = folder / "report.json"
            args = [
                "hardware_teacher_link.py",
                "--run-on-device",
                "--stride2",
                "--heads",
                "--jobs",
                tmp,
                "--model",
                tmp,
                "--corpus",
                tmp,
                "--output",
                str(output),
                "--rounds",
                "2",
            ]
            memory = ReadOnlyMemory()
            observer = Mock()
            observer.__enter__ = Mock(
                return_value=SimpleNamespace(_transport=SimpleNamespace(memory=memory))
            )
            observer.__exit__ = Mock(return_value=False)
            dev = Mock()
            dev.__enter__ = Mock(
                return_value=SimpleNamespace(
                    _transport=SimpleNamespace(memory=memory, poisoned=True)
                )
            )
            dev.__exit__ = Mock(return_value=False)
            power = Mock()
            power.__enter__ = Mock(return_value=power)
            power.__exit__ = Mock(return_value=False)
            runner = SimpleNamespace(sealed=True, admin=Mock())
            record = {"stride2": True, "heads": True, "dw_blocks": 2, "loader": {}, "linked": {}}
            from contextlib import ExitStack

            with ExitStack() as stack:
                for target, value in (
                    ("sys.argv", args),
                    ("hardware_teacher_link.os.geteuid", lambda: 0),
                    (
                        "hardware_teacher_link.HR.load_jobs",
                        Mock(return_value={"rs": (None, {"sha256": "r"})}),
                    ),
                    (
                        "hardware_teacher_link.load_link",
                        Mock(return_value=(None, bytes(4), record)),
                    ),
                    (
                        "framenpu.convolution.load_landmark_graph",
                        Mock(return_value=SimpleNamespace(param_sha256="p", weights_sha256="w")),
                    ),
                    ("framenpu.landmark.specifications", Mock(return_value=[])),
                    ("hardware_teacher_link._lease", Mock(return_value=Mock())),
                    ("hardware_teacher_link.PowerSnapshot", Mock(return_value=power)),
                    ("hardware_teacher_link.settled_health", Mock(return_value={})),
                    ("hardware_teacher_link.Device", Mock(return_value=observer)),
                    (
                        "hardware_teacher_link.audited_device",
                        Mock(return_value=Mock(return_value=dev)),
                    ),
                    ("hardware_teacher_link.service", Mock(return_value="active")),
                    ("hardware_teacher_link.failure_snapshot", Mock(return_value={})),
                    ("hardware_teacher_heads.HeadsRunner", Mock(return_value=runner)),
                    (
                        "hardware_teacher_heads.run",
                        Mock(side_effect=RecoveryRequired("uncertain completion")),
                    ),
                ):
                    stack.enter_context(patch(target, value))
                self.assertEqual(L.main(), 1)
                self.assertEqual(L.Device.call_count, 1)
            report = json.loads(output.read_text())
            self.assertFalse(report["window_restored"])
            self.assertFalse(report["reservation_restored"])
            self.assertIn("RecoveryRequired", report["error"])
            runner.admin.assert_not_called()

    def test_descriptor_fault_reaches_epilogue_instead_of_entry_trap(self):
        from types import SimpleNamespace
        from unittest.mock import Mock
        import struct
        import hardware_teacher_link as L
        from framenpu import JobFailed
        from framenpu.resident_gemm import SSR

        class FakeDevice:
            def _submit(self, loader, data, **kw):
                self.params = kw["params"]
                workers = []
                for worker in range(3):
                    row = [0] * 16
                    row[0], row[1], row[7], row[8] = 1, worker, SSR[worker], 0x87000000
                    if worker == 0:
                        row[2], row[3], row[4] = 2, 37, L.IMAGE_CACHED
                    workers.append(row)
                output = struct.pack("<I", 0xDECAFBAD).ljust(1024, b"\0")
                kw["_completion_validator"](
                    SimpleNamespace(workers=workers, output=output, kernel_reference_ticks=1000)
                )
                raise JobFailed("intentional trap")

        runner = L.LinkRunner.__new__(L.LinkRunner)
        runner.dev, runner.loader, runner.image, runner.sealed = FakeDevice(), None, bytes(4), True
        runner._check_reservation = Mock()
        plan = SimpleNamespace(table=bytes(128), table_at=T.WIN_LO)
        job = R.Job("rs", first=0, end=1)
        runner.submit(plan, job, linked=True, fault=True, descriptor_fault=True)
        self.assertEqual(runner.dev.params[5], 0)
        runner.submit(plan, job, linked=True, fault=True)
        self.assertEqual(runner.dev.params[5], 1)


@unittest.skipUnless(
    importlib.util.find_spec("numpy") and (MODEL / "hand.ncnn.param").exists(),
    "numpy and FNPU_TEACHER_MODEL required",
)
class HeadsPlanTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from framenpu.convolution import load_landmark_graph
        from framenpu.landmark import specifications

        cls.graph = load_landmark_graph(MODEL / "hand.ncnn.param", MODEL / "hand.ncnn.bin")
        cls.specs = specifications(cls.graph)

    def test_fused_plan_matches_host_heads_and_retains_tail(self):
        import numpy as np

        rng = np.random.default_rng(91)
        sh = T.shapes(self.specs)
        inputs = {b: rng.uniform(-1, 2, sh[b]).astype(np.float16) for b in R.INPUTS}
        ref_last = None
        for mode in ("baseline", "observe", "heads"):
            p = (
                R.build_plan(self.graph, self.specs, "merged")
                if mode == "baseline"
                else H.build_plan(self.graph, self.specs, observe=mode == "observe")
            )
            win = bytearray(T.WIN_HI - T.WIN_LO)
            for off, data in R.setup_writes(p).items():
                win[off - T.WIN_LO : off - T.WIN_LO + len(data)] = data
            for b, data in inputs.items():
                raw = T.grid_of(sh[b]).to_bytes(data)
                win[p.where[b] - T.WIN_LO : p.where[b] - T.WIN_LO + len(raw)] = raw
            rows = np.frombuffer(p.table, "<u4").reshape(-1, 32)
            for row in rows:
                self.assertTrue(H.native_ok(row) if row[0] == 6 else R.native_ok(row))
            R.simulate(p, win)

            def raw(name, n):
                at = p.where[name] - T.WIN_LO
                return bytes(win[at : at + n])

            if mode == "baseline":
                ref_raw = raw(R.FINAL, 2352 * 128)
                ref_last, _ = T.grid_of(sh[R.FINAL]).from_bytes(ref_raw, 672)
                reference = T.heads(self.graph, self.specs, ref_last)
                continue
            out = np.frombuffer(raw("heads", 512), "<f4")
            for name, values in H.split(out).items():
                np.testing.assert_allclose(values, reference[name], rtol=0, atol=H.CAPS[name])
            self.assertEqual(len(p.jobs), 1)
            self.assertLessEqual(p.end, T.WIN_HI)
            self.assertEqual(p.jobs[0].vtcm_peak, 14099)
            if mode == "observe":
                self.assertEqual(raw(R.FINAL, 2352 * 128), ref_raw)
                pool = np.frombuffer(raw("pooled", 2688), "<f4")
                np.testing.assert_allclose(
                    pool, H.pool_reference(ref_last), rtol=0, atol=H.CAPS["pool"]
                )
            else:
                self.assertEqual(len(rows), 46)
                self.assertEqual([int(r[10]) for r in rows if r[0] == 4], [4])


class HeadsBuildTests(unittest.TestCase):
    def test_linked_heads_fit_and_reference_machine_code_is_unchanged(self):
        try:
            from framenpu.compiler import Toolchain

            Toolchain.discover()
        except Exception as exc:
            self.skipTest(str(exc))
        import build_teacher_link as B

        with tempfile.TemporaryDirectory() as tmp:
            plain = B.build(Path(tmp) / "plain", stride2=True)
            heads = B.build(Path(tmp) / "heads", stride2=True, heads=True)
            self.assertEqual(
                plain["linked"]["sha256"],
                "ef5721f82e2f6fe20934032b0b9692ec422cae24aec863fe9c66c37aeae5a244",
            )
            self.assertEqual(plain["reference"], heads["reference"])
            self.assertEqual(plain["loader"], heads["loader"])
            self.assertLessEqual(heads["linked"]["bytes"], 0x3F80)
            self.assertTrue(heads["heads"])


if __name__ == "__main__":
    unittest.main()
