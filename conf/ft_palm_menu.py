"""ft_palm_menu: the palm menu's tiles, ~/.config/frametop-palm.txt.

Frametop Settings' Quick Settings page (quicksettings/) writes it, ft-screens draws it
(screens/palmtiles.h, the same parser in C++) and the input relay runs only what's in it ("palm
ACTION" on its control socket). A line each, tabs between, in the order the card shows them:

  PAGE  1|0  ACTION  LABEL

PAGE is menu (the card's first page), quick (its Quick Settings page) or eyes (the wide row under the
tiles while gaze mode is on: one entry). 1 shows it, 0 keeps it here hidden. ACTION is the input
relay's: one of Frametop's own (CATALOGUE), "profile:NAME", or "run:NAME", a command of yours
(commands/ft_commands.py); any other the relay knows works too, written here by hand. LABEL is the
tile's words (LABEL_MAX characters). Blank lines and lines starting with # are left out, and a save from
Frametop Settings drops them. A page shows its first MAX entries that are on.

With no file the card is DEFAULT, whose Quick Settings page shows only those QUICK_SETTINGS in
frametop.conf names, if it's set (how that page was picked before this file).
A screens_toggle tile labelled "Hide Screens" or "Show Screens" says which it would do.
Standard library only: the relay runs it with the host's python3.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ft_conf  # noqa: E402  (QUICK_SETTINGS, and the list grammar)
import ft_store  # noqa: E402  (the file read strictly, locked and written whole)

PATH = os.path.expanduser("~/.config/frametop-palm.txt")
PAGES = ("menu", "quick", "eyes")
MAX = {"menu": 6, "quick": 6, "eyes": 1}   # screens/quickaccess.h's kTiles
LABEL_MAX = 40
QUICK_SETTINGS_KEY = "QUICK_SETTINGS"
# What the card shows with no file (screens/palmtiles.h's kDefault: quicksettings/test checks it).
DEFAULT = (
    "menu\t1\tsteam_menu\tSteam Menu\n"
    "menu\t1\tscreens_toggle\tHide Screens\n"
    "menu\t1\tlayout_reset\tReset Layout…\n"
    "menu\t1\tspin_prev\tSpin Left\n"
    "menu\t1\tspin_next\tSpin Right\n"
    "menu\t1\tdock_all\tDock Windows\n"
    "quick\t1\tscreens_toggle\tScreens\n"
    "quick\t1\tkeyboard_toggle\tKeyboard\n"
    "quick\t1\tdesktopless_toggle\tDesktopless\n"
    "quick\t1\thands_toggle\tHand Tracking\n"
    "quick\t1\tpause_toggle\tGame Pause\n"
    "quick\t1\tdictate\tDictation\n"
    "eyes\t1\tgaze_quickcal\tQuick Eye Calibrate\n"
)
# Frametop's own actions a tile can run, and the words a new one starts with, in the picker's order.
CATALOGUE = (
    ("steam_menu", "Steam Menu"), ("screens_toggle", "Hide Screens"), ("layout_reset", "Reset Layout…"),
    ("spin_prev", "Spin Left"), ("spin_next", "Spin Right"), ("dock_all", "Dock Windows"),
    ("keyboard_toggle", "Keyboard"), ("desktopless_toggle", "Desktopless"), ("hands_toggle", "Hand Tracking"),
    ("pause_toggle", "Game Pause"), ("dictate", "Dictation"), ("gaze_quickcal", "Quick Eye Calibrate"),
    ("gaze_toggle", "Gaze Pointer"), ("pointer_toggle", "Pointer"), ("recenter", "Recenter Pointer"),
    ("follow_toggle", "Head Follow"), ("sens_up", "Faster Pointer"), ("sens_down", "Slower Pointer"),
    ("dashboard", "SteamVR Dashboard"), ("frunner", "Search"), ("os_menu", "OS Menu"),
    ("keys_list", "Shortcut List"), ("float_toggle", "Float Window"), ("carry_toggle", "Carry Panel"),
    ("anchor_toggle", "Anchor Panel"), ("close_app", "Close App"), ("release_keys", "Release Keys"),
    ("tile_left", "Tile Left"), ("tile_right", "Tile Right"), ("tile_up", "Tile Up"), ("tile_down", "Tile Down"),
    ("tile_wider", "Tile Wider"), ("tile_narrower", "Tile Narrower"), ("tile_taller", "Tile Taller"),
    ("tile_shorter", "Tile Shorter"), ("tile_nudge_up", "Nudge Tile Up"), ("tile_nudge_down", "Nudge Tile Down"),
)
# The relay's other actions make no sense from a tap on the palm: clicks and scrolls where the
# pointer is, what's held down, and the two that pass a button through or do nothing.
LEFT_OUT = frozenset({"left", "right", "middle", "back", "scroll_up", "scroll_down", "gaze_precision", "gaze_drag",
                      "gaze_left", "gaze_right", "dictate_hold", "key", "none"})


SPACE = " \t\r\n"   # what a field is trimmed of (screens/palmtiles.h's Trim)


class Entry(dict):
    """One tile: {page, on, action, label}."""

    def __init__(self, page, on, action, label):
        super().__init__(page=page, on=bool(on), action=action, label=label)


def clean(text, limit=None):
    """text on one line, with no tabs, control characters or spaces at its ends."""
    text = "".join(c if c.isprintable() else " " for c in str(text))
    return text[:limit].strip() if limit else text.strip()


def parse_line(line):
    """A line's Entry, None for a blank or a comment, or ValueError (screens/palmtiles.h's ParseLine)."""
    if not line.strip(SPACE) or line.strip(SPACE).startswith("#"):
        return None
    parts = [p.strip(SPACE) for p in line.split("\t")]
    if len(parts) != 4:
        raise ValueError("not PAGE, 1 or 0, ACTION and LABEL with tabs between")
    page, on, action, label = parts
    if page not in PAGES:
        raise ValueError(f"no page {page!r}")
    if on not in ("0", "1"):
        raise ValueError(f"{on!r} isn't 1 or 0")
    if not action or not label:
        raise ValueError("no action or no label")
    return Entry(page, on == "1", action, label[:LABEL_MAX].strip(SPACE))


def parse(text):
    """(the entries, why each bad line was left out). Only the first eyes entry counts; with none,
    the default one is there (screens/palmtiles.h does the same)."""
    entries, problems = [], []
    for n, line in enumerate(text.split("\n"), 1):
        try:
            e = parse_line(line)
        except ValueError as why:
            problems.append(f"line {n}: {why}")
            continue
        if e and not (e["page"] == "eyes" and of(entries, "eyes")):
            entries.append(e)
    if not of(entries, "eyes"):
        entries += of(parse(DEFAULT)[0], "eyes")
    return entries, problems


def of(entries, page):
    """A page's entries."""
    return [e for e in entries if e["page"] == page]


