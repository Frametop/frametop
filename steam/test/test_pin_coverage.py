"""Source integration tripwires; these do not substitute for in-headset tests."""
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class Coverage(unittest.TestCase):
    def test_compositor_show_calls_go_through_gate(self):
        source = (ROOT / "screens/vr.cpp").read_text()
        raw = [line.strip() for line in source.splitlines() if "->ShowOverlay(" in line]
        self.assertEqual(len(raw), 2, "New overlays must use PinShow, not raw ShowOverlay")
        self.assertIn("ft_pin_allowed()", raw[0])
        self.assertIn("ft_pin_allowed()", raw[1])
        self.assertIn("visible = visible && ft_pin_allowed()", source)
        self.assertIn("denied || wasDenied", source)
        self.assertIn("PollNextOverlayEvent(it->first", source)

    def test_compositor_rejects_input_commands_while_locked(self):
        source = (ROOT / "screens/compositor.c").read_text()
        gate = source.split("if (s->vr && !ft_pin_allowed() &&", 1)[1].split("{", 1)[0]
        for command in ('"input "', '"key "', '"pad "', '"wheel "', '"hwheel "', '"vrkeyboard "', '"frunner on"'):
            self.assertIn(command, gate)

    def test_relay_commits_no_text_past_the_gate(self):
        source = (ROOT / "input/input-relay.py").read_text()
        body = source.split("    def commit_text(", 1)[1].split("\n    def ", 1)[0]
        self.assertLess(body.index("ft_pinlock.allowed()"), body.index("sendto("))

    def test_voice_runs_no_command_while_locked(self):
        source = (ROOT / "voice/ft-voiced").read_text()
        wake = source.split("    def on_wake(", 1)[1].split("\n    def ", 1)[0]
        self.assertLess(wake.index("ft_pinlock.allowed()"), wake.index("run_hook("))
        finish = source.split("    async def finish_command(", 1)[1].split("\n    async def ", 1)[0]
        self.assertLess(finish.index("ft_pinlock.allowed()"), finish.index("voice_commands.match("))

    def test_lock_path_cannot_stop_apps(self):
        source = (ROOT / "screens/vr.cpp").read_text()
        body = source.split("void ft_vr_poll(", 1)[1].split("    RefreshPoses();", 1)[0]
        self.assertIn("HideOverlay", body)
        for forbidden in ("SIGSTOP", "SIGTERM", "SIGKILL", "kill(", "system(", "shutdown("):
            self.assertNotIn(forbidden, body)

    def test_input_released_before_focus_clears(self):
        source = (ROOT / "screens/compositor.c").read_text()
        body = source.split("if (denied && !s->pin_denied)", 1)[1].split("s->pin_denied = denied", 1)[0]
        clear = body.index("wlr_seat_pointer_notify_clear_focus")
        self.assertLess(body.index("release_all_keys"), clear)
        self.assertLess(body.index("release_seat_buttons"), clear)
        self.assertIn("s->pointer_focus = NULL", body)

    def test_independent_export_channels_refuse_protected_mode(self):
        for name in ("remote-ctl.sh", "remote-desktop.sh", "vnc-bridge.sh"):
            self.assertIn("ft_pinlock.enabled()", (ROOT / "session" / name).read_text(), name)
        self.assertIn("if ft_pinlock.enabled():", (ROOT / "session/ft-notifyfwd").read_text())

    def test_auxiliary_panels_have_security_gate(self):
        for name in ("frunner/ft-spotpanel.cpp", "gaze/panel/ft-gazepanel.cpp",
                     "hands/rec/panel/ft-handpanel.cpp", "screens/handview.cpp", "keyboard/src/vr.cpp"):
            self.assertIn("ft_pin_allowed()", (ROOT / name).read_text(), name)


if __name__ == "__main__":
    unittest.main()
