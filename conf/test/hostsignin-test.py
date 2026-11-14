#!/usr/bin/env python3
"""Offline test of conf/ft_hostsignin.py: what each sign-in asks a host's Web UI and what it
makes of the answers. A host's displays: Vibepollo gives a token, Sunshine has none and only
checks the login, a wrong login and another certificate say so, and the dongle's address is
tried first. Its app windows: the Vibepollo on 48989 when there is one, else the one on 47989,
with the window routes in the token and each host's own words when it can't share windows.
keep and forget: the files are the ones ft_hostapi reads, readable only by the user. The
connections are stubs holding a certificate made here, so nothing reaches a PC.

  conf/test/hostsignin-test.py
"""
import base64
import json
import os
import stat
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import ft_hostapi  # noqa: E402
import ft_hostsignin  # noqa: E402

failures = []


def check(label, got, want):
    ok = got == want
    print(("ok    " if ok else "FAIL  ") + label + ("" if ok else f": got {got!r}, want {want!r}"), flush=True)
    if not ok:
        failures.append(label)


tmp = tempfile.mkdtemp(prefix="hostsignin-test-")
ft_hostapi.TOKEN_DIR = os.path.join(tmp, "hosts")


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
PIN = ft_hostapi.spki_pin(DER)
LOGIN = "Basic " + base64.b64encode(b"me:secret").decode()


class Reply:
    def __init__(self, status, body):
        self.status, self.body = status, body

    def read(self):
        return self.body


class Web:
    """A host's Web UIs: {port: {(method, path): (status, body)}}. A port that's missing
    refuses the connection; a path that's missing is a 404."""

    def __init__(self, ports, der=DER, down=()):
        self.ports, self.der, self.down, self.asked = ports, der, down, []

    def connect(self, address, port, timeout):
        return Conn(self, address, port)


class Conn:
    def __init__(self, web, address, port):
        self.web, self.address, self.port, self.sock, self.closed = web, address, port, self, False

    def connect(self):
        if self.port not in self.web.ports or self.address in self.web.down:
            raise ConnectionRefusedError("refused")

    def getpeercert(self, binary_form):
        return self.web.der

    def request(self, method, path, body=None, headers=None):
        self.web.asked.append((self.address, self.port, method, path, json.loads(body) if body else None, headers))
        self.answer = self.web.ports[self.port].get((method, path), (404, b"Not Found"))

    def getresponse(self):
        return Reply(*self.answer)

    def close(self):
        self.closed = True


def raises(fn):
    try:
        return fn()
    except ft_hostsignin.SignInError as e:
        return f"SignInError: {e}"


TOKEN = (200, b'{"token": "tok1"}')

# --- a host's displays ---
web = Web({47990: {("POST", "/api/token"): TOKEN}})
check("Vibepollo: its token and pin", ft_hostsignin.sign_in(["192.0.2.20"], "me", "secret", connect=web.connect),
      ("tok1", PIN, "vibepollo"))
check("...asked with the login, for the display scopes only", web.asked,
      [("192.0.2.20", 47990, "POST", "/api/token", {"scopes": ft_hostsignin.SCOPES},
        {"Authorization": LOGIN, "Content-Type": "application/json"})])
check("...which have no window route", [s for s in ft_hostsignin.SCOPES if "windows" in s["path"]], [])

web = Web({47990: {("POST", "/api/token"): (401, b"")}})
check("a wrong login", raises(lambda: ft_hostsignin.sign_in(["h"], "me", "x", connect=web.connect)),
      "SignInError: Wrong user name or password")
web = Web({47990: {("POST", "/api/token"): (500, b"oops")}})
check("no token made", raises(lambda: ft_hostsignin.sign_in(["h"], "me", "x", connect=web.connect)),
      "SignInError: It didn't make a token (500 oops)")

web = Web({47990: {("POST", "/api/token"): TOKEN}}, der=OTHER)
check("another certificate than the pinned one: refused",
      raises(lambda: ft_hostsignin.sign_in(["h"], "me", "secret", known=PIN, connect=web.connect))[:45],
      "SignInError: Its certificate isn't the one se")
