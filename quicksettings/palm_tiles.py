"""Frametop Settings' Quick Settings page: what its tiles can run, and changing the list
(conf/ft_palm_menu.py's entries). Plain Python (no Qt): ft_quick_settings' Backend calls these.
Tests: quicksettings/test/ (test_palm_page.py has the page itself).
"""
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
for folder in ("conf", "commands"):
    sys.path.insert(0, os.path.join(REPO, folder))
import ft_commands  # noqa: E402  (your commands)
import ft_palm_menu as palm  # noqa: E402  (the file, its pages and the native actions)
import ft_store  # noqa: E402
from ft_input_actions import ACTION_LABELS  # noqa: E402

LAYOUT_PATH = os.path.expanduser("~/.config/frametop-layout.json")   # its profiles
RUN, PROFILE = "run:", "profile:"   # the input relay's prefixes (input/input-relay.py)
NATIVE = dict(palm.CATALOGUE)


def profiles(path=None):
    """The saved profiles' names (docs/profiles.md)."""
    data = ft_store.load_json(path or LAYOUT_PATH, {}, ft_store.is_object)
    layouts = data.get("layouts")
    return sorted(layouts, key=str.casefold) if isinstance(layouts, dict) else []


def choices(commands=None, layouts=None):
    """Every action a tile can run, in the picker's order: [{value, text}]. Your commands first."""
    commands = [c["name"] for c in ft_commands.load()] if commands is None else commands
    layouts = profiles() if layouts is None else layouts
    return ([{"value": RUN + n, "text": f"Run command {n}"} for n in commands]
            + [{"value": a, "text": ACTION_LABELS[a]} for a in NATIVE]
            + [{"value": PROFILE + n, "text": f"Open profile {n}"} for n in layouts])


def with_missing(listed, entries):
    """The picker's choices with any action a tile has that isn't there now (a command or profile
    that's gone, or one put in the file by hand), so its picker still shows what it is."""
    known = {c["value"] for c in listed}
    others = sorted({e["action"] for e in entries} - known)
    return listed + [{"value": a, "text": f"{a} (not available)" if a.startswith((RUN, PROFILE))
                      else ACTION_LABELS.get(a, a)} for a in others]


def suggested(action, page="menu"):
    """The words a tile that runs `action` starts with."""
    if action == "screens_toggle" and page == "quick":
        return "Screens"
    if action.startswith(RUN):
        name = action[len(RUN):]
        return palm.clean(ft_commands.spoken(name).title() or name, palm.LABEL_MAX)
    if action.startswith(PROFILE):
        return palm.clean(action[len(PROFILE):], palm.LABEL_MAX)
    return NATIVE.get(action, action)


def full(entries, page):
    """The page shows all it has room for."""
    return len([e for e in palm.of(entries, page) if e["on"]]) >= palm.MAX[page]


def _at(entries, page, index):
    on_page = palm.of(entries, page)
    return on_page[index] if 0 <= index < len(on_page) else None


def set_shown(entries, page, index, on):
    e = _at(entries, page, index)
    if e is not None and (not on or e["on"] or not full(entries, page)):
        e["on"] = bool(on)
    return entries


def set_label(entries, page, index, text):
    """Its words; none typed, the ones its action starts with."""
    e = _at(entries, page, index)
    if e is not None:
        e["label"] = palm.clean(text, palm.LABEL_MAX) or suggested(e["action"], page)
    return entries


def set_action(entries, page, index, action):
    """What it runs. Words nobody changed follow it."""
    e = _at(entries, page, index)
    action = palm.clean(action)
    if e is not None and action and "\t" not in action:
        if e["label"] == suggested(e["action"], page):
            e["label"] = suggested(action, page)
        e["action"] = action
    return entries


def add(entries, page, action):
    """A new tile at the page's end, shown if there's room."""
    action = palm.clean(action)
    if page in ("menu", "quick") and action:
        entries.append(palm.Entry(page, not full(entries, page), action, suggested(action, page)))
    return entries


def remove(entries, page, index):
    """Take a tile out (the eye row only hides)."""
    e = _at(entries, page, index)
    return entries if e is None or page == "eyes" else [x for x in entries if x is not e]


def move(entries, page, index, by):
    """A tile one place up (-1) or down (1) its page."""
    on_page = palm.of(entries, page)
    to = index + by
    if 0 <= index < len(on_page) and 0 <= to < len(on_page):
        on_page[index], on_page[to] = on_page[to], on_page[index]
        entries = [e for e in entries if e["page"] != page] + on_page
    return entries
