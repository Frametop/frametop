"""Native ABI tests over private abstract datagrams. No live compositor access."""
import importlib
import json
import socket
import subprocess
import sys
import threading
import time
import uuid
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "display-settings"))


def client_module():
    assert (REPO / "display-settings/native_portal_client.py").exists(), "native IPC client is missing"
    return importlib.import_module("native_portal_client")


def status(**changes):
    return dict({"wanted": False, "connected": True}, **changes)


def wire(value=None):
    return "ok portal " + json.dumps(status() if value is None else value)


class Server:
    def __init__(self, handler=None):
        self.address = "\0native-portal-test-" + uuid.uuid4().hex
        self.commands = []
        self.handler = handler or (lambda command: wire())
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.sock.bind(self.address)
        self.sock.settimeout(.02)
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        while not self.stop.is_set():
            try:
                data, sender = self.sock.recvfrom(8192)
            except TimeoutError:
                continue
            command = data.decode()
            self.commands.append(command)
            reply = self.handler(command)
            if reply is not None:
                try:
                    self.sock.sendto(reply.encode() if isinstance(reply, str) else reply, sender)
                except OSError:
                    pass  # a timed-out private client has closed its socket

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.stop.set()
        self.thread.join(2)
        self.sock.close()


@pytest.mark.parametrize("wanted,connected", [(False, False), (True, False), (False, True), (True, True)])
def test_status_roundtrip_is_read_only(wanted, connected):
    m = client_module()
    value = status(wanted=wanted, connected=connected)
    with Server(lambda _: wire(value)) as server:
        assert m.Client(server.address).status() == value
        assert server.commands == ["passthrough portal status"]


@pytest.mark.parametrize("bad", ["ok", "error unknown command", "ok portal {}",
    wire(status(wanted=0)), wire(status(connected=1)), wire() + " garbage",
    b"ok portal \xff", 'ok portal {"wanted":true,"wanted":false,"connected":true}'])
def test_rejects_malformed_status(bad):
    m = client_module()
    with Server(lambda _: bad) as server, pytest.raises(m.PortalError):
        m.Client(server.address).status()


@pytest.mark.parametrize("enabled", [True, False])

def test_set_requires_exact_ack_then_readback(enabled):
    m = client_module()
    value = status(wanted=enabled)
    with Server(lambda _: wire(value)) as server:
        assert m.Client(server.address).set_wanted(enabled) == value
        assert server.commands == ["passthrough portal " + ("on" if enabled else "off"),
                                   "passthrough portal status"]


@pytest.mark.parametrize("ack,readback", [("ok", wire(status(wanted=True))),
    (wire(), wire()),
    (wire(status(wanted=True)), wire())])
def test_set_does_not_accept_wrong_ack_or_readback(ack, readback):
    m = client_module()
    with Server(lambda cmd: readback if cmd.endswith("status") else ack) as server, pytest.raises(m.PortalError):
        m.Client(server.address).set_wanted(True)


def test_transport_has_total_deadline_and_reply_bound():
    m = client_module()
    def delayed(_):
        time.sleep(.06)
        return wire(status(wanted=True))
    with Server(delayed) as server:
        start = time.monotonic()
        with pytest.raises(m.PortalError):
            m.Client(server.address, timeout=.09).set_wanted(True)
        assert time.monotonic() - start < .2
    with Server(lambda _: b"x" * 8192) as server, pytest.raises(m.PortalError, match="large"):
        m.Client(server.address).status()
    with Server(lambda _: None) as server, pytest.raises(m.PortalError, match="timed out"):
        m.Client(server.address, timeout=.03).status()


@pytest.mark.parametrize("command,wanted", [("on", True), ("off", False)])
def test_cli_explicit_commands_acknowledge_then_query(command, wanted):
    client_module()
    value = status(wanted=wanted)
    with Server(lambda _: wire(value)) as server:
        result = subprocess.run([sys.executable, str(REPO / "display-settings/native_portal_client.py"),
            "--socket", "@" + server.address[1:], command], capture_output=True, text=True, timeout=3, check=False)
        assert result.returncode == 0, result.stderr
        assert json.loads(result.stdout) == value
        assert server.commands == ["passthrough portal " + command, "passthrough portal status"]


def test_cli_runs_real_bounded_protocol_and_errors_honestly():
    client_module()
    with Server() as server:
        cmd = [sys.executable, str(REPO / "display-settings/native_portal_client.py"),
               "--socket", "@" + server.address[1:], "status"]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=3, check=False)
        assert result.returncode == 0, result.stderr
        assert json.loads(result.stdout) == status()
        server.handler = lambda _: "error unknown command"
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=3, check=False)
        assert result.returncode != 0
        assert "unavailable" in result.stderr.lower()
        assert not result.stdout
