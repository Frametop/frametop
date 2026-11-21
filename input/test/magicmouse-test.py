#!/usr/bin/env python3
"""Offline test of Magic Mouse 2 surface-to-wheel decoding (input/magicmouse.py):
packed touch records, hid-magicmouse's scroll step, and real Bluetooth reports.
Nothing opens hidraw or uinput.

  input/test/magicmouse-test.py
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import magicmouse as mm  # noqa: E402

failures = []


def check(label, got, want):
    ok = got == want
    print(("ok    " if ok else "FAIL  ") + label + ("" if ok else f": got {got!r}, want {want!r}"), flush=True)
    if not ok:
        failures.append(label)


def report(touches, clicks=0, relx=0, rely=0):
    buf = bytearray(14 + 8 * len(touches))
    buf[0] = mm.MOUSE2_REPORT_ID
    buf[1] = clicks
    struct.pack_into("<hh", buf, 2, relx, rely)
    for i, t in enumerate(touches):
        buf[14 + i * 8:22 + i * 8] = mm.pack_touch(*t)
    return bytes(buf)


check("Magic Mouse 2 Bluetooth", mm.is_magic_mouse2(0x004C, 0x0269), True)
check("Magic Mouse 2 USB", mm.is_magic_mouse2(0x05AC, 0x0269), True)
check("Magic Mouse 2 USB-C", mm.is_magic_mouse2(0x004C, 0x0323), True)
check("Magic Keyboard is not a mouse", mm.is_magic_mouse2(0x004C, 0x0267), False)
check("s12 positive", mm.s12(0x368), 872)
check("s12 negative", mm.s12(0xDFB), -517)

packed = mm.pack_touch(4, -517, -872, 9, mm.TOUCH_STATE_DRAG)
check("pack/parse touch", mm.parse_touch(packed), (4, -517, -872, 9, mm.TOUCH_STATE_DRAG))

emu = mm.ScrollEmulator()
check("short mouse report: no wheel", emu.feed(bytes((0x12, 0, 0, 0, 0, 0, 0, 0))), [])
check("wrong report id", emu.feed(bytes((0x31, 0, 0, 0, 0, 0, 0, 0))), [])

emu = mm.ScrollEmulator()
start = report([(1, 0, 0, 10, mm.TOUCH_STATE_START)])
drag = report([(1, 0, 250, 10, mm.TOUCH_STATE_DRAG)])
check("finger down: no wheel yet", emu.feed(start), [])
# divisor = (64-32)*7 = 224; step_y = cdiv(0-250, 224) = -1
check("a 250-unit drag: one notch down", emu.feed(drag), [(mm.REL_WHEEL, -1)])

emu = mm.ScrollEmulator()
emu.feed(report([(2, 0, 0, 10, mm.TOUCH_STATE_START)]))
# two steps in one report: 448/224 = 2
check("a longer drag: two notches",
      emu.feed(report([(2, 0, 448, 10, mm.TOUCH_STATE_DRAG)])), [(mm.REL_WHEEL, -2)])

emu = mm.ScrollEmulator()
emu.feed(report([(3, 0, 0, 10, mm.TOUCH_STATE_START)]))
check("horizontal drag: hwheel",
      emu.feed(report([(3, 224, 0, 10, mm.TOUCH_STATE_DRAG)])), [(mm.REL_HWHEEL, 1)])

# A real Bluetooth report captured on this Frame after F1 02 01 (one finger, 22 bytes).
real = bytes.fromhex("12 00 d7 ff 00 00 00 00 29 00 00 40 a0 be fb 8d 36 87 6f 09 75 42")
ident, x, y, size, state = mm.parse_touch(real[14:])
check("captured touch id", ident, 4)
check("captured touch x", x, -517)
check("captured touch y", y, -872)
check("captured touch size", size, 9)
check("captured touch is a drag", state, mm.TOUCH_STATE_DRAG)

emu = mm.ScrollEmulator()
check("a captured report feeds without error", isinstance(emu.feed(real), list), True)
ident, x, y, size, state = mm.parse_touch(real[14:])
emu = mm.ScrollEmulator()
emu.feed(report([(ident, x, y, size, mm.TOUCH_STATE_START)]))
check("the same finger 300 units later: a wheel notch",
      emu.feed(report([(ident, x, y + 300, size, mm.TOUCH_STATE_DRAG)])), [(mm.REL_WHEEL, -1)])

emu = mm.ScrollEmulator()
check("a finger first seen mid-drag starts where it is: no jump",
      (emu.feed(report([(5, 100, -900, 10, mm.TOUCH_STATE_DRAG)])),
       emu.feed(report([(5, 100, -900 + 300, 10, mm.TOUCH_STATE_DRAG)]))), ([], [(mm.REL_WHEEL, -1)]))

# Which hidraw nodes are the mouse comes from sysfs: nothing else is opened.
import tempfile  # noqa: E402
sysfs = tempfile.mkdtemp(prefix="ft-mm-sysfs-")
for name, hid_id, uniq in (("hidraw0", "0003:0000046D:0000C52B", "aa:bb"),   # another mouse, same uniq
                           ("hidraw1", "0005:0000004C:00000269", "AA:BB"),
                           ("hidraw2", "0005:0000004C:00000269", "cc:dd"),
                           ("hidraw3", "garbage", "aa:bb")):
    os.makedirs(f"{sysfs}/{name}/device")
    with open(f"{sysfs}/{name}/device/uevent", "w") as f:
        f.write(f"DRIVER=hid-generic\nHID_ID={hid_id}\nHID_NAME=x\nHID_UNIQ={uniq}\n")
check("sysfs names only the Magic Mouse 2 with that uniq", mm._hidraw_names("aa:bb", sysfs), ["hidraw1"])
check("...and nothing when there's no sysfs", mm._hidraw_names("aa:bb", sysfs + "/none"), [])

# MagicMice.sync: a mouse whose surface reports won't turn on isn't tried again each second.
import types  # noqa: E402
tries, logged = [], []
mm._find_hidraw = lambda uniq: tries.append(uniq) or ("/dev/hidraw9", os.open(os.devnull, os.O_RDONLY))
mm._enable_multitouch = lambda fd: False
mice = mm.MagicMice(logged.append)
node = types.SimpleNamespace(is_mouse=True, uniq="AA:BB", vendor=0x004C, product=0x0269)
for _ in range(3):
    mice.sync({1: node})
check("surface reports that won't turn on: tried once, logged once", (tries, len(logged), mice.fds()), (["aa:bb"], 1, []))
mice.sync({})
mice.sync({1: node})
check("...and tried again after it reconnects", tries, ["aa:bb", "aa:bb"])

print("FAILED: " + ", ".join(failures) if failures else "all passed", flush=True)
sys.exit(1 if failures else 0)
