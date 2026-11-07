"""ft_hostapi: questions to a PC's Vibepollo Web UI API, as ft-stream asks them (stream/host.cpp).

A host from Remote Displays has an API token in ~/.local/share/frametop-stream/hosts/ADDRESS.token
and, when it was signed in from there, the public key pin its certificate had then in ADDRESS.pin
(curl's "sha256//BASE64" form). The certificate is self-signed, so it isn't checked against a CA:
a pin there must match, or the answer isn't read. A host without a pin file (one paired with
make-frametop-token) takes any certificate, as ft-stream does.

get_json(address, path) is one GET with the token, its JSON answer or None (no token, no answer,
a wrong key, not JSON). Standard library plus the openssl command for the pin; a pin is worked out
once per certificate.
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


def token_path(address):
    return os.path.join(TOKEN_DIR, f"{address}.token")


def pin_path(address):
    return os.path.join(TOKEN_DIR, f"{address}.pin")


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


def get_json(address, path, timeout=2.0, connect=None, token_address=None):
    """GET https://ADDRESS:47990PATH with the token for token_address (address by default), or
    None. connect(address, timeout) -> an unconnected HTTPSConnection, for tests."""
    key = token_address or address
    token = _first_word(token_path(key))
    if not token:
        return None
    known = _first_word(pin_path(key))
    if connect is None:
        ctx = ssl.create_default_context()
        ctx.check_hostname, ctx.verify_mode = False, ssl.CERT_NONE  # self-signed: pinned below
        connect = lambda a, t: http.client.HTTPSConnection(a, WEB_UI_PORT, timeout=t, context=ctx)
    conn = connect(address, timeout)
    try:
        conn.connect()
        if known and spki_pin(conn.sock.getpeercert(binary_form=True)) != known:
            return None
        conn.request("GET", path, headers={"Authorization": f"Bearer {token}"})
        reply = conn.getresponse()
        body = reply.read(65536)
        if reply.status != 200:
            return None
        return json.loads(body)
    except (OSError, ValueError, http.client.HTTPException, subprocess.CalledProcessError):
        return None
    finally:
        conn.close()
