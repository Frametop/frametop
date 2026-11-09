"""Nested output scaling: host notification, delayed KWin updates and target positions."""
import copy
import importlib.util
import io
import pathlib
import unittest
from unittest.mock import Mock, patch

ROOT = pathlib.Path(__file__).resolve().parents[2]


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Scales(unittest.TestCase):
    def setUp(self):
        self.m = load_module("scale_layout", ROOT / "layout/ft_layout.py")
        self.layout = {"screens": [{"size": [3840, 2160], "metres": 2, "scale": 2},
                                   {"size": [1920, 1080], "metres": 1.5, "scale": 1.5}]}

    def test_screen_args(self):
        self.layout["screens"].insert(0, {"size": [1920, 1080], "metres": 1, "scale": 1})
        self.assertEqual(self.m.screen_args(self.layout),
                         "--screen 1920x1080@1.000 --screen 3840x2160@2.000x2 --screen 1920x1080@1.500x1.5")

    def test_mode_size_uses_current_mode_and_preserves_rotation(self):
        o = {"currentModeId": "current", "rotation": 2,
             "modes": [{"id": "other", "size": {"width": 640, "height": 480}},
                       {"id": "current", "size": {"width": 3840, "height": 2160}}],
             "size": {"width": 2160, "height": 3840}}
        self.assertEqual(self.m.mode_size(o), (2160, 3840))
        o["modes"] = []
        self.assertEqual(self.m.mode_size(o), (2160, 3840))

    def apply(self, backend="screens", updates=True):
        self.layout["screens"][1]["scale"] = 1
        old = [{"id": 1, "name": "WL-0", "enabled": True, "scale": 1, "priority": 1,
                "size": {"width": 3840, "height": 2160}, "pos": {"x": 0, "y": 0}},
               {"id": 2, "name": "WL-1", "enabled": True, "scale": 1,
                "size": {"width": 1920, "height": 1080}, "pos": {"x": 3840, "y": 0}}]
        new = copy.deepcopy(old)
        new[0]["scale"] = 2
        events = []
        calls = []

        def outputs(env):
            calls.append(env)
            events.append("outputs")
            return new if updates and len(calls) >= 4 else old

        def doctor(args, **kwargs):
            events.append(args)

        sock = Mock()
        sock.ask.side_effect = lambda cmd: events.append(cmd) or "ok"
        with patch.object(self.m, "backend", return_value=backend), \
             patch.object(self.m, "nested_env", return_value={"WAYLAND_DISPLAY": "test"}), \
             patch.object(self.m, "load_layout", return_value=self.layout), \
             patch.object(self.m, "screens_socket", return_value=sock), \
             patch.object(self.m, "primary_screen", return_value=0), \
             patch.object(self.m, "arrangement", return_value=[[0], [1]]), \
             patch.object(self.m, "outputs", side_effect=outputs), \
             patch.object(self.m.subprocess, "run", side_effect=doctor), \
             patch.object(self.m.time, "sleep") as sleep:
            result = self.m.apply_scales()
        return result, events, calls, sleep

    def test_host_scale_before_doctor_and_poll_until_updated(self):
        result, events, calls, sleep = self.apply()
        doctor_index = next(i for i, e in enumerate(events) if isinstance(e, list))
        self.assertLess(events.index("scale 1 2"), doctor_index)
        self.assertIn("output.1.scale.2", result)
        self.assertIn("output.2.position.1920,0", result)
        self.assertEqual(len(calls), 4)
        self.assertEqual(sleep.call_count, 2)
        self.assertEqual([e for e in events if isinstance(e, str) and e.startswith("scale 1")],
                         ["scale 1 2", "scale 1 2"])

    def test_positions_use_target_scale_even_when_poll_times_out(self):
        result, _, calls, sleep = self.apply(updates=False)
        self.assertIn("output.2.position.1920,0", result)
        self.assertEqual(len(calls), 11)  # initial query plus ten convergence queries
        self.assertEqual(sleep.call_count, 9)

    def test_timed_out_poll_sends_no_stale_scale(self):
        _, events, _, _ = self.apply(updates=False)
        # Only the layout's scale: KWin's old one would put that back on a KWin 6.6.
        self.assertEqual([e for e in events if isinstance(e, str) and e.startswith("scale 1")], ["scale 1 2"])
        self.assertIn("scale 2 1", events)

    def test_gamescope_does_not_notify_host_or_poll(self):
        _, events, calls, sleep = self.apply(backend="gamescope")
        self.assertFalse(any(isinstance(e, str) and e.startswith("scale ") for e in events))
        self.assertEqual(len(calls), 2)
        sleep.assert_not_called()


class HostProtocols(unittest.TestCase):
    def test_fractional_scale_required_without_warning_string(self):
        m = load_module("scale_update_check", ROOT / "scripts/update-check.py")
        lib = b"wl_compositor isn't supported by the host compositor\0wp_fractional_scale_manager_v1\0"
        for supported in (False, True):
            with self.subTest(supported=supported):
                built = b"wl_compositor\0" + (b"wp_fractional_scale_manager_v1\0" if supported else b"")
                def read(path, mode):
                    return io.BytesIO(lib if str(path).endswith("libkwin.so.6") else built)
                with patch.object(m.glob, "glob", return_value=["/usr/lib/libkwin.so.6"]), \
                     patch.object(m, "ft_screens_binary", return_value="/tmp/ft-screens"), \
                     patch("builtins.open", side_effect=read), patch.object(m, "report") as report:
                    m.check_kwin_host_protocols()
                self.assertEqual(report.call_args.args[0], "ok" if supported else "FAIL")
                if not supported:
                    self.assertIn("wp_fractional_scale_manager_v1", report.call_args.args[2])


if __name__ == "__main__":
    unittest.main()
