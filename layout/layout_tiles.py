"""A profile's dashboard tiles (docs/profiles.md): which of the desktop's screens and remote
displays are anchored to the SteamVR dashboard, and where in the grid (docs/design.md
"Dashboard tiles"). Floating windows aren't kept here: ft-screens remembers each app's own
(`floatapp`), and a profile's apps open with new panel numbers anyway.

ft-screens' `tiles` lists every anchored panel as "N STATE", by column and row; `tiles set` takes
them back the same way (and `tiles add` the rest of one too long for a command, after them). A profile keeps them by what each panel is ("screen:N", "remote:ID"),
since a remote display's panel number can differ next time. Pure: the caller asks ft-screens
(`ask(text) -> reply`, raising on an error) and says which panel is which.
"""

MAX_COMMAND = 480   # ft-screens reads a command of up to 511 bytes (compositor.c, vr.cpp)


def parse(reply):
    """`tiles`' "ok N STATE; N STATE..." as [(N, STATE)]."""
    if not reply.startswith("ok"):
        raise ValueError(reply)
    out = []
    for part in reply[2:].split(";"):
        n, _, state = part.strip().partition(" ")
        if n.isdigit() and state:
            out.append((int(n), state.strip()))
    return out


def capture(ask, panels):
    """[[KEY, STATE]] for the anchored panels among `panels` ({number: key}), in ft-screens'
    order (by column and row: the order `tiles set` takes them back in)."""
    return [[panels[n], state] for n, state in parse(ask("tiles")) if n in panels]


def commands(saved, panels):
    """The commands that put `saved` (capture's) back: every panel in `panels` ({number: key})
    the profile didn't anchor goes off first, then the saved ones in order. Each command fits
    ft-screens' buffer: the first is `tiles set`, any after it `tiles add` (the columns go on
    after the ones before). Items too long for any command are left out."""
    numbers = {key: n for n, key in panels.items()}
    kept = {key for key, _ in saved}
    items = [f"{n} off" for n, key in sorted(panels.items()) if key not in kept]
    items += [f"{numbers[key]} {state}" for key, state in saved if key in numbers]
    out, line = [], ""
    for item in items:
        if len("tiles set " + item) > MAX_COMMAND:
            continue
        if line and len(line) + 2 + len(item) > MAX_COMMAND:
            out.append(line)
            line = ""
        line = f"{line}; {item}" if line else f"tiles {'add' if out else 'set'} {item}"
    if line:
        out.append(line)
    return out


def memory_key(key):
    """A key as ft-screens' tile memory keeps it (screens/tilememory.h's Clean): one word, so an
    app named "Steam Big Picture" is "Steam_Big_Picture" there, and in the command that names it."""
    return "".join("_" if c <= " " or c == "\x7f" else c for c in key)[:200]


def remember_commands(saved, keys):
    """The commands that tell ft-screens' tile memory a profile's tiles (`keys`: {a profile's key:
    the memory's}), for a display that connects later: first one alone, a profile's put back now."""
    lines = [f"tiles remember {memory_key(keys[key])} {state}" for key, state in saved if key in keys]
    return ["tiles remember"] + [line for line in lines if len(line) <= MAX_COMMAND]


def restore(ask, saved, panels, keys=None):
    """Puts a profile's tiles back. Returns ft-screens' replies to the sets. (An ft-screens older
    than `tiles remember` refuses that one: the tiles connected now still go back. A tile it
    refuses doesn't stop the ones after it. One it doesn't answer ends what's told to its memory;
    the sets are still tried, and say so themselves if it's gone.)"""
    first, *tiles = remember_commands(saved, keys or {})
    try:
        if _refused(ask, first):
            tiles = []
        for command in tiles:
            _refused(ask, command)
    except RuntimeError:   # (no answer, raised on by _refused)
        pass
    return [ask(c) for c in commands(saved, panels)]


def _refused(ask, command):
    """Asks ft-screens one thing; whether it refused it. No answer at all (ask's own kind of
    RuntimeError, ft_layout's NoAnswer) isn't a refusal: it's raised on, so restore doesn't wait
    out every tile of an ft-screens that hangs."""
    try:
        ask(command)
    except RuntimeError as e:
        if type(e) is not RuntimeError:
            raise
        return True
    return False
