#!/usr/bin/python3
"""vrws: vrserver's local web socket, with nothing but the standard library.

vrserver (SteamVR) serves its controller binding page on 127.0.0.1:27062, and that page
follows the controllers' raw input through a web socket there (mailbox input_server,
request_input_state_updates). Reading it takes nothing from anyone: whatever the buttons are
bound to still happens, and it works whatever app has input focus, VR games included. SteamVR
input only reaches the focused app, or an overlay with global input, which then takes the button
from the game (pointer/helper/vrbuttons.h). Frametop Voice (voice/ft-voiced) reads its
push-to-talk button the same way. It's undocumented, so a SteamVR update could change it
(scripts/update-check.py checks it). The host's Python has no `websockets` module, so this is a
small RFC 6455 client of its own.

  controllers()   the Frame controllers SteamVR has now: {root path: "left" | "right"}. A
                  controller's root path is /user/hand/<side>, or /devices/cv/<serial> while the
                  3D mouse's virtual controller holds that hand role.
  VrSocket()      a connection: open(mailbox), subscribe(path), messages(timeout) -> raw texts,
                  each a JSON object like {"type": "update_component_states", "device": path,
                  "components": {"/input/thumbstick/click": true, ...}} with the components
                  that changed (mostly capacitive sensing, about 160 a second)

Run as a program, it prints the controllers' button changes for a while (default 10 s), or with
--touch, every touch and value component too (probe P3 of the hand interaction design: which
components say a controller is held, and how soon they let go):

  input/vrws.py [--touch] [seconds]
"""
import base64
import json
import os
import select
import socket
import struct
import sys
import time
import urllib.request

HOST, PORT = "127.0.0.1", 27062
ORIGIN = f"http://{HOST}:{PORT}"
# vrserver answers the binding page's own requests; these headers make ours look like them.
HEADERS = {"Referer": f"{ORIGIN}/dashboard/controllerbinding.html"}


def getstate(timeout=3):
    """vrserver's /input/getstate.json: the devices and their input components."""
    req = urllib.request.Request(f"{ORIGIN}/input/getstate.json", headers=HEADERS)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.load(resp)


def controllers(timeout=3):
    """{root path: side} for the Frame controllers SteamVR has now."""
    return {d["root_path"]: d["side"] for d in getstate(timeout).get("devices", [])
            if d.get("controller_type") == "frame_controller" and d.get("root_path")
            and d.get("side") in ("left", "right")}


