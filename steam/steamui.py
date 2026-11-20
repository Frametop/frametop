"""Run JavaScript in Steam's UI, standard library only.

Steam on the Frame runs with -cef-enable-debugging, so its UI's JavaScript context
(SharedJSContext) is reachable over the Chrome DevTools Protocol on 127.0.0.1:8080. Steam's
own calls live there: SteamClient.* (settings, SteamVR's dashboard) and the UI's stores
(SteamUIStore). Used by Frametop Settings (display-settings/steam_settings.py) and
ft-steam (the Steam menu shortcut).

  evaluate(expression)   the expression's value (awaited, JSON-able); SteamUnreachable when
                         Steam or its UI isn't there, or the script threw
  Session()              one connection kept open, for a caller that evaluates often
                         (keyboard/host's Steam hook): .evaluate(expression, timeout), .close()
"""
import base64
import json
import os
import socket
import struct
import urllib.request

CDP_PORT = 8080


class SteamUnreachable(Exception):
    pass


class _WebSocket:
    def __init__(self, url, timeout=5):
        host_port, path = url[len("ws://"):].split("/", 1)
        host, port = host_port.rsplit(":", 1)
        self.sock = socket.create_connection((host, int(port)), timeout=timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall((f"GET /{path} HTTP/1.1\r\nHost: {host_port}\r\nUpgrade: websocket\r\n"
                           f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise SteamUnreachable("Steam closed the connection")
            head += chunk
        if b" 101 " not in head.split(b"\r\n", 1)[0]:
            raise SteamUnreachable(head.split(b"\r\n", 1)[0].decode(errors="replace"))
        self.buf = head.split(b"\r\n\r\n", 1)[1]

    def send(self, text):
        data, mask = text.encode(), os.urandom(4)
        n = len(data)
        if n < 126:
            head = struct.pack(">BB", 0x81, 0x80 | n)
        elif n < 65536:
            head = struct.pack(">BBH", 0x81, 0x80 | 126, n)
        else:
            head = struct.pack(">BBQ", 0x81, 0x80 | 127, n)
        self.sock.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def _take(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise SteamUnreachable("Steam closed the connection")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def recv(self):
        message = b""
        while True:
            b0, b1 = self._take(2)
            n = b1 & 0x7F
            if n == 126:
                n = struct.unpack(">H", self._take(2))[0]
            elif n == 127:
                n = struct.unpack(">Q", self._take(8))[0]
            payload = self._take(n)
            opcode = b0 & 0x0F
            if opcode == 0x8:
                raise SteamUnreachable("Steam closed the connection")
            if opcode == 0x9:
                self._control(0x8A, payload)  # a ping: pong
                continue
            if opcode == 0xA:
                continue
            message += payload
            if b0 & 0x80:
                return message.decode(errors="replace")

    def _control(self, head, payload):
        mask = os.urandom(4)
        self.sock.sendall(struct.pack(">BB", head, 0x80 | len(payload)) + mask +
                          bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

    def close(self):
        self.sock.close()


def _shared_context_url():
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{CDP_PORT}/json", timeout=3) as r:
            targets = json.load(r)
    except OSError as e:
        raise SteamUnreachable(f"Steam isn't reachable on port {CDP_PORT} ({e})") from e
    url = next((t["webSocketDebuggerUrl"] for t in targets if t.get("title") == "SharedJSContext"), None)
    if not url:
        raise SteamUnreachable("Steam's UI isn't running")
    if not url.startswith(f"ws://127.0.0.1:{CDP_PORT}/"):
        raise SteamUnreachable("Steam's debugger isn't on this machine's loopback")
    return url


def _value(reply):
    result = reply.get("result", {})
    if "exceptionDetails" in result:
        details = result["exceptionDetails"]
        raise SteamUnreachable(details.get("exception", {}).get("description") or details.get("text", "error"))
    return result.get("result", {}).get("value")


class Session:
    """One connection to Steam's SharedJSContext, kept open."""

    def __init__(self, timeout=5):
        try:
            self.ws = _WebSocket(_shared_context_url(), timeout=timeout)
        except OSError as e:
            raise SteamUnreachable(str(e)) from e
        self.next_id = 0

    def evaluate(self, expression, timeout=2.0):
        self.next_id += 1
        try:
            self.ws.sock.settimeout(timeout)
            self.ws.send(json.dumps({"id": self.next_id, "method": "Runtime.evaluate",
                                     "params": {"expression": expression, "awaitPromise": True,
                                                "returnByValue": True}}))
            while True:
                reply = json.loads(self.ws.recv())
                if reply.get("id") == self.next_id:
                    return _value(reply)
        except (OSError, ValueError) as e:
            raise SteamUnreachable(str(e)) from e

    def close(self):
        self.ws.close()


def evaluate(expression):
    """Runs JavaScript in Steam's SharedJSContext and returns its (awaited) value."""
    session = Session()
    try:
        return session.evaluate(expression, timeout=5)
    finally:
        session.close()
