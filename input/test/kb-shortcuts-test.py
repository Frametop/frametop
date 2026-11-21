#!/usr/bin/env python3
"""input/kb_shortcuts.py: Full Keyboard's shortcut letters on other layouts, offline (no xkb).

  python3 input/test/kb-shortcuts-test.py
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from kb_shortcuts import US_LETTERS, shortcut_code  # noqa: E402

CTRL, RCTRL, ALT, ALTGR, SHIFT, SUPER = 29, 97, 56, 100, 42, 125
KEY_A, KEY_Q, KEY_W, KEY_Y, KEY_Z, KEY_C, DIGIT1, ENTER = 30, 16, 17, 21, 44, 46, 2, 28
# The parts of three layouts these tests press (character -> (code, modifiers)), as layout_keys has them.
AZERTY = {"a": (KEY_Q, ()), "q": (KEY_A, ()), "z": (KEY_W, ()), "w": (KEY_Z, ()), "c": (KEY_C, ()),
          "&": (DIGIT1, ()), "1": (DIGIT1, (SHIFT,))}
QWERTZ = {"z": (KEY_Y, ()), "y": (KEY_Z, ()), "c": (KEY_C, ())}
RUSSIAN = {"ф": (KEY_A, ()), "с": (KEY_C, ())}  # ф, с: no Latin letters


def test_us_letters():
    assert len(US_LETTERS) == 26 and set(US_LETTERS.values()) == set("abcdefghijklmnopqrstuvwxyz")
    assert (US_LETTERS[KEY_A], US_LETTERS[KEY_Q], US_LETTERS[KEY_Z], US_LETTERS[50]) == ("a", "q", "z", "m")


def test_shortcut_letter_goes_as_its_key():
    assert shortcut_code(KEY_A, {CTRL}, AZERTY) == KEY_Q        # Ctrl+A selects all on AZERTY
    assert shortcut_code(KEY_Z, {CTRL}, AZERTY) == KEY_W        # Ctrl+Z undoes
    assert shortcut_code(KEY_Z, {CTRL, SHIFT}, QWERTZ) == KEY_Y  # Ctrl+Shift+Z redoes on QWERTZ
    assert shortcut_code(KEY_Q, {ALT}, AZERTY) == KEY_A
    assert shortcut_code(KEY_C, {SUPER}, AZERTY) == KEY_C
    assert shortcut_code(KEY_A, {RCTRL}, AZERTY) == KEY_Q


def test_place_kept():
    assert shortcut_code(KEY_A, set(), AZERTY) == KEY_A          # no shortcut modifier
    assert shortcut_code(KEY_A, {SHIFT}, AZERTY) == KEY_A
    assert shortcut_code(KEY_A, {ALTGR}, AZERTY) == KEY_A
    assert shortcut_code(KEY_A, {CTRL}, RUSSIAN) == KEY_A        # no Latin a: the desktop's fallback
    assert shortcut_code(DIGIT1, {CTRL}, AZERTY) == DIGIT1       # not a letter
    assert shortcut_code(ENTER, {CTRL}, AZERTY) == ENTER
    assert shortcut_code(KEY_A, {CTRL}, {"a": (KEY_Q, (SHIFT,))}) == KEY_A  # only with Shift: kept
    assert shortcut_code(CTRL, {CTRL}, AZERTY) == CTRL


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
            print("ok", name)