check("...before the login is sent", web.asked, [])

sunshine = {("POST", "/api/token"): (400, b'{"error": "Bad Request"}'), ("GET", "/api/clients/list"): (200, b"{}")}
web = Web({47990: sunshine})
check("Sunshine (a bare 400): no token, the login checked",
      ft_hostsignin.sign_in(["h"], "me", "secret", connect=web.connect), ("", PIN, "sunshine"))
check("...by listing clients with it", web.asked[-1][2:], ("GET", "/api/clients/list", None, {"Authorization": LOGIN}))
web = Web({47990: {("GET", "/api/clients/list"): (401, b"")}})
check("Sunshine (a 404), wrong login", raises(lambda: ft_hostsignin.sign_in(["h"], "me", "x", connect=web.connect)),
      "SignInError: Wrong user name or password")
web = Web({47990: {("POST", "/api/token"): (400, b'{"error": "scopes"}')}})
check("another 400 isn't Sunshine", raises(lambda: ft_hostsignin.sign_in(["h"], "me", "x", connect=web.connect))[:37],
      "SignInError: It didn't make a token (")

web = Web({47990: {("POST", "/api/token"): TOKEN}}, down=["10.35.78.20"])
check("the dongle doesn't answer: the network does",
      ft_hostsignin.sign_in(["10.35.78.20", "192.0.2.20"], "me", "secret", connect=web.connect)[0], "tok1")
check("...asked there only", [a[0] for a in web.asked], ["192.0.2.20"])
web = Web({47990: {("POST", "/api/token"): TOKEN}})
ft_hostsignin.sign_in(["10.35.78.20", "192.0.2.20"], "me", "secret", connect=web.connect)
check("the dongle answers: the login goes over it only", [a[0] for a in web.asked], ["10.35.78.20"])
check("nothing answers", raises(lambda: ft_hostsignin.sign_in(["h"], "me", "x", connect=Web({}).connect)),
      "SignInError: Couldn't reach it: refused")

# --- a host's app windows ---
windows = {("GET", "/api/frametop/windows"): (200, b'{"windows": []}'), ("POST", "/api/token"): TOKEN}
machine = {("GET", "/api/frametop/windows"): (503, b'{"error": "Run vibepollo-frametop-windows setup"}'),
           ("POST", "/api/token"): TOKEN}

web = Web({48990: windows, 47990: machine})
check("a Linux PC: the Vibepollo on 48989", ft_hostsignin.app_windows(["h"], "me", "secret", connect=web.connect),
      (48989, "tok1", PIN))
check("...asked for its windows with the login, then for a token with the window routes", [a[1:5] for a in web.asked],
      [(48990, "GET", "/api/frametop/windows", None),
       (48990, "POST", "/api/token", {"scopes": ft_hostsignin.WINDOW_SCOPES})])
check("...which keep the display scopes, for each window's client",
      [s for s in ft_hostsignin.SCOPES if s not in ft_hostsignin.WINDOW_SCOPES], [])
check("...and add every window route ft-layout and ft-stream ask",
      sorted(s["path"][len("/api/frametop/windows"):] for s in ft_hostsignin.WINDOW_SCOPES if "windows" in s["path"]),
      ["", "/apps", "/at", "/open", "/release", "/resize"])

web = Web({47990: windows})
check("a Windows PC: its only Vibepollo, on 47989",
      ft_hostsignin.app_windows(["h"], "me", "secret", connect=web.connect), (47989, "tok1", PIN))
web = Web({47990: machine})
check("a Linux PC with no Vibepollo in its session: the host's own words",
      raises(lambda: ft_hostsignin.app_windows(["h"], "me", "secret", connect=web.connect)),
      "SignInError: Run vibepollo-frametop-windows setup")
