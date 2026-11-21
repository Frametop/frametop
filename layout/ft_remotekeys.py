"""A Mac display's keyboard settings, kb_layout and keymap (ft-layout remote set and remote host),
which ft-appless reads from the layout and turns into the Mac's key codes (stream/apple_hp.h).

kb_layout is the layout the keys are read with: qwerty (the default), dvorak, dvorak-qwerty-cmd
(Command combinations as on QWERTY) or colemak; the Mac gets the characters they type in it. keymap swaps keys before they go: FROM:TO pairs
of key names (canonical_key_name), TO "none" to drop a key. A display's own settings win over
its host's (effective_keyboard)."""
import re

KB_LAYOUT_ALIASES = {
    "qwerty": "qwerty", "us": "qwerty", "default": "qwerty",
    "dvorak": "dvorak",
    "dvorak-qwerty-cmd": "dvorak-qwerty-cmd", "dvorak-cmd": "dvorak-qwerty-cmd",
    "colemak": "colemak",
}
KEY_ALIASES = {
    "esc": "esc", "escape": "esc",
    "minus": "minus", "hyphen": "minus",
    "equal": "equal", "equals": "equal",
    "backspace": "backspace", "bksp": "backspace",
    "tab": "tab",
    "leftbrace": "leftbrace", "lbrace": "leftbrace", "leftbracket": "leftbrace", "lbracket": "leftbrace",
    "rightbrace": "rightbrace", "rbrace": "rightbrace", "rightbracket": "rightbrace", "rbracket": "rightbrace",
    "enter": "enter", "return": "enter",
    "leftctrl": "leftctrl", "lctrl": "leftctrl", "ctrl": "leftctrl", "control": "leftctrl", "leftcontrol": "leftctrl",
    "semicolon": "semicolon",
    "apostrophe": "apostrophe", "quote": "apostrophe",
    "grave": "grave", "backtick": "grave", "tilde": "grave",
    "leftshift": "leftshift", "lshift": "leftshift", "shift": "leftshift",
    "backslash": "backslash",
    "comma": "comma",
    "dot": "dot", "period": "dot",
    "slash": "slash",
    "rightshift": "rightshift", "rshift": "rightshift",
    "kpasterisk": "kpasterisk", "kpmultiply": "kpasterisk",
    "leftalt": "leftalt", "lalt": "leftalt", "alt": "leftalt", "opt": "leftalt", "option": "leftalt",
    "leftopt": "leftalt", "leftoption": "leftalt",
    "space": "space", "spacebar": "space",
    "capslock": "capslock", "caps": "capslock",
    "numlock": "numlock",
    "kp0": "kp0", "kp1": "kp1", "kp2": "kp2", "kp3": "kp3", "kp4": "kp4",
    "kp5": "kp5", "kp6": "kp6", "kp7": "kp7", "kp8": "kp8", "kp9": "kp9",
    "kpminus": "kpminus", "kpsubtract": "kpminus",
    "kpplus": "kpplus", "kpadd": "kpplus",
    "kpdot": "kpdot", "kpdecimal": "kpdot",
    "102nd": "102nd", "iso102nd": "102nd",
    "kpenter": "kpenter",
    "rightctrl": "rightctrl", "rctrl": "rightctrl", "rightcontrol": "rightctrl",
    "kpslash": "kpslash", "kpdivide": "kpslash",
    "rightalt": "rightalt", "ralt": "rightalt", "altgr": "rightalt", "ropt": "rightalt", "roption": "rightalt",
    "rightopt": "rightalt", "rightoption": "rightalt",
    "home": "home", "up": "up", "pageup": "pageup", "pgup": "pageup",
    "left": "left", "right": "right", "end": "end", "down": "down", "pagedown": "pagedown", "pgdn": "pagedown",
    "insert": "insert", "ins": "insert",
    "delete": "delete", "del": "delete", "forwarddelete": "delete",
    "kpequal": "kpequal",
    "leftmeta": "leftmeta", "lmeta": "leftmeta", "meta": "leftmeta", "super": "leftmeta",
    "leftsuper": "leftmeta", "lsuper": "leftmeta", "cmd": "leftmeta", "command": "leftmeta",
    "leftcmd": "leftmeta", "leftcommand": "leftmeta", "lcmd": "leftmeta", "win": "leftmeta", "lwin": "leftmeta",
    "rightmeta": "rightmeta", "rmeta": "rightmeta", "rightsuper": "rightmeta", "rsuper": "rightmeta",
    "rcmd": "rightmeta", "rcommand": "rightmeta", "rightcmd": "rightmeta", "rightcommand": "rightmeta",
    "rwin": "rightmeta",
    "globe": "globe", "fn": "globe", "function": "globe",
}


def canonical_key_name(raw, allow_none=False):
    """Canonical physical key name for ft-appless keymap entries (stream/apple_hp.h)."""
    low = raw.strip().lower()
    if not low:
        raise ValueError("empty key name")
    for prefix in ("code:", "evdev:"):
        if low.startswith(prefix):
            tail = low[len(prefix):]
            if tail.isdigit() and 1 <= int(tail) <= 255 and int(tail) != 194:
                return f"code:{int(tail)}"
            raise ValueError(f"bad key code {raw!r}")
    s = re.sub(r"[-_]", "", low) if len(low) > 1 else low
    if allow_none and s in ("none", "disabled", "off"):
        return "none"
    if len(s) == 1 and ("a" <= s <= "z" or "0" <= s <= "9"):
        return s
    if len(s) in (2, 3) and s[0] == "f" and s[1:].isdigit() and 1 <= int(s[1:]) <= 20:
        return f"f{int(s[1:])}"
    if s in KEY_ALIASES:
        return KEY_ALIASES[s]
    raise ValueError(f"unknown key {raw!r}")


def apply_keyboard_kv(target, key, value):
    """Applies kb_layout=... or keymap=... onto a host or display dict."""
    if key == "kb_layout":
        norm = value.strip().lower().replace("_", "-")
        if not norm or norm == "none":
            target.pop("kb_layout", None)
            return True
        if norm in KB_LAYOUT_ALIASES:
            target["kb_layout"] = KB_LAYOUT_ALIASES[norm]
            return True
        return False
    if key == "keymap":
        norm = value.strip()
        if not norm or norm.lower() == "none":
            target.pop("keymap", None)
            return True
        out = {}
        try:
            for item in norm.split(","):
                src, sep, dst = item.partition(":")
                if not sep:
                    return False
                out[canonical_key_name(src, False)] = canonical_key_name(dst, True)
        except ValueError:
            return False
        if not out:
            return False
        target["keymap"] = out
        return True
    return False


def effective_keyboard(host, d):
    """Merged (kb_layout, keymap) for a remote display from its host and own entry."""
    kb = ""
    keymap = {}
    for obj in (host, d):
        if isinstance(obj.get("kb_layout"), str) and obj["kb_layout"]:
            kb = obj["kb_layout"]
        if isinstance(obj.get("keymap"), dict):
            for src, dst in obj["keymap"].items():
                if isinstance(src, str) and isinstance(dst, str):
                    if src == dst:
                        keymap.pop(src, None)
                    else:
                        keymap[src] = dst
    return kb, keymap