class VrSocket:
    def __init__(self, timeout=3):
        self.sock = socket.create_connection((HOST, PORT), timeout=timeout)
        try:
            key = base64.b64encode(os.urandom(16)).decode()
            self.sock.sendall((f"GET / HTTP/1.1\r\nHost: {HOST}:{PORT}\r\nUpgrade: websocket\r\n"
                               f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n"
                               f"Origin: {ORIGIN}\r\nReferer: {HEADERS['Referer']}\r\n\r\n").encode())
            head = b""
            while b"\r\n\r\n" not in head:
                chunk = self.sock.recv(4096)
                if not chunk:
                    raise OSError("vrserver closed the connection during the handshake")
                head += chunk
            head, rest = head.split(b"\r\n\r\n", 1)
            status = head.split(b"\r\n", 1)[0]
            if b" 101 " not in status + b" ":
                raise OSError(f"vrserver refused the web socket: {status.decode(errors='replace')}")
        except BaseException:
            self.sock.close()
            raise
        self.sock.settimeout(None)
        self.buf = bytearray(rest)
        self.parts = bytearray()  # a fragmented message so far
        self.mailbox = ""

    def fileno(self):
        return self.sock.fileno()

    def close(self):
        try:
            self._frame(0x8, b"")
        except OSError:
            pass
        self.sock.close()

    def _frame(self, opcode, payload):
        """One frame to vrserver: final, masked (as a client's must be)."""
        n = len(payload)
        head = bytes([0x80 | opcode])
        if n < 126:
            head += bytes([0x80 | n])
        elif n < 65536:
            head += bytes([0x80 | 126]) + struct.pack(">H", n)
        else:
            head += bytes([0x80 | 127]) + struct.pack(">Q", n)
        mask = os.urandom(4)
        self.sock.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

    def send(self, text):
        self._frame(0x1, text.encode())

    def open(self, mailbox):
        self.send(f"mailbox_open {mailbox}")
        self.mailbox = mailbox

    def subscribe(self, path, on=True):
        kind = "request_input_state_updates" if on else "cancel_input_state_updates"
        self.send("mailbox_send input_server " +
                  json.dumps({"type": kind, "device_path": path, "returnAddress": self.mailbox}))

    def messages(self, timeout=None):
        """The text messages that came in, as strings: waits up to `timeout` seconds (None:
        forever) for some, [] at the timeout. Raises OSError when the connection ends."""
        out = self._parse()
        if out:
            return out
        if not select.select([self.sock], [], [], timeout)[0]:
            return []
        chunk = self.sock.recv(65536)
        if not chunk:
            raise OSError("vrserver closed the web socket")
        self.buf += chunk
        return self._parse()

    def _parse(self):
        """Every whole frame in the buffer; a partial one waits for the rest."""
        out, buf, pos = [], self.buf, 0
        while len(buf) - pos >= 2:
            b0, b1 = buf[pos], buf[pos + 1]
            n, head = b1 & 0x7F, 2
            if n == 126:
                if len(buf) - pos < 4:
                    break
                n, head = struct.unpack_from(">H", buf, pos + 2)[0], 4
            elif n == 127:
                if len(buf) - pos < 10:
                    break
                n, head = struct.unpack_from(">Q", buf, pos + 2)[0], 10
            masked = b1 & 0x80
            if masked:
                head += 4
            if len(buf) - pos < head + n:
                break
            data = bytes(buf[pos + head:pos + head + n])
            if masked:  # servers don't mask, but nothing says they can't
                mask = buf[pos + head - 4:pos + head]
                data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
            pos += head + n
            opcode = b0 & 0x0F
            if opcode == 0x8:
                raise OSError("vrserver closed the web socket")
            if opcode == 0x9:
                self._frame(0xA, data)  # ping: pong
            elif opcode in (0x0, 0x1, 0x2):
                self.parts += data
                if b0 & 0x80:
                    out.append(self.parts.decode(errors="replace"))
                    self.parts = bytearray()
        del buf[:pos]
        return out


def main():
    args = sys.argv[1:]
    touch = "--touch" in args
    args = [a for a in args if a != "--touch"]
    seconds = float(args[0]) if args else 10.0
    ws = VrSocket()
    ws.open(f"frametop_vrws_{os.getpid()}")
    sides = controllers()
    for path, side in sides.items():
        print(f"{side} controller: {path}", flush=True)
        ws.subscribe(path)
    if not sides:
        print("no Frame controllers", flush=True)
    start = time.monotonic()
    count = 0
    try:
        while time.monotonic() - start < seconds:
            for text in ws.messages(timeout=0.5):
                count += 1
                if "/click" not in text and not (touch and ("/touch" in text or "/value" in text)):
                    continue
                msg = json.loads(text)
                side = sides.get(msg.get("device"), msg.get("device"))
                for name, value in sorted(msg.get("components", {}).items()):
                    if name.endswith("/click"):
                        print(f"{time.monotonic() - start:7.3f}  {side} {name[len('/input/'):-len('/click')]} "
                              f"{'down' if value else 'up'}", flush=True)
                    elif touch and name.endswith("/touch"):
                        print(f"{time.monotonic() - start:7.3f}  {side} {name[len('/input/'):-len('/touch')]} "
                              f"{'touched' if value else 'let go'}", flush=True)
                    elif touch and name.endswith("/value"):
                        print(f"{time.monotonic() - start:7.3f}  {side} {name} {value}", flush=True)
    finally:
        ws.close()
    print(f"{count} messages in {seconds:.0f} s")


if __name__ == "__main__":
    main()
