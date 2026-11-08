#!/usr/bin/env python3
"""Offline test of conf/ft_hostapi.py: the token goes with the question, a pinned key must match,
and anything else (no token, an error status, not JSON, no answer) is None. An instance with a
token of its own (ADDRESS-PORT.token) is asked on its own Web UI port; a POST sends its body. The connection is a
stub holding a certificate made here, so nothing reaches a PC.

  conf/test/hostapi-test.py
"""
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import ft_hostapi  # noqa: E402

failures = []


def check(label, got, want):
    ok = got == want
    print(("ok    " if ok else "FAIL  ") + label + ("" if ok else f": got {got!r}, want {want!r}"), flush=True)
    if not ok:
        failures.append(label)


tmp = tempfile.mkdtemp(prefix="hostapi-test-")
ft_hostapi.TOKEN_DIR = tmp


def cert():
    key, crt = os.path.join(tmp, "k.pem"), os.path.join(tmp, "c.der")
    subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
                    "-nodes", "-keyout", key, "-outform", "der", "-out", crt, "-subj", "/CN=test", "-days", "1"],
                   check=True, capture_output=True)
    with open(crt, "rb") as f:
        der = f.read()
    os.unlink(key)
    os.unlink(crt)
    return der


DER, OTHER = cert(), cert()


class Reply:
    def __init__(self, status, body):
        self.status, self.body = status, body

    def read(self, n):
        return self.body[:n]


class Conn:
    made = []

    def __init__(self, der, status=200, body=b'{"scene": "game"}', fail=False):
        self.der, self.status, self.body, self.fail = der, status, body, fail
        self.sent, self.closed = None, False
        self.sock = self
        Conn.made.append(self)

    def getpeercert(self, binary_form):
        return self.der

    def connect(self):
        if self.fail:
            raise ConnectionRefusedError()

    def request(self, method, path, body=None, headers=None):
        self.sent = (method, path, headers)
        self.data = body

    def getresponse(self):
        return Reply(self.status, self.body)

    def close(self):
        self.closed = True


def write(name, text):
    with open(os.path.join(tmp, name), "w") as f:
        f.write(text)


ports = []


def connector(conn):
    def connect(a, p, t):
        ports.append(p)
        return conn
    return connect


def get(conn, address="192.0.2.20", **kw):
    return ft_hostapi.get_json(address, "/api/frametop/vrscene", connect=connector(conn), **kw)


check("no token: None, nothing asked", (get(Conn(DER)), len(Conn.made)), (None, 1))
check("...not even connected", Conn.made[-1].sent, None)

write("192.0.2.20.token", "abc123\n")
c = Conn(DER)
check("a token, no pin: the answer", get(c), {"scene": "game"})
check("...with the token as Bearer", c.sent, ("GET", "/api/frametop/vrscene", {"Authorization": "Bearer abc123"}))
check("...closed", c.closed, True)

write("192.0.2.20.pin", ft_hostapi.spki_pin(DER) + "\n")
check("the pinned key: the answer", get(Conn(DER)), {"scene": "game"})
c = Conn(OTHER)
check("another key: None", get(c), None)
check("...and the token not sent", c.sent, None)

check("403: None", get(Conn(DER, status=403)), None)
check("not JSON: None", get(Conn(DER, body=b"<html>")), None)
check("no answer: None", get(Conn(DER, fail=True)), None)

c = Conn(DER)
check("the dongle's address with the host's token", get(c, address="10.35.78.20", token_address="192.0.2.20"),
      {"scene": "game"})
check("...asked with that token", c.sent[2], {"Authorization": "Bearer abc123"})
check("pin cached per certificate", len(ft_hostapi._pins), 2)
check("...on the shared Web UI port", ports[-1], 47990)

c = Conn(DER)
check("another port without a token of its own: the shared one", get(c, port=48989), {"scene": "game"})
check("...on 47990", (ports[-1], c.sent[2]), (47990, {"Authorization": "Bearer abc123"}))
write("192.0.2.20-48989.token", "own456\n")
c = Conn(OTHER)
check("its own token, no pin of its own: any key", get(c, port=48989), {"scene": "game"})
check("...its token, on its Web UI port", (ports[-1], c.sent[2]), (48990, {"Authorization": "Bearer own456"}))
write("192.0.2.20-48989.pin", ft_hostapi.spki_pin(DER) + "\n")
check("...with its own pin: another key is refused", get(Conn(OTHER), port=48989), None)
check("47989 is the shared token's", ft_hostapi.token_path("192.0.2.20", 47989), os.path.join(tmp, "192.0.2.20.token"))

c = Conn(DER, body=b'{"output": "FT-W1"}')
got = ft_hostapi.post_json("192.0.2.20", "/api/frametop/windows/open", {"address": "0x1"}, connect=connector(c),
                           port=48989)
check("POST: status and answer", got, (200, {"output": "FT-W1"}))
check("...its body as JSON", (c.sent[0], c.sent[2].get("Content-Type"), c.data),
      ("POST", "application/json", b'{"address": "0x1"}'))
check("an error status keeps the answer", ft_hostapi.post_json(
    "192.0.2.20", "/x", {}, connect=connector(Conn(DER, status=400, body=b'{"error": "no window"}')), port=48989),
    (400, {"error": "no window"}))

for name in os.listdir(tmp):
    os.unlink(os.path.join(tmp, name))
os.rmdir(tmp)
print(f"{len(failures)} failed" if failures else "all passed")
sys.exit(1 if failures else 0)
