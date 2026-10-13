#!/usr/bin/env python3
"""A look at spotwatch's panel without a headset: ft-spotpanel --render draws a made-up search
(or the state given as a file) into a PNG, over a dark checkerboard so the glass shows.

  spotwatch/tools/render.py OUT.png [STATE_FILE]
"""
import os
import subprocess
import sys

import gi

gi.require_version("GdkPixbuf", "2.0")
from gi.repository import GdkPixbuf, GLib  # noqa: E402

HERE = os.path.dirname(os.path.realpath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
W, H = 1120, 720
SAMPLE = "\n".join([
    "query\tfire",
    "sel\t0",
    "row\titem\t-\tFirefox\tOpen · Screen 2 · 2 windows\tSwitch to",
    "row\titem\t-\tFirefox Developer Edition\tWeb Browser\tOpen",
    "row\titem\t-\tdesk-pc - OLED\tRemote display · live\tGo to",
])
if len(sys.argv) < 2:
    sys.exit(__doc__)
state = open(sys.argv[2]).read() if len(sys.argv) > 2 else SAMPLE
raw = "/tmp/ft-spotpanel-render.rgba"
subprocess.run([os.path.join(REPO, "scripts", "in-box"), os.path.join(REPO, "screens", "build", "ft-spotpanel"),
                "--render", raw], input=state.encode(), check=True,
               env=dict(os.environ, XDG_RUNTIME_DIR=f"/run/user/{os.getuid()}"))
with open(raw, "rb") as f:
    px = f.read()
panel = GdkPixbuf.Pixbuf.new_from_bytes(GLib.Bytes.new(px), GdkPixbuf.Colorspace.RGB, True, 8, W, H, W * 4)
back = GdkPixbuf.Pixbuf.new(GdkPixbuf.Colorspace.RGB, True, 8, W, H)
back.fill(0x2a3550ff)
tile = GdkPixbuf.Pixbuf.new(GdkPixbuf.Colorspace.RGB, True, 8, 40, 40)
tile.fill(0x56709aff)
for y in range(0, H, 40):
    for x in range((y // 40) % 2 * 40, W, 80):
        tile.copy_area(0, 0, min(40, W - x), min(40, H - y), back, x, y)
panel.composite(back, 0, 0, W, H, 0, 0, 1, 1, GdkPixbuf.InterpType.NEAREST, 255)
back.savev(sys.argv[1], "png", [], [])
print(sys.argv[1])
