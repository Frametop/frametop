"""Public API and standalone-checkout regressions; no device access."""

import contextlib
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

from framenpu import CompilerError, Dense, InvalidModel, Job, Model
from framenpu.cli import main
from framenpu.compiler import Toolchain, build_job


class ContainerSchemaTests(unittest.TestCase):
    def test_nonobject_fjob_metadata_is_a_value_error(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bad.fjob"
            for metadata in ([], "text", None, 3, {"abi": 1}):
                with self.subTest(metadata=metadata):
                    raw = json.dumps(metadata).encode()
                    path.write_bytes(b"FJOB" + struct.pack("<I", len(raw)) + raw + b"\0" * 4)
                    with self.assertRaises(ValueError):
                        Job.load(path)

    def test_job_name_and_hmx_types(self):
        for name in ("", "x" * 129, []):
            with self.subTest(name=name), self.assertRaises(ValueError):
                Job(b"\0" * 4, name=name)
        with self.assertRaises(ValueError):
            Job(b"\0" * 4, requires_hmx="false")

    def test_invalid_model_manifest_shapes_are_domain_errors(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bad.fnpu"
            for manifest in (
                [],
                None,
                "text",
                {"format": "framenpu-model", "version": True, "layers": []},
                {"format": "framenpu-model", "version": 1, "input_features": 1, "layers": [[]]},
                {
                    "format": "framenpu-model",
                    "version": 1,
                    "input_features": 1,
                    "layers": [{"op": "Dense", "weights": [], "bias": {}}],
                },
            ):
                with self.subTest(manifest=manifest):
                    with zipfile.ZipFile(path, "w") as archive:
                        archive.writestr("model.json", json.dumps(manifest))
                    with self.assertRaises(InvalidModel):
                        Model.load(path)

    def test_model_serialization_has_canonical_timestamps_and_bytes(self):
        model = Model(1, [Dense([[1]], [0.5])], name="canonical")
        with tempfile.TemporaryDirectory() as directory:
            first, second = Path(directory) / "a.fnpu", Path(directory) / "b.fnpu"
            model.save(first)
            model.save(second)
            self.assertEqual(first.read_bytes(), second.read_bytes())
            with zipfile.ZipFile(first) as archive:
                self.assertTrue(
                    all(i.date_time == (1980, 1, 1, 0, 0, 0) for i in archive.infolist())
                )
            self.assertEqual(Model.load(first).layers, model.layers)


class ToolingTests(unittest.TestCase):
    def test_cli_error_is_actionable_without_traceback(self):
        output = io.StringIO()
        with contextlib.redirect_stderr(output), self.assertRaises(SystemExit) as raised:
            main(["inspect-model", "/missing-model.fnpu"])
        self.assertEqual(raised.exception.code, 2)
        self.assertIn("framenpu:", output.getvalue())
        self.assertNotIn("Traceback", output.getvalue())

    def test_missing_compiler_names_environment_override(self):
        with patch.dict(os.environ, {"CLANG": "missing-hexagon-compiler"}):
            with patch("framenpu.compiler.shutil.which", return_value=None):
                with self.assertRaisesRegex(CompilerError, "CLANG"):
                    Toolchain.discover()

    def test_compiler_rejects_host_only_clang(self):
        with patch("framenpu.compiler.shutil.which", side_effect=lambda name: name):
            with patch("framenpu.compiler._run", return_value="x86, aarch64"):
                with self.assertRaisesRegex(CompilerError, "Hexagon"):
                    Toolchain.discover()

    def test_custom_source_basename_cannot_overwrite_sdk_object(self):
        calls = []

        def execute(command):
            calls.append(command)
            if "--only-section=.payload" in command:
                Path(command[-1]).write_bytes(b"\0" * 4)
            return ""

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "entry.S"
            source.write_text("placeholder; compilation is mocked")
            output = root / "nested" / "job.fjob"
            with patch("framenpu.compiler._run", side_effect=execute):
                job = build_job(
                    source,
                    output,
                    build_dir=root / "build",
                    toolchain=Toolchain("clang", "linker", "objcopy"),
                )
            self.assertEqual(Job.load(output), job)
        objects = [command[-1] for command in calls if "-c" in command]
        self.assertEqual(len(objects), 3)
        self.assertEqual(len(set(objects)), 3)

    def test_hardware_smoke_requires_explicit_opt_in(self):
        for name in (
            "hardware_smoke.py",
            "hardware_dispatch.py",
            "hardware_hmx.py",
            "hardware_multitile.py",
            "hardware_hvx.py",
            "hardware_readback.py",
        ):
            with self.subTest(script=name):
                result = subprocess.run(
                    [sys.executable, str(ROOT / "tests" / name)],
                    text=True,
                    capture_output=True,
                    cwd=tempfile.gettempdir(),
                    check=False,
                )
                self.assertEqual(result.returncode, 2)
                self.assertIn("--run-on-device is required", result.stderr)

    def test_examples_help_from_unrelated_directory(self):
        for name in ("make_xor.py", "run_xor.py", "run_custom.py"):
            with self.subTest(example=name):
                result = subprocess.run(
                    [sys.executable, str(ROOT / "examples" / name), "--help"],
                    text=True,
                    capture_output=True,
                    cwd=tempfile.gettempdir(),
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("usage:", result.stdout)


if __name__ == "__main__":
    unittest.main()
