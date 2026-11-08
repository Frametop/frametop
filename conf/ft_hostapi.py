"""ft_hostapi: questions to a PC's Vibepollo Web UI API, as ft-stream asks them (stream/host.cpp).

A host from Remote Displays has an API token in ~/.local/share/frametop-stream/hosts/ADDRESS.token
and, when it was signed in from there, the public key pin its certificate had then in ADDRESS.pin
(curl's "sha256//BASE64" form). The certificate is self-signed, so it isn't checked against a CA:
a pin there must match, or the answer isn't read. A host without a pin file (one paired with
make-frametop-token) takes any certificate, as ft-stream does.

An instance on a port of its own (a host's Frametop windows, on 48989) may have a token of its
own, ADDRESS-PORT.token (and ADDRESS-PORT.pin): with port=PORT, that one and that instance's Web
UI (PORT + 1) when it's there, else the shared token and 47990, as ft-stream does (stream/host.h).

get_json(address, path) is one GET with the token, its JSON answer or None (no token, no answer,
a wrong key, not JSON); post_json(address, path, body) one POST, (status, JSON answer or None).
Standard library plus the openssl command for the pin; a pin is worked out once per certificate.
"""

import base64
import hashlib
import http.client
import json
import os
import ssl
import subprocess

TOKEN_DIR = os.path.expanduser("~/.local/share/frametop-stream/hosts")
WEB_UI_PORT = 47990

_pins = {}  # sha256 of a certificate -> its public key pin


GAMESTREAM_PORT = 47989


def token_path(address, port=0):
    """ADDRESS.token, or with `port` ADDRESS-PORT.token when there is one: an instance's own (a
    host's Frametop windows: a Linux PC's second instance on 48989, or a Windows PC's only one,
    on 47989, whose window routes ADDRESS.token may not reach)."""
    if port:
        own = os.path.join(TOKEN_DIR, f"{address}-{port}.token")
        if os.path.exists(own):
            return own
    return os.path.join(TOKEN_DIR, f"{address}.token")


def pin_path(address, port=0):
    return token_path(address, port)[:-len(".token")] + ".pin"


def web_ui_port(address, port=0):
    """The Web UI port the token for (address, port) is for."""
    return port + 1 if token_path(address, port) != token_path(address) else WEB_UI_PORT


def _first_word(path):
    try:
        with open(path) as f:
            words = f.read().split()
    except OSError:
        return ""
    return words[0] if words else ""


def spki_pin(der):
    """A certificate's public key pin as curl takes it (CURLOPT_PINNEDPUBLICKEY)."""
    digest = hashlib.sha256(der).digest()
    if digest not in _pins:
        pem = subprocess.run(["openssl", "x509", "-inform", "der", "-pubkey", "-noout"], input=der,
                             capture_output=True, check=True).stdout
        key = subprocess.run(["openssl", "pkey", "-pubin", "-outform", "der"], input=pem,
                             capture_output=True, check=True).stdout
        _pins[digest] = "sha256//" + base64.b64encode(hashlib.sha256(key).digest()).decode()
    return _pins[digest]


def request(address, method, path, body=None, timeout=2.0, connect=None, token_address=None, port=0):
    """(HTTP status, JSON answer or None): status 0 when there's no token, no answer or a wrong
    key. connect(address, port, timeout) -> an unconnected HTTPSConnection, for tests."""
    key = token_address or address
    token = _first_word(token_path(key, port))
    if not token:
        return 0, None
    known = _first_word(pin_path(key, port))
    if connect is None:
        ctx = ssl.create_default_context()
        ctx.check_hostname, ctx.verify_mode = False, ssl.CERT_NONE  # self-signed: pinned below
        connect = lambda a, p, t: http.client.HTTPSConnection(a, p, timeout=t, context=ctx)
    conn = connect(address, web_ui_port(key, port), timeout)
    try:
        conn.connect()
        if known and spki_pin(conn.sock.getpeercert(binary_form=True)) != known:
            return 0, None
        headers = {"Authorization": f"Bearer {token}"}
        data = None
        if body is not None:
            data = json.dumps(body).encode()
            headers["Content-Type"] = "application/json"
        conn.request(method, path, body=data, headers=headers)
        reply = conn.getresponse()
        raw = reply.read(1 << 20)
        try:
            answer = json.loads(raw) if raw else None
        except ValueError:
            answer = None
        return reply.status, answer
    except (OSError, http.client.HTTPException, subprocess.CalledProcessError):
        return 0, None
    finally:
        conn.close()


def get_json(address, path, timeout=2.0, connect=None, token_address=None, port=0):
    """GET https://ADDRESS:47990PATH with the token for token_address (address by default): its
    JSON answer, or None."""
    status, answer = request(address, "GET", path, timeout=timeout, connect=connect, token_address=token_address,
                             port=port)
    return answer if status == 200 else None


def post_json(address, path, body, timeout=20.0, connect=None, token_address=None, port=0):
    """POST BODY (JSON) to PATH: (HTTP status, JSON answer or None)."""
    return request(address, "POST", path, body=body, timeout=timeout, connect=connect, token_address=token_address,
                   port=port)
