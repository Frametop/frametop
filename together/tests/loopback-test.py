#!/usr/bin/env python3
"""ft-together for real, several of it on this machine's loopback: a room opened, people let
in and kept out, the guests' own links, leaving, a program that dies, and the owner's end.

    together/tests/loopback-test.py PATH-TO-ft-together

Each ft-together runs with --loopback (no Tailscale: 127.0.0.1 is the tailnet, and everyone on
it is someone), a port of its own and a local socket of its own. The rules themselves are
tested without a network in session-test.cpp; this is what only real links show.
"""
import json
import os
import signal
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "conf"))
import ftipc  # noqa: E402

failed = 0


def check(what, ok):
    global failed
    failed += not ok
    print(("ok  " if ok else "FAIL") + "  " + what, flush=True)
    return ok


def free_port():
    """A UDP port nothing listens on right now."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class End:
    """One ft-together and the questions to it."""

    def __init__(self, program, name, port=None, sock=None):
        self.name = name
        self.port = port or free_port()
        self.sock = sock or "ft_together_test_%d_%s" % (os.getpid(), name)
        self.client = ftipc.Client(timeout=2.0)
        self.proc = subprocess.Popen([program, "--loopback", "--name", name, "--port", str(self.port), "--socket", self.sock],
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    def ask(self, text):
        return self.client.ask(self.sock, text) or "none (%s)" % self.client.why

    def state(self):
        reply = self.ask("state")
        return json.loads(reply[3:]) if reply.startswith("ok {") else {"mode": reply, "note": "", "people": [], "asks": [], "codes": 0}

    def until(self, what, seconds=5.0):
        """Whether what(state) comes true in time."""
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if what(self.state()):
                return True
            time.sleep(0.05)
        return False

    def end(self, sig=signal.SIGTERM):
        """Ends it; its exit status and what it printed."""
        if self.proc.poll() is None:
            self.proc.send_signal(sig)
        try:
            out, _ = self.proc.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            out, _ = self.proc.communicate()
            return -99, out
        return self.proc.returncode, out


def names(state):
    return sorted(p["name"] for p in state["people"])


def linked(state):
    return all(p["linked"] for p in state["people"])


def let_in(owner, guest, host="127.0.0.1"):
    """`guest` joins `owner`'s room with a new invite and is accepted."""
    code = owner.ask("invite")
    if not (code.startswith("ok ") and guest.ask("join %s %d %s" % (host, owner.port, code[3:])) == "ok"):
        return False
    if not owner.until(lambda s: any(a["name"] == guest.name for a in s["asks"])):
        return False
    ask = [a for a in owner.state()["asks"] if a["name"] == guest.name][0]
    if ask["login"] != "someone@loopback" or ask["address"] != "127.0.0.1":
        return False
    return owner.ask("accept %d" % ask["id"]) == "ok" and guest.until(lambda s: s["mode"] == "joined")


