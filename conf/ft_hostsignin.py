"""ft_hostsignin: signing in to a PC's Web UI for the API token Frametop keeps (Remote Displays).

Vibepollo makes a token for its Web UI login that can do only what's listed (SCOPES), so the
password is used once and never kept. Two sign-ins:

  sign_in      a host's displays: the token ft-stream pairs them with, on Web UI port 47990.
               Plain Sunshine (a Mac) has no tokens; then the login is only checked.
  app_windows  a host's app windows (docs/remote-displays.md, "PC app windows"): which Vibepollo
               on it shares them, and a token of its own for that one, with the window routes
               too (WINDOW_SCOPES). A Linux PC shares them from a second Vibepollo in its
               Hyprland session, on 48989; a Windows PC from its only one, on 47989.

keep() saves what a sign-in answered where ft-stream and ft_hostapi read it, readable only by
the user, and forget() removes all that's kept for an address.

Each sign-in tries the addresses in order (a host's dongle first, so the password skips the router) and
answers with the token and the Web UI certificate's public key pin (ft_hostapi.spki_pin), or
raises SignInError with what to tell the user. The certificate is self-signed: with a pin from
an earlier sign-in, another certificate is refused before the login is sent.

connect(address, port, timeout) -> an unconnected HTTPSConnection, for tests.
"""
import base64
import http.client
import json
import os
import ssl
import subprocess

import ft_hostapi

# What the token for a host's displays may do: pair their clients (submit their PINs), list
# the clients and set their permissions, list the host's monitors, and turn them on or off and
# change their modes while Frametop streams (Frametop's build; remote-displays/host_monitors.py).
SCOPES = [{"path": "/api/pin", "methods": ["POST"]}, {"path": "/api/clients/list", "methods": ["GET"]},
          {"path": "/api/clients/update", "methods": ["POST"]}, {"path": "/api/display-devices", "methods": ["GET"]},
          {"path": "/api/frametop/monitors", "methods": ["GET", "POST"]}]
# ... and the one for its app windows: the same for each window's client, and the window routes
# ft-layout and ft-stream ask (list the windows and apps, open or launch, resize, which window
# is at a point, put one back).
WINDOW_SCOPES = SCOPES + [
    {"path": "/api/frametop/windows", "methods": ["GET"]}, {"path": "/api/frametop/windows/apps", "methods": ["GET"]},
    {"path": "/api/frametop/windows/open", "methods": ["POST"]}, {"path": "/api/frametop/windows/resize", "methods": ["POST"]},
    {"path": "/api/frametop/windows/at", "methods": ["POST"]}, {"path": "/api/frametop/windows/release", "methods": ["POST"]}]
# The HTTP ports a host's app windows may be on, the one that's only ever for them first. Each
# one's Web UI is on the port after it.
WINDOW_PORTS = (48989, ft_hostapi.GAMESTREAM_PORT)

WRONG_LOGIN = "Wrong user name or password"
# What a host that can't be reached raises; anything it answers is a SignInError instead.
_NO_ANSWER = (OSError, ValueError, http.client.HTTPException, subprocess.SubprocessError)


class SignInError(Exception):
    """A sign-in that didn't work: str() is what to tell the user."""


def _connect(address, port, timeout):
    ctx = ssl.create_default_context()
    ctx.check_hostname, ctx.verify_mode = False, ssl.CERT_NONE  # self-signed: pinned in _open
    return http.client.HTTPSConnection(address, port, timeout=timeout, context=ctx)


def _open(connect, address, port, known, timeout):
    """(a connected connection, its certificate's pin). `known`: the pin it must have."""
    conn = connect(address, port, timeout)
    try:
        conn.connect()
        pin = ft_hostapi.spki_pin(conn.sock.getpeercert(binary_form=True))
        if known and pin != known:
            raise SignInError("Its certificate isn't the one seen when it was added. If Vibepollo or Sunshine was "
                              "installed again, remove the host and add it again.")
    except BaseException:
        conn.close()
        raise
    return conn, pin


def _basic(user, password):
    return {"Authorization": "Basic " + base64.b64encode(f"{user}:{password}".encode()).decode()}


# More than any sign-in answer: a host can't make Frametop read without end.
MOST_ANSWER = 1 << 20


def _ask(conn, method, path, login, body=None):
    """(HTTP status, the answer's text, its first MOST_ANSWER bytes)."""
    headers = dict(login)
    if body is not None:
        headers["Content-Type"] = "application/json"
    conn.request(method, path, json.dumps(body) if body is not None else None, headers)
    reply = conn.getresponse()
    return reply.status, reply.read(MOST_ANSWER).decode(errors="replace")


