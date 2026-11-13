import importlib.machinery
import importlib.util
import io
from pathlib import Path
import struct
import tempfile
import time
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
loader = importlib.machinery.SourceFileLoader("framehands_cli", str(ROOT / "framehands"))
spec = importlib.util.spec_from_loader(loader.name, loader)
cli = importlib.util.module_from_spec(spec)
loader.exec_module(cli)


class FrameHandsTest(unittest.TestCase):
    def test_both_hands_and_stale_data(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(cli, "RUN", Path(tmp)):
            data = bytearray(64 + 2 * 272)
            data[:8] = b"FHHANDS1"
            struct.pack_into("<IIQQQII", data, 8, 2, len(data), 2,
                             time.monotonic_ns(), time.monotonic_ns(), 2, 0)
            struct.pack_into("<I", data, 64 + 4, 0)
            struct.pack_into("<I", data, 64 + 272 + 4, 1)
            path = Path(tmp) / "hands"
            path.write_bytes(data)
            self.assertEqual(cli.hands(), "left, right")
            struct.pack_into("<Q", data, 24, time.monotonic_ns() - 2_000_000_000)
            path.write_bytes(data)
            self.assertEqual(cli.hands(), "hand data stale")

    def test_torn_shared_frame_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "hands"
            data = bytearray(64 + 2 * 272)
            data[:8] = b"FHHANDS1"
            struct.pack_into("<Q", data, 16, 3)
            path.write_bytes(data)
            self.assertIsNone(cli.shared(path, b"FHHANDS1", len(data)))

    def test_both_sides_gestures_are_reported(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(cli, "RUN", Path(tmp)):
            data = bytearray(64 + 6 * 64)
            data[:8] = b"FHGEST01"
            struct.pack_into("<IIQQQ", data, 8, 3, len(data), 2,
                             time.monotonic_ns(), time.monotonic_ns())
            struct.pack_into("<IIII", data, 64, 3, 1, 1, 0)
            struct.pack_into("<IIII", data, 128, 3, 2, 1, 0)
            (Path(tmp) / "gestures").write_bytes(data)
            output = io.StringIO()
            with patch("sys.stdout", output):
                self.assertEqual(cli.gestures(), 0)
            self.assertIn("left pinch: held", output.getvalue())
            self.assertIn("right pinch: held", output.getvalue())

    def test_service_selection_is_isolated(self):
        with patch.object(cli, "config", return_value={"TRACKING": "1", "RENDER": "0"}):
            self.assertEqual(cli.selected(), cli.UNITS[:2])
        with patch.object(cli, "config", return_value={"TRACKING": "1", "RENDER": "0", "GESTURE_INPUT": "1"}):
            self.assertEqual(cli.selected(), cli.UNITS[:2] + ("framehands-input.service",))
        with patch.object(cli, "config", return_value={"TRACKING": "0"}):
            self.assertEqual(cli.selected(), ())

    def test_performance_presets_keep_full_accuracy_by_default(self):
        with patch.object(cli, "config", return_value={"PERFORMANCE": "balanced"}):
            self.assertEqual(cli.tracker_args(), ("--status", "60"))
        with patch.object(cli, "config", return_value={"PERFORMANCE": "power"}):
            self.assertIn("--int8", cli.tracker_args())

    def test_config_write_preserves_unrelated_settings(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(cli, "CONFIG", Path(tmp) / "config"):
            cli.CONFIG.write_text("HANDS_MODEL=stock\nAUTO_START=0\n")
            cli.set_config("AUTO_START", "1")
            self.assertEqual(cli.config(), {"HANDS_MODEL": "stock", "AUTO_START": "1"})

    def test_services_follow_steamvr_without_desktop_dependencies(self):
        for path in (ROOT / "services").glob("*.service"):
            unit = path.read_text().lower()
            self.assertIn("partof=steamvr.service", unit)
            self.assertIn("requisite=steamvr.service", unit)
            self.assertNotIn("frametop", unit)
            self.assertNotIn("desktop", unit)


if __name__ == "__main__":
    unittest.main()
