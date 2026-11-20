"""Full Keyboard's logical key IDs (its layouts' names, which its text sink reports) as Linux
evdev key codes, for typing named keys on Frametop's desktop through the input relay ("kbkey").

A sink "key" event is one press and release of a named control, with the modifiers held for
it; characters come as "commit" text instead. So the desktop gets: modifiers down, the key
down and up, modifiers up (presses()).
"""

# linux/input-event-codes.h
EVDEV = {
    "Escape": 1, "Digit1": 2, "Digit2": 3, "Digit3": 4, "Digit4": 5, "Digit5": 6, "Digit6": 7,
    "Digit7": 8, "Digit8": 9, "Digit9": 10, "Digit0": 11, "Minus": 12, "Equal": 13,
    "Backspace": 14, "Tab": 15, "KeyQ": 16, "KeyW": 17, "KeyE": 18, "KeyR": 19, "KeyT": 20,
    "KeyY": 21, "KeyU": 22, "KeyI": 23, "KeyO": 24, "KeyP": 25, "BracketLeft": 26,
    "BracketRight": 27, "Enter": 28, "ControlLeft": 29, "KeyA": 30, "KeyS": 31, "KeyD": 32,
    "KeyF": 33, "KeyG": 34, "KeyH": 35, "KeyJ": 36, "KeyK": 37, "KeyL": 38, "Semicolon": 39,
    "Quote": 40, "Backquote": 41, "ShiftLeft": 42, "Backslash": 43, "KeyZ": 44, "KeyX": 45,
    "KeyC": 46, "KeyV": 47, "KeyB": 48, "KeyN": 49, "KeyM": 50, "Comma": 51, "Period": 52,
    "Slash": 53, "ShiftRight": 54, "NumpadMultiply": 55, "AltLeft": 56, "Space": 57,
    "CapsLock": 58, "F1": 59, "F2": 60, "F3": 61, "F4": 62, "F5": 63, "F6": 64, "F7": 65,
    "F8": 66, "F9": 67, "F10": 68, "NumLock": 69, "ScrollLock": 70, "Numpad7": 71,
    "Numpad8": 72, "Numpad9": 73, "NumpadSubtract": 74, "Numpad4": 75, "Numpad5": 76,
    "Numpad6": 77, "NumpadAdd": 78, "Numpad1": 79, "Numpad2": 80, "Numpad3": 81, "Numpad0": 82,
    "NumpadDecimal": 83, "ZenkakuHankaku": 85, "IntlBackslash": 86, "F11": 87, "F12": 88,
    "IntlRo": 89, "Convert": 92, "KanaMode": 93, "NonConvert": 94, "NumpadEnter": 96,
    "ControlRight": 97, "NumpadDivide": 98, "PrintScreen": 99, "AltRight": 100, "Home": 102,
    "ArrowUp": 103, "PageUp": 104, "ArrowLeft": 105, "ArrowRight": 106, "End": 107,
    "ArrowDown": 108, "PageDown": 109, "Insert": 110, "Delete": 111, "Pause": 119,
    "NumpadComma": 121, "IntlYen": 124, "MetaLeft": 125, "MetaRight": 126, "ContextMenu": 127,
}
# The sink's modifier names (its shell_sink.cpp): the left-hand key for each, AltGr the right Alt.
MODIFIERS = {"Ctrl": 29, "Shift": 42, "Alt": 56, "AltGr": 100, "Super": 125}
# Cut, Copy and Paste come as KeyX/KeyC/KeyV with Ctrl already (integration.md, Text sink).


def presses(key, modifiers):
    """The (code, 1|0) presses that type a sink "key" event on the desktop, or [] for a key the
    desktop has no code for (Full Keyboard's own controls, such as Emoji)."""
    code = EVDEV.get(key)
    if code is None:
        return []
    held = [MODIFIERS[m] for m in dict.fromkeys(modifiers) if m in MODIFIERS and MODIFIERS[m] != code]
    return ([(m, 1) for m in held] + [(code, 1), (code, 0)] + [(m, 0) for m in reversed(held)])