def main(program):
    ends = []

    def start(name, **kw):
        e = End(program, name, **kw)
        ends.append(e)
        return e

    ada, ben, cy = start("ada"), start("ben"), start("cy")
    try:
        ok = all(e.until(lambda s: s["mode"] == "idle") for e in (ada, ben, cy))
        if not check("three of them start, each on its own port and socket", ok):
            return
        check("nobody is in a room, and nothing can be done in one",
              ada.ask("invite").startswith("error") and ada.ask("leave").startswith("error") and ada.ask("away game").startswith("error"))
        check("nonsense is answered with what can be asked", ada.ask("dance").startswith("error state|open") and ada.ask("").startswith("error") and
              ada.ask("x" * 4000) == "error that is too long")

        check("ada opens a room", ada.ask("open 1 2 0.5") == "ok" and ada.state()["mode"] == "hosting" and names(ada.state()) == ["ada"])

        # kept out
        check("a call to an address off the tailnet isn't made", ben.ask("join 192.168.1.5 %d ABCDEFGH" % ada.port).startswith("error couldn't call") and
              ben.ask("join example.invalid %d ABCDEFGH" % ada.port).startswith("error couldn't call") and ben.state()["mode"] == "idle")
        ok = ben.ask("join 127.0.0.1 %d WRNGCODE" % ada.port) == "ok" and ben.until(lambda s: s["mode"] == "idle" and s["note"].startswith("Not let in"))
        check("a wrong invite code is refused, and its owner isn't asked", ok and ada.state()["asks"] == [])
        nobody = free_port()
        ok = ben.ask("join 127.0.0.1 %d ABCDEFGH" % nobody) == "ok" and ben.until(lambda s: s["mode"] == "idle" and s["note"] != "", 15)
        check("a call nobody answers ends by itself", ok)
        code = ada.ask("invite")[3:]
        ok = ben.ask("join 127.0.0.1 %d %s" % (ada.port, code)) == "ok" and ada.until(lambda s: len(s["asks"]) == 1)
        ok = ok and ada.ask("decline %d" % ada.state()["asks"][0]["id"]) == "ok" and ben.until(lambda s: s["mode"] == "idle" and s["note"].startswith("Not let in"))
        check("someone the owner declines isn't in", ok and names(ada.state()) == ["ada"])

        # noise on the port: not QUIC, and QUIC's first byte with rubbish after it
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            for i in range(200):
                s.sendto(bytes([0xC0 if i % 2 else i]) + os.urandom(1 + (i * 37) % 1300), ("127.0.0.1", ada.port))
        check("noise sent to the port changes nothing", ada.state()["mode"] == "hosting" and names(ada.state()) == ["ada"])

        # let in
        check("ben is let in", let_in(ada, ben) and ada.until(lambda s: names(s) == ["ada", "ben"] and linked(s)))
        check("cy too, calling by name", let_in(ada, cy, "localhost") and ada.until(lambda s: names(s) == ["ada", "ben", "cy"] and linked(s)))
        ok = ben.until(lambda s: names(s) == ["ada", "ben", "cy"] and linked(s)) and cy.until(lambda s: names(s) == ["ada", "ben", "cy"] and linked(s))
        check("the two guests link up with each other", ok)
        spots = {p["name"]: p["spot"] for p in cy.state()["people"]}
        check("everyone has the owner's room: who stands where", spots == {p["name"]: p["spot"] for p in ada.state()["people"]} and spots["ada"] == [1, 0, 2, 0.5] and
              len({tuple(v) for v in spots.values()}) == 3)
        check("a guest can't invite, and nobody can open or join a second room",
              ben.ask("invite").startswith("error") and ben.ask("open").startswith("error") and ada.ask("join 127.0.0.1 %d ABCDEFGH" % ben.port).startswith("error"))

        ok = ben.ask("away game") == "ok" and all(e.until(lambda s: [p["away"] for p in s["people"] if p["name"] == "ben"] == [1]) for e in (ada, ben, cy))
        ok = ok and ben.ask("away back") == "ok" and all(e.until(lambda s: [p["away"] for p in s["people"] if p["name"] == "ben"] == [0]) for e in (ada, ben, cy))
        check("being away is told to everyone, and being back", ok)

        # a program that dies without a word
        cy.proc.kill()
        cy.proc.wait()
        ok = ada.until(lambda s: names(s) == ["ada", "ben"], 20) and ben.until(lambda s: names(s) == ["ada", "ben"] and linked(s), 5)
        check("a guest whose program dies is out of the room once its link has been silent", ok)

        # leaving, and coming back
        ok = ben.ask("leave") == "ok" and ben.state()["mode"] == "idle" and ada.until(lambda s: names(s) == ["ada"], 3)
        check("a guest who leaves is out at once", ok)
        dee = start("dee")
        check("the room goes on: ben comes back, and someone new", let_in(ada, ben) and let_in(ada, dee) and
              all(e.until(lambda s: names(s) == ["ada", "ben", "dee"] and linked(s)) for e in (ada, ben, dee)))

        # a second program
        twin = start("twin", sock=ada.sock)
        status, out = twin.end(signal.SIGCONT)
        check("a second ft-together on the same socket doesn't start", status == 1 and "already running" in out)
        twin = start("twin", port=ben.port)
        status, out = twin.end(signal.SIGCONT)
        check("...nor one on a port that is taken", status == 1 and "taken" in out)

        # the owner's program ends
        status, out = ada.end()
        check("the owner's program ends cleanly when told to", status == 0)
        ok = all(e.until(lambda s: s["mode"] == "idle" and "owner closed it" in s["note"], 3) for e in (ben, dee))
        check("...and that ends the room for everyone, at once", ok)
        for e in (ben, dee):
            status, out = e.end()
            check("%s ends cleanly" % e.name, status == 0)
    finally:
        for e in ends:
            status, out = e.end(signal.SIGKILL)
            if failed and out:
                print("--- %s said:\n%s" % (e.name, out))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: loopback-test.py PATH-TO-ft-together")
    main(sys.argv[1])
    print("FAILED" if failed else "all ok")
    sys.exit(1 if failed else 0)
