"""ft-pointer.cpp's shape and settings, read from its source (it needs OpenVR to build, so no
test runs it). DEE-322 split its 3,300-line main() into a Pointer struct; these pin what that
split had to keep: every frametop.conf setting it reads, with its default and range, in its
order; main() as Init() and a Tick() loop; Tick()'s sections in the loop's old order; and this
tick's readings (Frame, Pointer::fr) set before any section reads them.
Run: python3 pointer/tests/test_pointer_shape.py"""
import re
import unittest
from pathlib import Path

SRC = (Path(__file__).resolve().parents[1] / "helper" / "ft-pointer.cpp").read_text()

# (key, default, (low, high) for std::clamp, "on" for a "!= 0" switch, None for neither), as
# main() read them before the split (2058f75).
SETTINGS = [
    ("POINTER_DISTANCE", 1.5, (0.3, 10.0)),
    ("POINTER_CURSOR_DEG", 0.4, (0.05, 5.0)),
    ("POINTER_ORIGIN_FRACTION", 0.95, (0.0, 0.98)),
    ("POINTER_ORIGIN_MARGIN", 0.15, (0.0, 1.0)),
    ("POINTER_SCENE_RADIUS", 0.5, (0.05, 2.0)),
    ("POINTER_EDGE_REACH", 0.3, (0.0, 2.0)),
    ("LAYOUT_GRAB_OFFSET", 0.075, (0.0, 1.0)),
    ("LAYOUT_SLIDE_SPEED", 0.5, (0.02, 2.0)),
    ("POINTER_LEASH_DEG", 10, (0.0, 90.0)),
    ("POINTER_LEASH_RETURN", 0.2, (0.0, 5.0)),
    ("POINTER_LEASH_DELAY", 0.2, (0.0, 5.0)),
    ("POINTER_FOLLOW_REACH", 70, (10.0, 89.0)),
    ("POINTER_FOLLOW", 0, "on"),
    ("POINTER_GAZE_RETAKE", 5, (1.0, 45.0)),
    ("POINTER_GAZE_NUDGE_MAX", 55, (1.0, 110.0)),
    ("POINTER_GAZE_HOLD", 0.5, (0.1, 5.0)),
    ("POINTER_GAZE_CONTROL_HOLD", 0.12, (0.05, 5.0)),
    ("POINTER_GAZE_SHOW", 1, (0.0, 30.0)),
    ("POINTER_HEAD_DEADZONE", 0.5, (0.0, 5.0)),
    ("POINTER_KEY_TAP", 0.25, (0.0, 1.0)),
    ("POINTER_GAZE", 0, "on"),
    ("POINTER_HANDS", 0, "on"),
    ("POINTER_GAZE_PINCH", 1, "on"),
    ("POINTER_PINCH_NO_RAY", 0, "on"),
    ("POINTER_RAY_BEAM", 0, "on"),
    ("POINTER_GAZE_CHORD", 0, "on"),
    ("POINTER_GAZE_RIGHT_DRAG", 0, (0.0, 5.0)),
    ("POINTER_GAZE_TWO_HAND", 1, "on"),
    ("POINTER_PINCH_MAX_SPEED", 1.0, (0.0, 10.0)),
    ("POINTER_TYPING_HIDE", 1.5, (0.0, 30.0)),
    ("POINTER_GAZE_SCROLL", 1.0, (-5.0, 5.0)),
    ("POINTER_GAZE_SCROLL_GAP", 0.035, (0.01, 0.08)),
    ("POINTER_RAY_THREE_WAIT", 110, (0.0, 400.0)),
    ("POINTER_PINCH_GAIN", 0.5, (0.05, 3.0)),
    ("POINTER_PINCH_DEADZONE", 1.5, (0.0, 10.0)),
    ("POINTER_GRIP_GAIN", 1.0, (0.05, 3.0)),
    ("POINTER_GRIP_BELOW", 0.35, (0.05, 1.0)),
    ("POINTER_PINCH_TYPING", 2.0, (0.0, 10.0)),
    ("POINTER_TYPING_RAISE", 0.08, (0.02, 0.5)),
    ("POINTER_TYPING_BELOW", 0.2, (0.0, 1.0)),
    ("POINTER_TYPING_POSTURE", 0.3, (0.0, 5.0)),
    ("POINTER_TYPING_RECENT", 600, (0.0, 86400.0)),
    ("POINTER_TYPING_KEEP", 300, (0.0, 86400.0)),
    ("POINTER_TYPING_MOUSE", 10, (0.0, 600.0)),
    ("POINTER_DESK_MIN", 0.8, (0.5, 1.0)),
    ("POINTER_HAND_RAY", 1, "on"),
    ("POINTER_RAY_DEADZONE", 2.5, (0.0, 10.0)),
    ("POINTER_RAY_PUSH", 1, "on"),
    ("POINTER_RAY_SCROLL", 1.0, (-5.0, 5.0)),
    ("POINTER_RAY_RELEASE", 2.0, (0.0, 60.0)),
    ("POINTER_RAY_GRACE", 0.5, (0.0, 3.0)),
    ("POINTER_RAY_SMOOTH_MINCUTOFF", 0.8, (0.05, 30.0)),
    ("POINTER_RAY_SMOOTH_BETA", 0.06, (0.0, 5.0)),
    ("POINTER_RAY_SMOOTH_HOLD", 0.5, (0.05, 30.0)),
    ("POINTER_RAY_STEADY", 1, "on"),
    ("POINTER_RAY_TORSO_WRISTS", 1, "on"),
    ("POINTER_RAY_HAND_WEIGHT", 0, (0.0, 1.0)),
    ("POINTER_RAY_HAND_PITCH", -30, (-90.0, 90.0)),
    ("POINTER_RAY_HAND_YAW", 8, (-90.0, 90.0)),
    ("POINTER_RAY_ANCHOR", 0, "on"),
    ("POINTER_RAY_READY", 6, (0.0, 30.0)),
    ("POINTER_RAY_READY_HIDE", 10, (0.0, 40.0)),
    ("POINTER_RAY_FIST_CURL", 1.1, (0.0, 2.0)),
    ("POINTER_RAY_RAISED", 0, (0.0, 100.0)),
    ("POINTER_RAY_FORWARD", 0, (0.0, 100.0)),
    ("POINTER_RAY_TAP", 0.3, (0.0, 2.0)),
    ("POINTER_RAY_TAP_DEADZONE", 6.0, (0.0, 30.0)),
    ("POINTER_GAZE_HAND_RAY", 0, "on"),
    ("POINTER_GAZE_PINCH_DEADZONE", 4.0, (0.0, 15.0)),
    ("POINTER_GAZE_PINCH_GAIN", 0.3, (0.05, 2.0)),
    ("POINTER_CONTROLLER_PICKUP", 1, (0.5, 5.0)),
    ("POINTER_LASER_WIDTH", 0.8, None),
]

