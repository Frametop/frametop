#!/usr/bin/env python3
"""input/kb_revive.py: a quit keyboard comes back when it's asked for, offline (no systemd).

  python3 input/test/kb-revive-test.py
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from kb_revive import AGAIN, START, UNIT, WAIT, KeyboardRevive  # noqa: E402


def revive():
    starts = []
    return KeyboardRevive(start=lambda: starts.append(1)), starts


def test_ask_starts_the_unit_and_its_hello_opens_once():
    r, starts = revive()
    assert r.ask(100.0) is True and starts == [1]
    assert r.take(103.0) is True    # the keyboard it started says hello: it opens
    assert r.take(104.0) is False   # ...once: a later keyboard (a restart) isn't opened


def test_a_keyboard_nobody_asked_for_stays_closed():
    r, starts = revive()
    assert r.take(5.0) is False and starts == []   # SteamVR's start, a restart: closed, as before


def test_presses_close_together_start_it_once():
    r, starts = revive()
    assert r.ask(100.0) is True
    assert r.ask(100.0 + AGAIN - 0.1) is False and starts == [1]
    assert r.take(100.0 + AGAIN) is True           # ...and still open it
    assert r.ask(100.0 + AGAIN + 0.1) is True and starts == [1, 1]   # no keyboard came: a later press tries again


def test_a_hello_long_after_does_not_open():
    r, _ = revive()
    r.ask(100.0)
    assert r.take(100.0 + WAIT) is False           # SteamVR started an hour later: not this press's keyboard
    r.ask(200.0)
    r.ask(203.0)                                   # a second press keeps waiting from then
    assert r.take(203.0 + WAIT - 0.1) is True


def test_start_command_names_the_unit_and_clears_its_start_limit():
    assert UNIT == "frametop-keyboard.service"
    assert START.index("reset-failed") < START.index("start --no-block") and START.count('"$0"') == 2


if __name__ == "__main__":
    for name, test in sorted(globals().items()):
        if name.startswith("test_"):
            test()
            print("ok   ", name)
    print("all passed")