def defaults(conf=None):
    """The card with no file: DEFAULT, its Quick Settings page as frametop.conf (conf, read) picks it."""
    entries = parse(DEFAULT)[0]
    value = (conf or {}).get(QUICK_SETTINGS_KEY)
    if value is not None:
        picked = set(ft_conf.items(value))
        for e in of(entries, "quick"):
            e["on"] = e["action"] in picked
    return entries


def _read(path, errors="replace"):
    """The file's text, None with no file."""
    try:
        with open(path, encoding="utf-8", errors=errors, newline="") as f:
            return f.read()
    except FileNotFoundError:
        return None


def load(path=None, conf=None):
    """The entries as the card shows them: bad lines left out; with no file (or one that can't be
    opened) the defaults. conf: frametop.conf's path."""
    try:
        text = _read(path or PATH)
    except OSError:
        text = None
    return defaults(ft_conf.read(conf)) if text is None else parse(text)[0]


def text_of(entries):
    """The file's text for these entries, page by page."""
    return "# The palm menu's tiles (Frametop Settings, Quick Settings): PAGE, 1 shown or 0 hidden, ACTION, LABEL\n" + \
        "".join(f"{e['page']}\t{int(e['on'])}\t{e['action']}\t{e['label']}\n" for p in PAGES for e in of(entries, p))


def update(fn, path=None, conf=None):
    """Change the entries under the file's lock: fn gets them (the defaults with no file) and returns
    the new ones, which are written. A file with lines that don't parse is never saved over, as that
    would lose them: ft_store.StoreError."""
    path = path or PATH
    with ft_store.locked(path):
        try:
            text = _read(path, "strict")
        except (OSError, ValueError) as e:
            raise ft_store.refuse(path, f"it doesn't read ({e})") from None
        if text is None:
            entries = defaults(ft_conf.read(conf))
        else:
            entries, problems = parse(text)
            if problems:
                raise ft_store.refuse(path, problems[0])
        new = fn(entries)
        ft_store.write_text(path, text_of(new))
    return new


def restore(path=None):
    """DEFAULT written back, whatever the file had. A copy of one that doesn't parse is kept; with no
    copy made, ft_store.StoreError and the file stays."""
    path = path or PATH
    with ft_store.locked(path):
        try:
            bad = bool(parse(_read(path, "strict") or "")[1])
        except (OSError, ValueError):
            bad = True
        if bad and not ft_store.backup(path):
            raise ft_store.refuse(path, "no copy of it could be made")
        ft_store.write_text(path, text_of(parse(DEFAULT)[0]))


def shown(entries, page):
    """What a page shows, in order."""
    return [e for e in of(entries, page) if e["on"]][:MAX[page]]


def actions(entries):
    """Every action the card can run now: what the relay takes from it."""
    return frozenset(e["action"] for p in PAGES for e in shown(entries, p))


def drop_action(action, path=None):
    """Take out the tiles that ran `action` (a command that's gone); the eye row, always there, runs its
    default again, shown or hidden as it was. How many went; with no file, none (the defaults run no
    commands), and the file isn't written."""
    path = path or PATH
    if not any(e["action"] == action for e in load(path)):
        return 0
    gone = []

    def change(entries):
        gone.extend(e for e in entries if e["action"] == action)
        for e in of(gone, "eyes"):
            e.update(of(parse(DEFAULT)[0], "eyes")[0], on=e["on"])
        return [e for e in entries if e["action"] != action]
    update(change, path)
    return len(gone)