def _error(text):
    """The "error" a Vibepollo answer gives, or ""."""
    try:
        why = json.loads(text).get("error")
    except (ValueError, AttributeError):
        return ""
    return why if isinstance(why, str) else ""


def _token(conn, login, scopes):
    """A new token for the login, from POST /api/token's answer."""
    status, text = _ask(conn, "POST", "/api/token", login, {"scopes": scopes})
    return _made(status, text)


def _made(status, text):
    if status == 401:
        raise SignInError(WRONG_LOGIN)
    token = json.loads(text).get("token") if status == 200 else None
    if not token:
        raise SignInError(f"It didn't make a token ({status} {text[:120]})")
    return token


def sign_in(addresses, user, password, known="", connect=_connect):
    """(token, pin, kind) from the first address that answers: kind "vibepollo", or "sunshine"
    with no token (the login worked there: it lists clients)."""
    login, error = _basic(user, password), ""
    for address in addresses:
        try:
            conn, pin = _open(connect, address, ft_hostapi.WEB_UI_PORT, known, 15)
            try:
                status, text = _ask(conn, "POST", "/api/token", login, {"scopes": SCOPES})
                # No tokens: plain Sunshine, which answers a POST to a path it doesn't have with a
                # bare 400 "Bad Request", before any login check (404 elsewhere).
                if status == 404 or (status == 400 and _error(text) == "Bad Request"):
                    status, _ = _ask(conn, "GET", "/api/clients/list", login)
                    if status == 401:
                        raise SignInError(WRONG_LOGIN)
                    if status != 200:
                        raise SignInError(f"It didn't take the login ({status})")
                    return "", pin, "sunshine"
                return _made(status, text), pin, "vibepollo"
            finally:
                conn.close()
        except _NO_ANSWER as e:
            error = str(e) or type(e).__name__
    raise SignInError(f"Couldn't reach it: {error}")


def app_windows(addresses, user, password, known=lambda port: "", connect=_connect):
    """(port, token, pin): the HTTP port of the Vibepollo on the host that shares its app
    windows, and a token for it. known(port): the pin that port's Web UI must have, or ""."""
    login, error = _basic(user, password), ""
    for port in WINDOW_PORTS:
        own = port != ft_hostapi.GAMESTREAM_PORT
        for address in addresses:
            try:
                conn, pin = _open(connect, address, port + 1, known(port), 8)
                try:
                    status, text = _ask(conn, "GET", "/api/frametop/windows", login)
                    if status == 401:
                        raise SignInError(WRONG_LOGIN + (" for its app windows: on a Linux PC that's the login "
                                                         "made with vibepollo-frametop-windows setup" if own else ""))
                    if status == 404:
                        raise SignInError("Its Vibepollo has no app windows. They need Frametop's build of "
                                          "Vibepollo, a newer one than it has.")
                    if status != 200:
                        raise SignInError(_error(text) or f"It answered {status}")
                    return port, _token(conn, login, WINDOW_SCOPES), pin
                finally:
                    conn.close()
            except _NO_ANSWER as e:
                error = str(e) or type(e).__name__
    raise SignInError(f"Couldn't reach it: {error}")


def _write_private(path, text):
    # The modes are set again: makedirs and O_CREAT leave a folder or file that's already there as it was.
    os.makedirs(ft_hostapi.TOKEN_DIR, mode=0o700, exist_ok=True)
    os.chmod(ft_hostapi.TOKEN_DIR, 0o700)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
    os.fchmod(fd, 0o600)
    with os.fdopen(fd, "w") as f:
        f.write(text + "\n")


def keep(address, token, pin, port=0):
    """Saves a sign-in's token ("" when there's none: Sunshine) and pin: ADDRESS.token and
    ADDRESS.pin, or with `port` that instance's own, ADDRESS-PORT.token and ADDRESS-PORT.pin."""
    path = ft_hostapi.own_token_path(address, port) if port else ft_hostapi.token_path(address)
    if token:
        _write_private(path, token)
    _write_private(path[:-len(".token")] + ".pin", pin)


def forget(address, ports=WINDOW_PORTS):
    """Removes the token, pin and Screen Sharing login kept for the address, and its instances' own on `ports`."""
    paths = [ft_hostapi.token_path(address)] + [ft_hostapi.own_token_path(address, p) for p in ports]
    for path in paths + [p[:-len(".token")] + ".pin" for p in paths] + [ft_hostapi.apple_path(address)]:
        try:
            os.remove(path)
        except OSError:
            pass
