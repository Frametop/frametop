#!/usr/bin/env python3
"""A stand-in for ft-gazepanel in the bench's tests: its control socket (--socket NAME) answers
"ok" to everything, "log start DIR" writes the three logs' files there, "log stop" answers
"ok 3 100 1". Each command goes on a line of $FT_FAKE_PANEL_LOG. Quits when stdin closes, as
ft-gazepanel --watch-stdin."""
import os
import socket
import sys
import threading
from pathlib import Path


def watch():
    while sys.stdin.buffer.read(4096):
        pass
    os._exit(0)


name = sys.argv[sys.argv.index("--socket") + 1]
log = os.environ.get("FT_FAKE_PANEL_LOG")
threading.Thread(target=watch, daemon=True).start()
sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
sock.bind("\0" + name)
while True:
    data, addr = sock.recvfrom(65536)
    text = data.decode()
    if log:
        with open(log, "a") as f:
            f.write(text + "\n")
    reply = "ok"
    if text.startswith("log start "):
        for f in ("targets.jsonl", "poses.jsonl", "ipd.jsonl"):
            (Path(text[10:]) / f).write_text("{}\n")
    elif text == "log stop":
        reply = "ok 3 100 1"
    if addr:
        sock.sendto(reply.encode(), addr)