SECTIONS = ["HeadsetOff", "GazeAwake", "RelayCommands", "ReadFrame", "ClaimAndTouch", "OverlaysAndRole",
            "LastUsedWins", "RecenterAndGaze", "Handoff", "NearClaims", "DeviceAndKeyPresses", "ReadHands",
            "Gestures", "RayDrive", "HeadFollow", "SlowWorkAndPose", "HeldPresses", "Events", "Idle"]
# Frame field -> the section that sets it each tick.
SETS = {"all": "ReadFrame", "hmdRaw": "ReadFrame", "tnow": "ReadFrame", "eye": "ReadFrame",
        "hg": "ReadHands", "handOk": "ReadHands", "rayMode": "RayDrive"}


def body(head):
    """The text of a function or struct from its first line to its closing brace at column 0."""
    i = SRC.index(head)
    return SRC[i:SRC.index("\n}", i)]


def settings():
    pat = re.compile(r'(?:std::clamp\()?ConfDouble\((?:conf|ReadConfig\(\)), "(\w+)", ([-\d.]+)\)'
                     r'(?:, ([-\d.]+), ([-\d.]+)\))?( != 0)?')
    return [(k, float(d), "on" if on else (float(lo), float(hi)) if lo else None)
            for k, d, lo, hi, on in pat.findall(SRC)]


class PointerShape(unittest.TestCase):
    def test_settings_keep_their_defaults_and_ranges(self):
        want = [(k, float(d), r if r in (None, "on") else (float(r[0]), float(r[1])))
                for k, d, r in SETTINGS]
        self.assertEqual(settings(), want)

    def test_main_is_init_then_tick(self):
        main = body("int main() {")
        self.assertRegex(main, r"Pointer p;\s*p\.Init\(\);\s*while \(p\.Tick\(\)\) \{\s*\}\s*return 0;")

    def test_tick_runs_the_sections_in_order(self):
        tick = body("bool Pointer::Tick() {")
        self.assertEqual(re.findall(r"(\w+)\(\)", tick)[1:], SECTIONS)  # [0] is Tick itself
        self.assertIn("if (!Events()) return false;", tick)  # SteamVR quit ends the loop
        for name in SECTIONS:
            self.assertRegex(SRC, rf"\n(void|bool) Pointer::{name}\(\) \{{")

    def test_frame_is_set_before_it_is_read(self):
        frame = body("struct Frame {")
        fields = set(re.findall(r"\b(\w+)(?:\[[^\]]*\])?(?: = [^;]+)?;", frame))
        self.assertEqual(fields, set(SETS))
        for field, setter in SETS.items():
            writers = [s for s in SECTIONS
                       if re.search(rf"fr\.{field}\b(\[[^\]]*\])? =[^=]|TrackingPose\([^;]*\bfr\.{field}\b|Read\(fr\.{field}\)",
                                    body(f"Pointer::{s}() {{"))]
            self.assertEqual(writers, [setter], field)
            for s in SECTIONS[:SECTIONS.index(setter)]:
                self.assertNotRegex(body(f"Pointer::{s}() {{"), rf"\bfr\.{field}\b", f"{s} reads fr.{field}")
        self.assertEqual(len(re.findall(r"\bfr\.\w+ =[^=]", body("struct Pointer {"))), 0)  # helpers only read


if __name__ == "__main__":
    unittest.main()
