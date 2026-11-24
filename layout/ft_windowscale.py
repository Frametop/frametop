"""The scale a PC's app window is drawn at on its panel (docs/remote-displays.md, "Scale"): which
one a window starts at, and the panel's size that takes it. Pure: ft_windows talks to the PC.

Only a PC that says "scaling" in its window list takes one (Hyprland). A window panel there keeps
its scale in "scale", its app in "pc_app", and, where the size it was given doesn't take its scale,
that size in "asked_size". The PC's "windows" entry keeps each app's last scale in "scales".
"""
import math

import ft_hostmonitors

STEPS = (1.0, 1.25, 1.5, 1.6, 1.75, 2.0, 2.25, 2.5)   # (screens/appscale.h's kPercents)
LIMITS = (0.5, 4.0)   # what a PC takes
NUDGE = 16            # how far a side may move to take a scale, in pixels (appscale.h's kNudge)
REMEMBERED = 100      # apps a PC's entry keeps


def valid(scale):
    """A scale a PC takes, or None for anything else (a layout file edited by hand, say)."""
    if isinstance(scale, bool) or not isinstance(scale, (int, float)) or not math.isfinite(scale):
        return None
    return float(scale) if LIMITS[0] <= scale <= LIMITS[1] else None


def _side(pixels, scale, low, high):
    """The even number of pixels nearest `pixels` that is a whole number at `scale`, checked as
    Hyprland checks it, or None."""
    for off in range(0, NUDGE + 1):
        for v in (pixels - off, pixels + off):
            if v % 2 == 0 and low <= v <= high and v / scale == round(v / scale):
                return v
    return None


def fitted(size, scale, low, high):
    """(size, scale): the size nearest `size` that the scale fits (Hyprland takes one only if the
    output's size divided by it is whole), and the scale in Hyprland's own steps of a 120th. A scale
    no size nearby fits becomes the nearest step that one does. A size past the limits (one opened
    bigger, or smaller) stays past them."""
    low, high = [min(lo, int(v)) for lo, v in zip(low, size)], [max(hi, int(v)) for hi, v in zip(high, size)]
    exact = round(scale * 120) / 120
    for s in [exact] + sorted(STEPS, key=lambda step: abs(step - scale)):
        w, h = (_side(int(v), s, lo, hi) for v, lo, hi in zip(size, low, high))
        if w and h:
            return (w, h), s
    return tuple(size), 1.0


def start(host, app):
    """The scale a window of `app` starts at: the one it was left at on that PC, else the one picked
    for the PC's display (a connected one's first), else 100%."""
    windows = host.get("windows")
    scales = windows.get("scales") if isinstance(windows, dict) else None
    kept = valid(scales.get(app)) if app and isinstance(scales, dict) else None
    if kept:
        return kept
    picked = [d for d in host.get("displays", []) if ft_hostmonitors.monitor_of(d) is not None and ft_hostmonitors.scale_of(d)]
    picked.sort(key=lambda d: bool(d.get("off")))
    return ft_hostmonitors.scale_of(picked[0]) if picked else 1.0


def keep(host, app, scale):
    """The app's scale on that PC, for its next window."""
    if not app or not isinstance(host.get("windows"), dict):
        return
    scales = host["windows"].get("scales")
    scales = dict(scales) if isinstance(scales, dict) else {}
    scales.pop(app, None)   # (the last one set goes last: the oldest goes first)
    scales[app] = scale
    host["windows"]["scales"] = dict(list(scales.items())[-REMEMBERED:])


def app_of(window):
    """A listed window's app, as desktopless mode names it: its desktop app, else its class."""
    app = window.get("app") or window.get("class")
    return app if isinstance(app, str) else ""


def shape(d, size, scale, low, high):
    """Panel d's size and scale for a size it's given: the nearest that fit, and the size given
    kept, so later scales nudge from it and not from each other."""
    (w, h), scale = fitted(size, scale, low, high)
    d["size"], d["scale"] = [w, h], round(scale, 6)
    if (w, h) != tuple(size):
        d["asked_size"] = list(size)
    else:
        d.pop("asked_size", None)


def told(n, d):
    """What ft-screens hears of panel n's scale, for its menu: 0 for none (its PC takes none), and the
    size that takes it, which may be a few pixels from the one its corner was dragged to."""
    scale = valid(d.get("scale"))
    if not scale:
        return f"remote {n} scale 0"
    return f"remote {n} scale {scale:g}" + (" {}x{}".format(*d["size"]) if d.get("size") else "")


def opening(host, answer, app, size, low, high):
    """(size, fields) for a window opening on that PC: its first size and its panel's fields (none
    where the PC, by its window list `answer`, takes no scale)."""
    if answer.get("scaling") is not True:
        return size, {}
    d = {"pc_app": app}
    shape(d, size, start(host, app), low, high)
    return tuple(d.pop("size")), d
