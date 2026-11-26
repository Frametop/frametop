"""Resolution comparison: coordinate units, confidence and oracle controls."""

import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / n) for n in ("src", "tests", "tools")]
import teacher_resolution_accuracy as R


@unittest.skipUnless(importlib.util.find_spec("numpy"), "NumPy required")
class ResolutionMetricTests(unittest.TestCase):
    def heads(self, x=0, y=0, presence=0.9, handedness=0.1):
        import numpy as np

        screen = np.zeros((21, 3), np.float32)
        screen[:, :2] = [x, y]
        return {
            "out0": screen.ravel(),
            "out3": np.zeros(63, np.float32),
            "out1": np.asarray([presence], np.float32),
            "out2": np.asarray([handedness], np.float32),
        }

    def test_coordinate_and_source_image_scales_are_explicit(self):
        a, b = self.heads(3, 4), self.heads()
        raw = R.compare_heads(a, b, roi_size=448)
        scaled = R.compare_heads(a, b, xy_scale=2, roi_size=448)
        self.assertEqual(raw["landmarks"]["mean_px"], 5)
        self.assertEqual(raw["landmarks"]["source_image_mean_px"], 10)
        self.assertEqual(scaled["landmarks"]["mean_px"], 10)
        self.assertEqual(scaled["landmarks"]["source_image_mean_px"], 20)
        self.assertEqual(raw["presence"], scaled["presence"])
        self.assertEqual(raw["world"], scaled["world"])

    def test_probability_head_names_and_threshold_changes(self):
        a, b = self.heads(presence=0.1, handedness=0.9), self.heads(presence=0.9, handedness=0.1)
        row = R.compare_heads(a, b)
        self.assertTrue(row["presence"]["threshold_0_5_changed"])
        self.assertTrue(row["handedness"]["threshold_0_5_changed"])
        self.assertAlmostEqual(row["presence"]["value"], 0.1)
        self.assertAlmostEqual(row["handedness"]["value"], 0.9)

    def test_negative_reference_crops_do_not_define_positive_landmark_summary(self):
        crops = {}
        for name, presence, dx in (("hand", 0.9, 3), ("background", 0.1, 300)):
            ref = self.heads(presence=presence)
            modes = {"native_224": R.compare_heads(ref, ref)}
            for mode in ("native_target", "target_upscaled_224"):
                modes[mode] = R.compare_heads(self.heads(x=dx, presence=presence), ref)
                modes[mode]["alternative_scale_target_to_224"] = R.compare_heads(
                    self.heads(x=dx, presence=presence), ref, xy_scale=2
                )
            crops[name] = {"modes": modes}
        summary = R.summarize({"crops": crops})
        self.assertEqual(summary["all_crops"]["crops"], 2)
        self.assertEqual(summary["reference_presence_positive"]["crops"], 1)
        self.assertEqual(
            summary["reference_presence_positive"]["modes"]["native_target"]["mean_px"], 3
        )
        self.assertEqual(
            summary["reference_presence_positive"]["alternative_scaled_native_target"]["mean_px"], 6
        )

    def test_changed_224_baseline_is_refused_before_resolution_comparison(self):
        import numpy as np

        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            source = folder / "source"
            gold = folder / "gold"
            source.mkdir()
            gold.mkdir()
            (source / "case").mkdir()
            (gold / "case").mkdir()
            (source / "case/frame.u8").write_bytes(bytes(16))
            manifest = {
                "param_sha256": R.PARAM_SHA,
                "weights_sha256": R.WEIGHTS_SHA,
                "crops": [
                    {
                        "name": "case",
                        "source_shape": [4, 4],
                        "roi_center_size_rotation": [2, 2, 4, 0],
                    }
                ],
            }
            (source / "manifest.json").write_text(json.dumps(manifest))
            files = {}
            for k, v in self.heads().items():
                p = gold / "case" / f"blob_{k}.f32"
                p.write_bytes(v.tobytes())
                files[k] = {"sha256": R.digest(p)}
            p = gold / "case/in0.f32"
            p.write_bytes(bytes(3 * 224 * 224 * 4))
            files["in0"] = {"sha256": R.digest(p)}
            (gold / "manifest.json").write_text(
                json.dumps(
                    {
                        "param_sha256": R.PARAM_SHA,
                        "weights_sha256": R.WEIGHTS_SHA,
                        "prior_manifest_sha256": R.digest(source / "manifest.json"),
                        "crops": [{"name": "case", "split": "heldout", "files": files}],
                    }
                )
            )
            oracle = SimpleNamespace(
                crop=lambda *args: np.zeros((224, 224), np.float32),
                run=lambda image: (self.heads(x=1), [672, 7, 7]),
            )
            graph = SimpleNamespace(
                param_sha256=R.PARAM_SHA, weights_sha256=R.WEIGHTS_SHA, shapes={"79": (672, 4, 4)}
            )
            with patch("framenpu.convolution.load_landmark_graph", return_value=graph):
                with self.assertRaisesRegex(ValueError, "reproduce pinned heads exactly"):
                    R.run(oracle, folder, source, gold, 128)

    @unittest.skipUnless(
        (Path(os.environ.get("FNPU_TEACHER_MODEL", "/nonexistent")) / "hand.ncnn.bin").exists(),
        "public teacher required",
    )
    def test_smaller_graph_has_same_heads_and_four_by_four_final_map(self):
        from framenpu.convolution import load_landmark_graph

        model = Path(os.environ["FNPU_TEACHER_MODEL"])
        args = (model / "hand.ncnn.param", model / "hand.ncnn.bin")
        a = load_landmark_graph(*args)
        b = load_landmark_graph(*args, input_shape=(3, 128, 128))
        self.assertEqual(a.shapes["79"], (672, 7, 7))
        self.assertEqual(b.shapes["79"], (672, 4, 4))
        for name in R.ORDER:
            self.assertEqual(a.shapes[name], b.shapes[name])
