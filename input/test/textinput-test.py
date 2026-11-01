#!/usr/bin/env python3
"""Offline test of ft-textinput, Frametop's input method: what it tells the relay, and the
characters it commits for the relay (DEE-192).

Runs its main() against a fake KWin (one end of a socketpair as WAYLAND_SOCKET) and a fake relay
on a renamed abstract socket. Nothing reaches the live desktop, KWin or the running relay.

  input/test/textinput-test.py
"""
import importlib.machinery
import importlib.util
import os
import socket
import struct
import sys
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
PATH = os.path.join(HERE, "..", "ft-textinput")
loader = importlib.machinery.SourceFileLoader("ft_textinput", PATH)
ti = importlib.util.module_from_spec(importlib.util.spec_from_loader("ft_textinput", loader))
loader.exec_module(ti)
ti.RELAY = f"\0ft_textinput_test_{os.getpid()}"

CONTEXT = 0xFF000001  # the context id KWin (the server) gives an activation
failures = []


def check(label, got, want):
    ok = got == want
    print(("ok    " if ok else "FAIL  ") + label + ("" if ok else f": got {got!r}, want {want!r}"), flush=True)
    if not ok:
        failures.append(label)


kwin, client = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
kwin.settimeout(1.0)
relay = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
relay.bind(ti.RELAY)
relay.settimeout(1.0)
os.environ["WAYLAND_SOCKET"] = str(client.fileno())
thread = threading.Thread(target=ti.main, daemon=True)
thread.start()


def event(obj, opcode, body=b""):
    kwin.sendall(struct.pack("=II", obj, (8 + len(body)) << 16 | opcode) + body)


def request():
    """The next request from ft-textinput as (object, opcode, body), or None within the timeout."""
    try:
        head = kwin.recv(8, socket.MSG_WAITALL)
    except socket.timeout:
        return None
    obj, word = struct.unpack("=II", head)
    body = kwin.recv((word >> 16) - 8, socket.MSG_WAITALL) if word >> 16 > 8 else b""
    return obj, word & 0xFFFF, body


def quiet():
    kwin.settimeout(0.3)
    got = request()
    kwin.settimeout(1.0)
    return got


check("asks for the registry", request(), (ti.DISPLAY, 1, struct.pack("=I", ti.REGISTRY)))
event(ti.REGISTRY, 0, struct.pack("=I", 7) + ti.string("zwp_input_method_v1") + struct.pack("=I", 1))
check("binds the input method", request(),
      (ti.REGISTRY, 0, struct.pack("=I", 7) + ti.string("zwp_input_method_v1") + struct.pack("=II", 1, ti.INPUT_METHOD)))

event(ti.INPUT_METHOD, 0, struct.pack("=I", CONTEXT))  # activate
said, ft_addr = relay.recvfrom(64)
check("a text field focused: textfield 1 to the relay", said, b"textfield 1")
check("...from an autobound address, not a fixed name", ft_addr.startswith(b"\0") and len(ft_addr) == 6, True)

event(CONTEXT, 4, struct.pack("=I", 9))  # commit_state, serial 9
relay.sendto("commit ą".encode(), ft_addr)
check("commit from the relay: commit_string on the context with its serial", request(),
      (CONTEXT, 1, struct.pack("=I", 9) + ti.string("ą")))

other = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
other.bind("")
other.sendto(b"commit x", ft_addr)
check("commit from another address: nothing", quiet(), None)
relay.sendto(b"type x", ft_addr)
check("anything but commit from the relay: nothing", quiet(), None)

event(ti.INPUT_METHOD, 1, struct.pack("=I", CONTEXT))  # deactivate
check("the text field lost focus: the context is destroyed", request(), (CONTEXT, 0, b""))
check("...and textfield 0 to the relay", relay.recv(64), b"textfield 0")
relay.sendto(b"commit y", ft_addr)
check("commit with no text field focused: nothing", quiet(), None)

kwin.close()
thread.join(2)
check("KWin gone: it ends", thread.is_alive(), False)
print("all passed" if not failures else "FAILED: " + ", ".join(failures))
sys.exit(1 if failures else 0)
