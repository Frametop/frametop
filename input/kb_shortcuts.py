"""Full Keyboard's shortcuts on a desktop with a layout of its own, for the input relay.

Full Keyboard's text sink names a shortcut's letter, not its key's place: with Ctrl, Alt or
Super held, the key that shows "a" comes as KeyA on every language, AZERTY's included (Latin
layouts send their shown letter; Cyrillic and IME ones the Latin letter of their place), and
Cut, Copy and Paste as Ctrl with KeyX, KeyC and KeyV. ft-kbhost passes those on as US evdev
codes ("kbkey"). Pressed as they are, a French desktop would read Ctrl+KeyA as Ctrl+Q. So the
letter goes as the key that types it on the layout typing goes through (the relay's
DesktopLayout keys). A letter that layout has no plain key for (a Cyrillic or Greek one) keeps
its place, where the desktop's Latin fallback finds it.

Full Keyboard also sends the Latin letter of the place for a key that shows punctuation there
(AZERTY's "," on KeyM, Dvorak's "'" on KeyQ), so Ctrl with one of those reaches the desktop as
Ctrl with that letter. The two can't be told apart here; letters, the shortcuts that matter,
win.
"""

# Full Keyboard's modifiers that make a key a shortcut: Ctrl, Alt and Super (left and right).
# Shift alone and AltGr type characters, which come as text instead.
SHORTCUT_MODIFIERS = frozenset({29, 97, 56, 125, 126})

# The letters' keys on a US layout (linux/input-event-codes.h): code -> letter.
US_LETTERS = {first + i: letter for row, first in (("qwertyuiop", 16), ("asdfghjkl", 30), ("zxcvbnm", 44))
              for i, letter in enumerate(row)}


def shortcut_code(code, held, keys):
    """The key to press for Full Keyboard's code, with the codes in held down: the key that
    types its letter on keys (character -> (code, modifiers)) when held has a shortcut
    modifier, else code."""
    letter = US_LETTERS.get(code)
    if letter is None or not SHORTCUT_MODIFIERS.intersection(held):
        return code
    stroke = keys.get(letter)
    return stroke[0] if stroke and not stroke[1] else code
