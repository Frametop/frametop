"""Native controller clicks use SteamVR's focus stack, without stealing input."""
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import Mock, patch


ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("dashpatch", ROOT / "pointer/dashpatch/ft-dashpatch.py")
dashpatch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dashpatch)


class NativeFocusTests(unittest.TestCase):
    def setUp(self):
        self.focus = dashpatch.NativeFocus()
        self.out = Mock()

    def test_native_window_hands_typing_back_once(self):
        self.focus.update("frametop.float.1", self.out)
        self.focus.update("gamescope.gamescope-0.window.70", self.out)
        self.focus.update("gamescope.gamescope-0.window.70", self.out)
        self.out.sendto.assert_called_once_with(b"click gamescope.gamescope-0.window.70", dashpatch.SCREENS)

    def test_returning_to_native_window_after_desktop_click(self):
        for key in ("gamescope.gamescope-0.window.70", "frametop.screen.1", "gamescope.gamescope-0.window.70"):
            self.focus.update(key, self.out)
        self.assertEqual(self.out.sendto.call_count, 2)

    def test_our_keyboard_controls_unknown_and_cursor_do_not_steal_typing(self):
        for key in ("org.framekeyboard.panel", "frametop.float.1.bar", "frametop.frunner", "-",
                    "gamescope.gamescope-0.cursor.70", "gamescope.gamescope-0.window.70.thumb",
                    "gamescope.gamescope-0.window.70.layer1", "system.systemui"):
            self.focus.update(key, self.out)
        self.out.sendto.assert_not_called()

    def test_missing_desktop_retries_without_restarting_anything(self):
        self.out.sendto.side_effect = [OSError("desktop not running"), None]
        for _ in range(3):
            self.focus.update("gamescope.gamescope-0.window.70", self.out)
        self.assertEqual(self.out.sendto.call_count, 2)

    def test_watcher_keeps_mode_protocol_and_sends_native_focus_change(self):
        ws = Mock()
        states = ["0 0 0 frametop.float.1", "0 0 0 gamescope.gamescope-0.window.70",
                  "0 0 0 gamescope.gamescope-0.window.70"]
        import json
        ws.recv.side_effect = [json.dumps({"id": i, "result": {"result": {"value": v}}})
                               for i, v in enumerate(states, 1)]
        with patch.object(dashpatch.socket, "socket", return_value=self.out), \
             patch.object(dashpatch, "dashboard", return_value="ws://test"), \
             patch.object(dashpatch, "_WebSocket", return_value=ws), \
             patch.object(dashpatch.time, "sleep", side_effect=[None, None, None, StopIteration]), \
             patch.object(dashpatch.time, "monotonic", return_value=10):
            with self.assertRaises(StopIteration):
                dashpatch.watch_steamvr()
        self.assertEqual([c.args[0] for c in self.out.sendto.call_args_list],
                         [b"steamvrmode 0 0 0", b"click gamescope.gamescope-0.window.70"])


if __name__ == "__main__":
    unittest.main()
