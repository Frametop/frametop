"""The push-to-talk key starts or stops dictation only when pressed on its own.
Needs the venv's Python (evdev): .venv/bin/python -m unittest"""
import asyncio
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
try:
    import frame_voice as fv  # noqa: E402
except ImportError as err:
    raise unittest.SkipTest(f"needs the venv's Python: {err}")

e = fv.e


class Dev:
    def __init__(self, path):
        self.path = path


KEYBOARD, OTHER, MOUSE = Dev("/dev/input/event3"), Dev("/dev/input/event7"), Dev("/dev/input/event9")


class Daemon(fv.FrameVoice):
    """on_key() and the state it reads, without models, mic or typing."""

    def __init__(self, hotkeys, mode):
        self.hotkeys = {e.ecodes[name]: name for name in hotkeys}
        self.kb_mode = mode
        self.dcfg = {"hold_delay": 0.0}
        self.hk_down = self.hk_chord = False
        self.hk_timer = None
        self.mods_down = set()
        self.mods_released = asyncio.Event()
        self._held_logged = None
        self.stream = None
        self.recording_source = None
        self.calls = []

    async def toggle_dictation(self, source):
        self.calls.append("toggle")

    def start_dictation(self, source):
        self.calls.append("start")
        self.recording_source = source

    async def stop_dictation(self):
        self.calls.append("stop")
        self.recording_source = None

    def cancel_dictation(self, chime=True):
        self.calls.append("cancel")
        self.recording_source = None


class Hotkey(unittest.IsolatedAsyncioTestCase):
    async def press(self, daemon, *events):
        """events: (device, key name, 1 for down or 0 for up)."""
        for dev, name, value in events:
            daemon.on_key(dev, e.ecodes[name], value)
            await asyncio.sleep(0)
        await asyncio.sleep(0.01)  # the tasks and the hold timer it started
        return daemon.calls

    async def test_tap_alone_toggles(self):
        d = Daemon(["KEY_RIGHTCTRL"], "toggle")
        self.assertEqual(await self.press(d, (KEYBOARD, "KEY_RIGHTCTRL", 1), (KEYBOARD, "KEY_RIGHTCTRL", 0)),
                         ["toggle"])

    async def test_modifier_held_before(self):
        for other in ("KEY_LEFTSHIFT", "KEY_RIGHTSHIFT", "KEY_LEFTALT", "KEY_LEFTMETA", "KEY_LEFTCTRL"):
            d = Daemon(["KEY_RIGHTCTRL"], "toggle")
            calls = await self.press(d, (KEYBOARD, other, 1), (KEYBOARD, "KEY_RIGHTCTRL", 1),
                                     (KEYBOARD, "KEY_RIGHTCTRL", 0), (KEYBOARD, other, 0))
            self.assertEqual(calls, [], other)

    async def test_modifier_released_first(self):
        d = Daemon(["KEY_RIGHTCTRL"], "toggle")
        self.assertEqual(await self.press(d, (KEYBOARD, "KEY_LEFTSHIFT", 1), (KEYBOARD, "KEY_RIGHTCTRL", 1),
                                          (KEYBOARD, "KEY_LEFTSHIFT", 0), (KEYBOARD, "KEY_RIGHTCTRL", 0)), [])

    async def test_modifier_held_on_another_keyboard(self):
        d = Daemon(["KEY_RIGHTCTRL"], "toggle")
        self.assertEqual(await self.press(d, (OTHER, "KEY_LEFTALT", 1), (KEYBOARD, "KEY_RIGHTCTRL", 1),
                                          (KEYBOARD, "KEY_RIGHTCTRL", 0), (OTHER, "KEY_LEFTALT", 0)), [])

    async def test_modifier_pressed_during(self):
        d = Daemon(["KEY_RIGHTCTRL"], "toggle")
        self.assertEqual(await self.press(d, (KEYBOARD, "KEY_RIGHTCTRL", 1), (KEYBOARD, "KEY_LEFTSHIFT", 1),
                                          (KEYBOARD, "KEY_LEFTSHIFT", 0), (KEYBOARD, "KEY_RIGHTCTRL", 0)), [])

    async def test_shortcut_with_the_key(self):
        d = Daemon(["KEY_RIGHTCTRL"], "toggle")
        self.assertEqual(await self.press(d, (KEYBOARD, "KEY_RIGHTCTRL", 1), (KEYBOARD, "KEY_C", 1),
                                          (KEYBOARD, "KEY_C", 0), (KEYBOARD, "KEY_RIGHTCTRL", 0)), [])

    async def test_tap_alone_after_a_shortcut(self):
        d = Daemon(["KEY_RIGHTCTRL"], "toggle")
        await self.press(d, (KEYBOARD, "KEY_LEFTSHIFT", 1), (KEYBOARD, "KEY_RIGHTCTRL", 1),
                         (KEYBOARD, "KEY_RIGHTCTRL", 0), (KEYBOARD, "KEY_LEFTSHIFT", 0))
        self.assertEqual(await self.press(d, (KEYBOARD, "KEY_RIGHTCTRL", 1), (KEYBOARD, "KEY_RIGHTCTRL", 0)),
                         ["toggle"])

    async def test_mouse_button_with_ctrl(self):
        d = Daemon(["BTN_SIDE"], "toggle")
        self.assertEqual(await self.press(d, (KEYBOARD, "KEY_LEFTCTRL", 1), (MOUSE, "BTN_SIDE", 1),
                                          (MOUSE, "BTN_SIDE", 0), (KEYBOARD, "KEY_LEFTCTRL", 0)), [])
        self.assertEqual(await self.press(d, (MOUSE, "BTN_SIDE", 1), (MOUSE, "BTN_SIDE", 0)), ["toggle"])

    async def test_hold_mode(self):
        d = Daemon(["KEY_RIGHTCTRL"], "hold")
        self.assertEqual(await self.press(d, (KEYBOARD, "KEY_LEFTSHIFT", 1), (KEYBOARD, "KEY_RIGHTCTRL", 1)), [])
        await self.press(d, (KEYBOARD, "KEY_RIGHTCTRL", 0), (KEYBOARD, "KEY_LEFTSHIFT", 0))
        self.assertEqual(d.calls, [])
        self.assertEqual(await self.press(d, (KEYBOARD, "KEY_RIGHTCTRL", 1)), ["start"])
        self.assertEqual(await self.press(d, (KEYBOARD, "KEY_RIGHTCTRL", 0)), ["start", "stop"])


if __name__ == "__main__":
    unittest.main()
