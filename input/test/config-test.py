#!/usr/bin/env python3
"""Offline test of the input relay's frametop.conf reading: a POINTER_* value that isn't a
number warns and keeps its default instead of stopping the relay at start, and HANDS_NEAR reads
the same here as in handsmode (Input Settings) for every way a line can say 0. ft-screens and the
pointer helper read through conf/ft_conf.h, checked against the same lines in conf/test/conf-test.py.
It loads input-relay.py without running it: no devices, sockets, or grabs.

  input/test/config-test.py [RELAY]   (default: input/input-relay.py next to this folder)
"""
import importlib.util
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
relay_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "input-relay.py")
relay = importlib.util.module_from_spec(importlib.util.spec_from_loader("relay", loader=None))
relay.__file__ = os.path.abspath(relay_path)
sys.path.insert(0, os.path.dirname(os.path.abspath(relay_path)))  # its game_pause and vrws
with open(relay_path) as f:
    exec(compile(f.read(), relay_path, "exec"), relay.__dict__)
sys.path.insert(0, os.path.join(HERE, "..", "..", "hands"))
import handsmode  # noqa: E402

failures = []
DIR = tempfile.mkdtemp()


def check(label, got, want):
    ok = got == want
    print(("ok    " if ok else "FAIL  ") + label + ("" if ok else f": got {got!r}, want {want!r}"), flush=True)
    if not ok:
        failures.append(label)


def conf(text):
    path = os.path.join(DIR, "frametop.conf")
    with open(path, "w") as f:
        f.write(text)
    return relay.read_config(path)


check("no file: pointer mode off, the defaults", relay.pointer_settings(relay.read_config(os.path.join(DIR, "none"))),
      (False, 0.03, 30.0, 40))
check("set: read", relay.pointer_settings(conf("POINTER=1\nPOINTER_SENSITIVITY=0.05\nPOINTER_IDLE='12'\n"
                                               "POINTER_WAKE_COUNTS=\"25\"   # quoted\n")), (True, 0.05, 12.0, 25))
check("garbage: the defaults, no exception",
      relay.pointer_settings(conf("POINTER=1\nPOINTER_SENSITIVITY=fast\nPOINTER_IDLE=1e400\nPOINTER_WAKE_COUNTS=40.5\n")),
      (True, 0.03, 30.0, 40))
check("a line bash can't read is left out",
      relay.pointer_settings(conf("POINTER=1\nPOINTER_IDLE = 5\nPOINTER_SENSITIVITY=0.1 0.2\n")), (True, 0.03, 30.0, 40))
check("8.0 as a count: 8", relay.pointer_settings(conf("POINTER_WAKE_COUNTS=8.0\n"))[3], 8)

# HANDS_NEAR: the relay gives the pointer helper's handoff hands for "on"; handsmode (Input
# Settings) says direct touch is off for 0. Each way of writing it reads alike in both.
for text, near, value in (("HANDS_NEAR=0\n", False, "0"), ("  HANDS_NEAR=0\n", False, "0"),
                          ('HANDS_NEAR="0"\n', False, "0"), ("HANDS_NEAR='on'\n", True, "on"),
                          ("HANDS_NEAR = 0\n", None, None), ("HANDS_NEAR=0 # off\n", False, "0")):
    c = conf(text)
    check(f"{text.strip()!r}: the relay reads {value!r}", c.get("HANDS_NEAR"), value)
    check(f"{text.strip()!r}: handsmode reads the same", handsmode.read_conf(os.path.join(DIR, "frametop.conf")), c)
    if near is not None:
        check(f"{text.strip()!r}: direct touch {'on' if near else 'off'}", handsmode.near_of(c), near)

print(f"\n{len(failures)} failed" if failures else "\nall ok")
sys.exit(1 if failures else 0)
