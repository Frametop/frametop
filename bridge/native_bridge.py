#!/usr/bin/env python3
"""App-scoped CDP transport. Only localhost Steam is contacted; no listeners or files are created."""
import base64
from collections import deque
import hashlib
import ipaddress
import json
import math
import os
from pathlib import Path
import select
import signal
import socket
import struct
import subprocess
import sys
import time
from urllib.parse import urlsplit
from urllib.request import HTTPRedirectHandler, ProxyHandler, build_opener

MAX_PACKET = 32768
GLOBAL = "__fullKeyboardNativeV1"
INSTALL_TIMEOUT = 2.0
ACTIVE_POLL = 0.035
IDLE_POLL = 0.075


def poll_interval(state):
    # Missing/new/inconsistent metadata must retain the ordinary fast cadence.
    busy = ('open', 'eligible', 'active', 'external', 'externalCandidate', 'externalRestore')
    return IDLE_POLL if state.get('idle') is True and not any(state.get(k) for k in busy) else ACTIVE_POLL


def install_expression(source, token, external_supported, expires):
    reference = f"globalThis.{GLOBAL}"
    # Python and CEF run on the same device. A command queued beyond its local
    # deadline must not install hooks later, even if CDP cleanup cannot get through.
    return (f"(() => {{ if (Date.now() >= {expires}) return null; "
            f"if ({reference}) {{ if (!{reference}.bridge.expired()) return false; "
            f"{reference}.bridge.dispose(); delete {reference}; }} "
            f"const bridge = ({source})(globalThis, undefined, {str(external_supported).lower()}); "
            f"if (Date.now() >= {expires}) {{ bridge.dispose(); return null; }} "
            f"{reference} = {{token: {json.dumps(token)}, bridge}}; return true; }})()")


def cleanup_expression(token, restore=True):
    reference = f"globalThis.{GLOBAL}"
    argument = "" if restore else "false"
    return (f"(() => {{ const b = {reference}; if (b?.token === {json.dumps(token)}) {{"
            f"b.bridge.dispose({argument}); delete {reference}; }} }})()")


class BridgeUnavailable(ValueError):
    """Only fixed, non-sensitive codes may cross the private status channel."""
    def __init__(self, reason):
        self.reason = reason if reason in {"debugger-blocked", "steam-incompatible",
                                          "replacement-busy", "reply-timeout"} else "helper-unavailable"
        super().__init__(self.reason)


def require_loopback_debugger():
    """Require the debugger endpoint we use to be bound to loopback."""
    for table in ("tcp", "tcp6"):
        try:
            lines = Path("/proc/net", table).read_text().splitlines()[1:]
        except FileNotFoundError:
            if table == "tcp6":
                continue  # IPv6 can be disabled by the device.
            raise BridgeUnavailable("debugger-blocked") from None
        except OSError:
            raise BridgeUnavailable("debugger-blocked") from None
        for line in lines:
            fields = line.split()
            if fields[3] != "0A":
                continue
            host, port = fields[1].split(":")
            if int(port, 16) != 8080:
                continue
            raw = bytes.fromhex(host)
            if sys.byteorder == "little":
                raw = b"".join(raw[i:i + 4][::-1] for i in range(0, len(raw), 4))
            address = ipaddress.ip_address(raw)
            mapped = getattr(address, "ipv4_mapped", None)
            if not (mapped or address).is_loopback:
                raise BridgeUnavailable("debugger-blocked")


class NoDebuggerRedirects(HTTPRedirectHandler):
    def redirect_request(self, *_args, **_kwargs):
        raise BridgeUnavailable("debugger-blocked")


