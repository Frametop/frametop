"""Spotwatch's search: which apps and remote displays match what's typed, best first
(spotwatch/ft-spotwatch, docs/spotwatch.md). Plain Python, no GLib, so its test runs anywhere.

A query matches an item's name best at its start, then at the start of one of its words, then
by its initials ("vsc": Visual Studio Code), then anywhere in it, then as letters in order
(fuzzy). Its generic name ("Web Browser"), keywords, and program name count too, a little
less, and never fuzzily. Several words must each match. What you open often, and opened in
the last day, comes higher; an app that's running a little higher still.
"""
import math
import re
import time
import unicodedata

SECONDARY = 0.6  # a generic name, keyword, or program name counts this much of a name match
DAY = 86400
WORDS = re.compile(r"[\s\-_.:/()]+")


def fold(text):
    """Lower case, without accents, so "é" finds "e"."""
    text = unicodedata.normalize("NFKD", text or "")
    return "".join(c for c in text if not unicodedata.combining(c)).lower().strip()


class Item:
    """Something to search: an app or a remote display."""

    def __init__(self, kind, key, title, generic="", keywords=(), exe="", data=None):
        self.kind = kind          # "app" or "remote"
        self.key = key            # its usage key: the desktop file name, or "remote:<id>"
        self.title = title
        self.generic = generic
        self.keywords = list(keywords)
        self.exe = exe
        self.data = data          # the caller's own (an app's info, a display's numbers)
        self.running = False


def match(query, text, fuzzy=True):
    """How well one folded query word matches a text (None: not at all)."""
    t = fold(text)
    if not t or not query:
        return None
    if t == query:
        return 120.0
    if t.startswith(query):
        return 100.0 - min(len(t) - len(query), 20) * 0.5
    words = [w for w in WORDS.split(t) if w]
    if any(w.startswith(query) for w in words):
        return 80.0
    if len(query) >= 2 and "".join(w[0] for w in words).startswith(query):
        return 75.0
    if query in t:
        return 60.0
    if not fuzzy:
        return None
    at, gaps = -1, 0
    for ch in query:
        i = t.find(ch, at + 1)
        if i < 0:
            return None
        gaps += i - at - 1
        at = i
    return max(5.0, 40.0 - 2.0 * gaps)


def match_item(query, item):
    """The best match of one query word against an item's fields."""
    best = match(query, item.title)
    for text in [item.generic, item.exe] + item.keywords:
        s = match(query, text, fuzzy=False)
        if s is not None and (best is None or s * SECONDARY > best):
            best = s * SECONDARY
    return best


def score(item, query, usage=None, now=None):
    """An item's score for a query (folded), or None when it doesn't match."""
    s = match_item(query, item)
    if s is None and " " in query:
        parts = [match_item(w, item) for w in query.split()]
        if any(p is None for p in parts):
            return None
        s = 0.9 * min(parts)
    if s is None:
        return None
    u = (usage or {}).get(item.key)
    if u:
        s += min(25.0, 7.0 * math.log2(1 + max(0, u.get("count", 0))))
        if (now or time.time()) - u.get("last", 0) < DAY:
            s += 8.0
    if item.running:
        s += 4.0
    return s


def search(items, query, usage=None, now=None, limit=50):
    """The items matching a query, best first (ties by title). An empty query matches nothing."""
    q = " ".join(fold(query).split())
    if not q:
        return []
    scored = [(score(it, q, usage, now), it) for it in items]
    scored = [(s, it) for s, it in scored if s is not None]
    scored.sort(key=lambda p: (-p[0], fold(p[1].title)))
    return [it for _, it in scored[:limit]]


def use(usage, key, now=None):
    """Count one use of an item (in place); returns usage."""
    u = usage.setdefault(key, {"count": 0, "last": 0})
    u["count"] = u.get("count", 0) + 1
    u["last"] = int(now or time.time())
    return usage


# Typed text on a US layout, for when libxkbcommon isn't there: evdev code -> (plain, shifted).
US_TEXT = {57: (" ", " ")}
for _row, _shifted, _first in (("1234567890-=", "!@#$%^&*()_+", 2), ("qwertyuiop[]", "QWERTYUIOP{}", 16),
                               ("asdfghjkl;'`", 'ASDFGHJKL:"~', 30), ("\\zxcvbnm,./", "|ZXCVBNM<>?", 43)):
    for _i, (_plain, _shift) in enumerate(zip(_row, _shifted)):
        US_TEXT[_first + _i] = (_plain, _shift)


def drop_word(text):
    """Text without its last word (Ctrl+Backspace)."""
    return re.sub(r"\S*\s*$", "", text.rstrip()) if text.strip() else ""
