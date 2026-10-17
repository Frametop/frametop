#!/usr/bin/env python3
"""Offline test of the input relay's device roles (node_role): the defaults, a saved role
taking every node of a device, and "combo" (Keyboard + 3D pointer) splitting a keyboard with a
touchpad by node. It loads input-relay.py without running it: no devices, sockets, or grabs.

  input/test/roles-test.py [RELAY]   (default: input/input-relay.py next to this folder)
"""
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
relay_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "input-relay.py")
relay = importlib.util.module_from_spec(importlib.util.spec_from_loader("relay", loader=None))
relay.__file__ = os.path.abspath(relay_path)
sys.path.insert(0, os.path.dirname(os.path.abspath(relay_path)))  # its game_pause and vrws
with open(relay_path) as f:
    exec(compile(f.read(), relay_path, "exec"), relay.__dict__)

failures = []


def check(label, got, want):
    ok = got == want
    print(("ok    " if ok else "FAIL  ") + label + ("" if ok else f": got {got!r}, want {want!r}"), flush=True)
    if not ok:
        failures.append(label)


def node(name, uniq, is_mouse, is_keyboard, bus=relay.BUS_BLUETOOTH):
    return relay.Node("/dev/input/event999", -1, name, bus, 0x045E, 0x005F, uniq, is_mouse, is_keyboard)


def roles(rule, nodes):
    return [relay.node_role(rule, n, nodes) for n in nodes]


# The keyboard of 2026-10-16: "Bluetooth 5.1 Keyboard" and "... Keyboard Mouse" (its touchpad), one address.
combo = [node("Bluetooth 5.1 Keyboard", "d1:01:0b:5d:87:05", False, True),
         node("Bluetooth 5.1 Keyboard Mouse", "d1:01:0b:5d:87:05", True, False)]
check("one device: both nodes have the address as id", {n.id for n in combo}, {"d1:01:0b:5d:87:05"})
check("no rule: the whole device is a pointer (it has a mouse node)", roles({}, combo), ["pointer", "pointer"])
check("combo: keys pass through, the touchpad is the pointer", roles({"role": "combo"}, combo),
      ["passthrough", "pointer"])
check("pointer saved: every node", roles({"role": "pointer"}, combo), ["pointer", "pointer"])
check("passthrough saved: every node", roles({"role": "passthrough"}, combo), ["passthrough", "passthrough"])
check("ignore saved: every node", roles({"role": "ignore"}, combo), ["ignore", "ignore"])
check("an unknown role: the default", roles({"role": "bogus"}, combo), ["pointer", "pointer"])

# The Swiftpoint Z3: its keyboard node is its extra buttons, so it stays with its mouse by default.
z3 = [node("Z3 Mouse", "aa:bb:cc:dd:ee:ff", True, False), node("Z3 Keyboard", "aa:bb:cc:dd:ee:ff", False, True)]
check("Z3, no rule: both nodes pointer", roles({}, z3), ["pointer", "pointer"])

kb = [node("TK-KB006 Keyboard", "11:22:33:44:55:66", False, True)]
check("a plain keyboard: pass through", roles({}, kb), ["passthrough"])
check("combo on a plain keyboard: still pass through", roles({"role": "combo"}, kb), ["passthrough"])
mouse = [node("Some Mouse", "77:88:99:aa:bb:cc", True, False)]
check("combo on a plain mouse: still the pointer", roles({"role": "combo"}, mouse), ["pointer"])
both = [node("One Node Combo", "dd:ee:ff:00:11:22", True, True)]
check("combo on one node with keys and motion: the pointer (keys go through its button map)",
      roles({"role": "combo"}, both), ["pointer"])

print("FAILED: " + ", ".join(failures) if failures else "all passed", flush=True)
sys.exit(1 if failures else 0)