class NativeGuard:
    """Separate native lease. EOF restores an armed exact-overlay request without CDP."""
    def __init__(self):
        self.socket = self.process = None
        self.armed = False
        executable = Path(__file__).resolve().parents[3] / "bin/framekeyboard-native-guard"
        if not executable.is_file():
            executable = Path(__file__).resolve().parent.parent / "framekeyboard-native-guard"
            if not executable.is_file():
                raise FileNotFoundError()
        parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.socket = parent
        parent.settimeout(.5)
        try:
            self.process = subprocess.Popen([str(executable), "--fd", str(child.fileno())],
                                            pass_fds=(child.fileno(),), stdin=subprocess.DEVNULL,
                                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if json.loads(parent.recv(2048)).get("ok") is not True:
                raise ValueError("native-guard-unavailable")
        except BaseException:
            self.close()
            raise
        finally:
            child.close()

    def command(self, op, **fields):
        data = json.dumps(dict(op=op, **fields), separators=(",", ":")).encode()
        if self.socket.send(data) != len(data):
            raise ValueError("native-guard-backpressure")
        return json.loads(self.socket.recv(2048)).get("ok") is True

    def prepare(self, candidate, frame):
        ok = self.command("prepare", key=candidate["key"], flags=candidate["flags"],
                          requireActive=candidate.get("requireActive") is True,
                          progress=frame["progress"], sent=frame["sent"], ready=frame["ready"])
        if ok:
            self.armed = True
        return ok

    def renew(self, frame):
        self.command("renew", progress=frame["progress"], sent=frame["sent"], ready=frame["ready"])

    def cancel(self):
        if not self.command("cancel"):
            raise ValueError("native-guard-cancel-failed")
        self.armed = False

    def restore(self):
        if not self.command("restore"):
            raise ValueError("native-guard-restore-failed")
        self.armed = False

    def close(self):
        if self.socket:
            self.socket.close()
            self.socket = None
        # An active native fallback belongs to this client. Leave the guard alive
        # until Steam reports its closure, or VR shutdown would close it immediately.
        if self.process:
            try:
                self.process.wait(timeout=.1)
            except subprocess.TimeoutExpired:
                pass


class CDP:
    def __init__(self):
        self.socket = None
        self.pending = b""
        self.sequence = 0
        try:
            require_loopback_debugger()
            self.network_checked = time.monotonic()
            opener = build_opener(ProxyHandler({}), NoDebuggerRedirects())
            with opener.open("http://127.0.0.1:8080/json/list", timeout=0.4) as response:
                data = response.read(1024 * 1024 + 1)
            if len(data) > 1024 * 1024:
                raise ValueError("target-list-too-large")
            targets = json.loads(data)
            target = next((t for t in targets if t.get("title") == "SharedJSContext"), None)
            if target is None:
                raise BridgeUnavailable("steam-incompatible")
            url = urlsplit(target["webSocketDebuggerUrl"])
            if url.scheme != "ws" or url.hostname not in ("localhost", "127.0.0.1") or url.port != 8080:
                raise BridgeUnavailable("debugger-blocked")
            self.socket = socket.create_connection(("127.0.0.1", 8080), timeout=0.3)
            key = base64.b64encode(os.urandom(16)).decode()
            path = url.path + ("?" + url.query if url.query else "")
            self.socket.sendall((f"GET {path} HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n"
                                 "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                                 f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
            while b"\r\n\r\n" not in self.pending:
                chunk = self.socket.recv(4096)
                if not chunk:
                    raise EOFError()
                self.pending += chunk
                if len(self.pending) > 16384:
                    raise ValueError("invalid-upgrade")
            header, self.pending = self.pending.split(b"\r\n\r\n", 1)
            accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest())
            if not header.startswith(b"HTTP/1.1 101 ") or accept.lower() not in header.lower():
                raise ValueError("upgrade-refused")
        except BaseException:
            self.close()
            raise

    def close(self):
        if self.socket:
            self.socket.close()
            self.socket = None

    def peek(self, count, deadline):
        while len(self.pending) < count:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError()
            self.socket.settimeout(remaining)
            data = self.socket.recv(min(65536, count - len(self.pending)))
            if not data:
                raise EOFError()
            self.pending += data
        return self.pending[:count]

    def receive_frame(self, deadline):
        # Do not consume a partial frame on timeout. The next call must be able
        # to drain a late reply, including one interrupted inside its payload.
        first, second = self.peek(2, deadline)
        length, header = second & 127, 2
        if length == 126:
            length, header = struct.unpack("!H", self.peek(4, deadline)[2:4])[0], 4
        elif length == 127:
            length, header = struct.unpack("!Q", self.peek(10, deadline)[2:10])[0], 10
        if length > 1024 * 1024 or second & 128 or not first & 0x80 or first & 0x70:
            raise ValueError("invalid-debug-frame")
        data = self.peek(header + length, deadline)[header:]
        self.pending = self.pending[header + length:]
        return first & 15, data

    def frame(self, opcode, data, deadline):
        size = len(data)
        header = bytes([0x80 | opcode])
        if size < 126:
            header += bytes([0x80 | size])
        elif size < 65536:
            header += bytes([0xfe]) + struct.pack("!H", size)
        else:
            header += bytes([0xff]) + struct.pack("!Q", size)
        mask = os.urandom(4)
        self.socket.settimeout(max(0.001, deadline - time.monotonic()))
        try:
            self.socket.sendall(header + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))
        except OSError as error:
            # A partial write cannot safely be retried on the same stream.
            self.close()
            raise ConnectionError("debugger-write-failed") from error

    def evaluate(self, expression, timeout=0.4):
        if time.monotonic() - self.network_checked >= 1:
            require_loopback_debugger()
            self.network_checked = time.monotonic()
        self.sequence += 1
        command = {"id": self.sequence, "method": "Runtime.evaluate", "params": {
            "expression": expression, "returnByValue": True, "silent": True}}
        deadline = time.monotonic() + timeout
        self.frame(1, json.dumps(command, ensure_ascii=True).encode(), deadline)
        while time.monotonic() < deadline:
            opcode, payload = self.receive_frame(deadline)
            if opcode == 9:
                self.frame(10, payload, deadline)
                continue
            if opcode == 8:
                raise EOFError()
            if opcode != 1:
                continue
            message = json.loads(payload)
            if message.get("id") != self.sequence:
                continue
            if "exceptionDetails" in message.get("result", {}) or "error" in message:
                raise BridgeUnavailable("steam-incompatible")
            return message.get("result", {}).get("result", {}).get("value")
        raise TimeoutError()


def send(parent, value):
    data = json.dumps(value, ensure_ascii=True, separators=(",", ":")).encode()
    if len(data) > MAX_PACKET or parent.send(data) != len(data):
        raise ValueError("parent-backpressure")


def report_unavailable(parent, reason):
    # Closing a seqpacket peer with unread frames discards its final reply with
    # ECONNRESET. Refuse new writes and drain the bounded queue before reporting.
    parent.shutdown(socket.SHUT_RD)
    for _ in range(1024):
        if not parent.recv(MAX_PACKET + 1, socket.MSG_DONTWAIT):
            break
    send(parent, {"op": "unavailable", "reason": reason})


class WatchdogProgress:
    """Private pipe from the overlay watchdog, independent of the VR/main loop."""
    def __init__(self, fd):
        self.fd = fd
        self.pending = b""
        self.sent = None
        self.slow_until = 0
        self.runtime_until = 0

    def poll(self):
        if self.fd is None:
            return
        while True:
            try:
                data = os.read(self.fd, 4096)
            except BlockingIOError:
                break
            if not data:
                break
            self.pending += data
            while len(self.pending) >= 24:
                sent, slow_until, runtime_until = struct.unpack("=ddd", self.pending[:24])
                self.pending = self.pending[24:]
                if math.isfinite(sent) and 0 < sent <= time.monotonic() and \
                        (self.sent is None or sent > self.sent):
                    self.sent = sent
                    self.slow_until = slow_until if math.isfinite(slow_until) and \
                        0 < slow_until <= sent + 30 else 0
                    self.runtime_until = runtime_until if math.isfinite(runtime_until) and \
                        0 < runtime_until <= sent + 30 else 0

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None


class ParentRecovery:
    def __init__(self):
        self.terminated_at = None
        self.boot_gap = None
        self.resume_until = 0
        self.resume_at = 0
        self.resume_frame = None

    @staticmethod
    def parent_stopped(owner):
        try:
            # comm can contain spaces and parentheses. The state follows its
            # final closing parenthesis; only a signal stop uses uppercase T.
            fields = Path(f"/proc/{owner}/stat").read_text().rsplit(") ", 1)[1].split()
            return fields[0] == "T"
        except (OSError, IndexError):
            return False  # An unreadable state leaves the normal deadlines in place.

    def observe_clock(self, now, boot_now, frame_sent):
        # BOOTTIME includes system suspend; MONOTONIC and the frame clock do not.
        gap = boot_now - now
        if self.boot_gap is not None and gap - self.boot_gap > .5 and self.resume_frame != frame_sent:
            self.resume_at, self.resume_until, self.resume_frame = now, now + 30, frame_sent
        self.boot_gap = gap
        if frame_sent > self.resume_at:
            self.resume_until = 0

    def check(self, owner, watch, frame_sent, watchdog_sent, slow_until=0, runtime_until=0):
        now = time.monotonic()
        if not owner or not watch or os.getppid() != owner:
            self.terminated_at = None
            return False
        stopped = self.parent_stopped(owner)
        if os.getppid() != owner:
            self.terminated_at = None
            return False
        if stopped:
            # Neither the main loop nor its watchdog can run after SIGSTOP, and
            # SIGTERM would remain pending. Remove the frozen panel's owner now.
            os.kill(owner, signal.SIGKILL)
            return True
        if now - frame_sent < 3:
            self.terminated_at = None
            return False
        if self.terminated_at is None and (now < self.resume_until or
                                          now < min(runtime_until, frame_sent + 30)):
            return False
        # A live overlay watchdog does not prove main-loop progress. Only an
        # explicit slow operation may delay recovery, never beyond 30 seconds
        # from the last completed frame. It cannot cancel a termination begun.
        if self.terminated_at is None and watchdog_sent is not None and \
                0 <= now - watchdog_sent < 3 and now < min(slow_until, frame_sent + 30):
            return False
        if self.terminated_at is None:
            os.kill(owner, signal.SIGTERM)
            self.terminated_at = now
        elif now - self.terminated_at >= 1:
            os.kill(owner, signal.SIGKILL)
            return True
        return False


def run(parent, client_factory=CDP, parent_pid=None, guard_factory=NativeGuard, health_fd=None):
    client = None
    guard = None
    install_attempted = False
    watchdog = WatchdogProgress(health_fd)
    recovery_watchdog = ParentRecovery()
    token = os.urandom(16).hex()
    reference = f"globalThis.{GLOBAL}"
    failure_reason = "debugger-unavailable"
    restore_native = True
    try:
        client = client_factory()
        try:
            guard = guard_factory()
        except (OSError, ValueError, EOFError):
            pass  # Steam targets remain supported when the native adapter is unavailable.
        failure_reason = "helper-unavailable"
        source = Path(__file__).with_name("steam_bridge.js").read_text()
        failure_reason = "steam-incompatible"
        # Never displace another live instance. A dead owner's lease must expire first.
        install_attempted = True
        result = client.evaluate(install_expression(source, token, guard is not None,
                                                   int((time.time() + INSTALL_TIMEOUT) * 1000)),
                                 timeout=INSTALL_TIMEOUT)
        if result is None:
            raise TimeoutError()  # The expression itself rejected a late installation.
        if result is not True:
            raise BridgeUnavailable("replacement-busy")
        failure_reason = "connection-lost"
        frame = {"progress": 0, "ready": False, "ack": 0, "fallback": False,
                 "sent": time.monotonic()}
        frame_sent = time.monotonic()
        last_parent = time.monotonic()
        watch_main_loop = False
        fallback_request = None
        external_ack = 0
        next_poll = 0.0
        pending = deque()
        held_keys = {}
        pending_repeats = set()
        stopping = False
        consecutive_timeouts = 0

        def evaluate(expression):
            nonlocal consecutive_timeouts
            try:
                result = client.evaluate(expression)
            except TimeoutError:
                consecutive_timeouts += 1
                if consecutive_timeouts >= 3:
                    raise BridgeUnavailable("reply-timeout") from None
                raise
            consecutive_timeouts = 0
            return result

        while True:
            if not Path(__file__).is_file() or not Path(__file__).with_name("steam_bridge.js").is_file():
                break  # Removing an installed app also withdraws a still-running bridge.
            readable, _, _ = select.select([parent], [], [],
                                           0 if pending else max(0, next_poll - time.monotonic()))
            # Drain releases before dispatching more work. CDP can be slower than
            # repeat generation; initial clicks stay ordered, repeats do not pile up.
            received = 0
            while readable:
                received += 1
                if received > 256:
                    raise ValueError("parent-backlog")
                packet = parent.recv(MAX_PACKET + 1)
                if not packet:
                    stopping = True
                    break
                if len(packet) > MAX_PACKET:
                    raise ValueError("oversize-packet")
                message = json.loads(packet)
                operation = message.get("op")
                if operation == "stop":
                    # Only an explicit prepared-update shutdown may suppress
                    # recovery. EOF, signals and malformed packets still restore.
                    restore_native = message.get("restore") is not False
                    stopping = True
                    break
                if operation == "frame":
                    if type(message.get("progress")) is not int or type(message.get("ready")) is not bool:
                        raise ValueError("invalid-frame")
                    frame = {k: message[k] for k in ("progress", "ready", "ack", "sent")}
                    # Restoration acknowledgements must survive subsequent frame packets.
                    frame["externalRestored"] = external_ack
                    if guard:
                        guard.renew(frame)
                    if message.get("fallback") is True:
                        fallback_request = message.get("fallbackRequest", 0)
                        next_poll = 0
                    last_parent = time.monotonic()
                    frame_sent = float(message["sent"])
                    watch_main_loop = message.get("watch") is True
                elif operation == "key-up":
                    if type(message.get("id")) is not int or type(message.get("request")) is not int:
                        raise ValueError("invalid-release")
                    key_id = message["id"]
                    held = held_keys.get(key_id)
                    if held and held[0] == message["request"]:
                        del held_keys[key_id]
                        cancelled_repeat = key_id in pending_repeats
                        pending_repeats.discard(key_id)
                        pending = deque(m for m in pending if not (
                            m["op"] == "key" and m["value"] == 2 and m["id"] == key_id))
                        if cancelled_repeat:
                            # No deletion after release, but the parent still
                            # needs completion for the repeat it submitted.
                            send(parent, {"op": "key-result", "id": key_id, "ok": True})
                elif operation == "dismiss":
                    if type(message.get("request")) is not int:
                        raise ValueError("invalid-dismissal")
                    pending.append(message)
                elif operation in ("text", "key"):
                    if type(message.get("request")) is not int or not isinstance(message.get("text"), str):
                        raise ValueError("invalid-input")
                    if len(message["text"].encode()) > 8192:
                        raise ValueError("oversize-input")
                    if operation == "key":
                        if type(message.get("id")) is not int or message["id"] <= 0 or \
                                type(message.get("value")) is not int or message["value"] not in (1, 2):
                            raise ValueError("invalid-key")
                        key_id = message["id"]
                        identity = (message["request"], message["text"])
                        if message["value"] == 1:
                            if key_id in held_keys or len(held_keys) >= 128:
                                raise ValueError("invalid-key-hold")
                            held_keys[key_id] = identity
                            pending.append(message)
                        elif held_keys.get(key_id) == identity and key_id not in pending_repeats:
                            pending_repeats.add(key_id)
                            pending.append(message)
                    else:
                        pending.append(message)
                else:
                    raise ValueError("unknown-operation")
                if len(pending) > 128:
                    raise ValueError("input-backlog")
                readable, _, _ = select.select([parent], [], [], 0)
            if stopping:
                break
            if pending:
                message = pending.popleft()
                operation = message["op"]
                try:
                    if operation == "dismiss":
                        ok = evaluate(f"{reference}?.bridge.dismiss({message['request']}) === true")
                        next_poll = 0
                    else:
                        if operation == "key" and message["value"] == 2:
                            pending_repeats.discard(message["id"])
                        # The JS side checks the target and lease again at delivery.
                        ok = evaluate(f"{reference}?.bridge.send({message['request']},"
                                      f"{json.dumps(message['text'], ensure_ascii=True)},"
                                      f"{str(operation == 'key').lower()}) === true")
                except TimeoutError:
                    ok = False  # Unknown delivery fails this request closed.
                if operation != "dismiss":
                    send(parent, {"op": f"{operation}-result", "id": message["id"], "ok": ok is True})
                if ok is not True:
                    fallback_request = message["request"]
            watchdog.poll()
            recovery_watchdog.observe_clock(time.monotonic(), time.clock_gettime(time.CLOCK_BOOTTIME), frame_sent)
            if not select.select([parent], [], [], 0)[0] and recovery_watchdog.check(
                    parent_pid, watch_main_loop, frame_sent, watchdog.sent, watchdog.slow_until,
                    watchdog.runtime_until):
                break
            if time.monotonic() - last_parent > 0.7 or not 0 <= time.monotonic() - frame_sent < 0.7:
                frame["ready"] = False
            if time.monotonic() >= next_poll:
                poll_frame = {**frame, "fallback": fallback_request is not None,
                              "fallbackRequest": fallback_request}
                try:
                    result = evaluate(f"{reference}?.bridge.step({json.dumps(poll_frame)})")
                    if not isinstance(result, dict) or result.get("version") != 1 or result.get("unavailable"):
                        raise BridgeUnavailable("steam-incompatible")
                    if guard:
                        recovery = result.get("externalRestore")
                        if recovery and recovery["request"] != external_ack:
                            guard.restore()
                            external_ack = frame["externalRestored"] = recovery["request"]
                        candidate = result.get("externalCandidate")
                        if candidate:
                            # Dismissal and resummon can both occur between polls. A
                            # candidate has no logical owner, so any old lease must
                            # be cancelled before the next exact target is armed.
                            if guard.armed:
                                guard.cancel()
                            if guard.prepare(candidate, frame):
                                result = evaluate(f"{reference}?.bridge.prepareExternal("
                                                         f"{candidate['request']},{json.dumps(candidate['key'])})")
                                if not isinstance(result, dict):
                                    raise BridgeUnavailable("steam-incompatible")
                        recovery = result.get("externalRestore")
                        if recovery and recovery["request"] != external_ack:
                            guard.restore()
                            external_ack = frame["externalRestored"] = recovery["request"]
                        elif not result.get("external") and guard.armed:
                            guard.cancel()
                except TimeoutError:
                    # step is idempotent. Keep the last state/fallback request;
                    # the JS lease and C++ freshness timeout remain independent.
                    next_poll = time.monotonic() + ACTIVE_POLL
                    continue
                fallback_request = None
                send(parent, {"op": "state", **result})
                next_poll = time.monotonic() + poll_interval(result)
    except (Exception, SystemExit) as error:
        # Errors are fixed status codes. Never echo CDP expressions, input packets or field values.
        try:
            reason = error.reason if isinstance(error, BridgeUnavailable) else failure_reason
            if isinstance(error, TimeoutError) and failure_reason == "steam-incompatible":
                reason = "install-timeout"
            report_unavailable(parent, reason)
        except Exception:
            pass
    finally:
        if client:
            if install_attempted:
                try:
                    # A lost reply does not prove the install failed. The token
                    # prevents this attempt's cleanup from removing a successor.
                    client.evaluate(cleanup_expression(token, restore_native))
                    if guard and guard.armed:
                        if restore_native:
                            guard.restore()
                        else:
                            guard.cancel()
                except Exception:
                    pass  # Steam-side watchdog restores presentation even if CDP disappeared.
            client.close()
        if guard:
            guard.close()
        watchdog.close()
        parent.close()


if __name__ == "__main__":
    # SIGTERM follows the same cleanup path as EOF from the owning application.
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    if len(sys.argv) != 5 or sys.argv[1] != "--fd" or sys.argv[3] != "--health-fd":
        sys.exit(2)
    run(socket.socket(fileno=int(sys.argv[2])), parent_pid=os.getppid(), health_fd=int(sys.argv[4]))