check("...and no token was made", [a[2:4] for a in web.asked], [("GET", "/api/frametop/windows")])
web = Web({47990: {("POST", "/api/token"): TOKEN}})
check("a Vibepollo without the window routes",
      raises(lambda: ft_hostsignin.app_windows(["h"], "me", "secret", connect=web.connect))[:46],
      "SignInError: Its Vibepollo has no app windows.")
web = Web({48990: {("GET", "/api/frametop/windows"): (401, b"")}, 47990: windows})
check("a wrong login for the one on 48989 says which login",
      raises(lambda: ft_hostsignin.app_windows(["h"], "me", "x", connect=web.connect))[:58],
      "SignInError: Wrong user name or password for its app windo")
web = Web({47990: {("GET", "/api/frametop/windows"): (401, b"")}})
check("a wrong login for the one on 47989",
      raises(lambda: ft_hostsignin.app_windows(["h"], "me", "x", connect=web.connect)),
      "SignInError: Wrong user name or password")

seen = []
web = Web({48990: windows}, der=OTHER)


def known(port):
    seen.append(port)
    return PIN


check("each port's pin is asked for, and another certificate refused",
      (raises(lambda: ft_hostsignin.app_windows(["h"], "me", "secret", known, connect=web.connect))[:30], seen, web.asked),
      ("SignInError: Its certificate i", [48989], []))
web = Web({48990: windows}, down=["10.35.78.20"])
check("the dongle first, then the network",
      (ft_hostsignin.app_windows(["10.35.78.20", "h"], "me", "secret", connect=web.connect)[0], web.asked[0][0]),
      (48989, "h"))
check("nothing answers", raises(lambda: ft_hostsignin.app_windows(["h"], "me", "x", connect=Web({}).connect)),
      "SignInError: Couldn't reach it: refused")


# --- what's kept ---
def kept():
    out = {}
    for name in sorted(os.listdir(ft_hostapi.TOKEN_DIR)):
        path = os.path.join(ft_hostapi.TOKEN_DIR, name)
        with open(path) as f:
            out[name] = (f.read(), stat.S_IMODE(os.stat(path).st_mode))
    return out


ft_hostsignin.keep("192.0.2.20", "tok1", PIN)
check("a host's token and pin, readable only by the user", kept(),
      {"192.0.2.20.token": ("tok1\n", 0o600), "192.0.2.20.pin": (PIN + "\n", 0o600)})
check("...in a folder only the user opens", stat.S_IMODE(os.stat(ft_hostapi.TOKEN_DIR).st_mode), 0o700)
ft_hostsignin.keep("192.0.2.20", "win2", PIN, 48989)
check("its app windows' own, by port", sorted(kept()),
      ["192.0.2.20-48989.pin", "192.0.2.20-48989.token", "192.0.2.20.pin", "192.0.2.20.token"])
check("...which is the one ft_hostapi asks that port with",
      (ft_hostapi.first_word(ft_hostapi.token_path("192.0.2.20", 48989)), ft_hostapi.web_ui_port("192.0.2.20", 48989),
       ft_hostapi.first_word(ft_hostapi.pin_path("192.0.2.20", 48989))), ("win2", 48990, PIN))
ft_hostsignin.keep("192.0.2.21", "", PIN)
check("Sunshine: the pin alone", sorted(n for n in kept() if n.startswith("192.0.2.21")), ["192.0.2.21.pin"])
ft_hostsignin.keep("192.0.2.2", "other", PIN)
ft_hostsignin.forget("192.0.2.20")
check("forgetting a host removes all of its files and nobody else's", sorted(kept()),
      ["192.0.2.2.pin", "192.0.2.2.token", "192.0.2.21.pin"])
ft_hostsignin.forget("192.0.2.20")  # again: nothing to remove, no error

for name in os.listdir(ft_hostapi.TOKEN_DIR):
    os.unlink(os.path.join(ft_hostapi.TOKEN_DIR, name))
os.rmdir(ft_hostapi.TOKEN_DIR)
os.rmdir(tmp)
print(f"{len(failures)} failed" if failures else "all passed")
sys.exit(1 if failures else 0)
