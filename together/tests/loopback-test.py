#!/usr/bin/env python3
"""ft-together for real, several of it on this machine's loopback: a room opened, people let
in and kept out, the guests' own links, poses passed between them, each one's floor, the avatars
they have on and the files of those, the panels they share and the video of one from the program
that encodes it to the programs that show it, leaving, a program that dies, and the owner's end.

    together/tests/loopback-test.py PATH-TO-ft-together

Each ft-together runs with --loopback (no Tailscale: 127.0.0.1 is the tailnet, and everyone on
it is someone), a port of its own, a local socket of its own, and folders of its own for the
wearer's avatars and for the session's files. The rules themselves are
tested without a network in session-test.cpp; this is what only real links show.
"""
import json
import os
import shutil
import signal
import socket
import stat
import struct
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import roomkit  # noqa: E402  (ft-together on the loopback, the questions to it, the tally)
from roomkit import End, Program, Watcher, check, free_port, let_in, linked, model, names, pose, voice  # noqa: E402

ftipc = roomkit.ftipc


def crosses(source, sinks, units, tries=4):
    """Whether pictures handed to `source` (a keyframe first) come out of every one of `sinks`,
    in order and byte for byte. A link gives a picture up when it is 150 ms late, which a busy
    machine can make happen: then the whole run is tried again."""
    want = [(b"U", bytes([0 if i else 1]) + u) for i, u in enumerate(units)]
    for _ in range(tries):
        for i, u in enumerate(units):
            source.unit(u, key=i == 0)
            time.sleep(0.005)
        got = [sink.messages(5, len(want)) for sink in sinks]
        if all(g == want for g in got):
            return True
        time.sleep(0.7)   # (a keyframe may be asked for again by then)
        source.messages(0.1)
        for sink in sinks:
            sink.messages(0.3)
    return False


class Home:
    """One Frame's folders: its wearer's avatars, and where the session's files are kept."""

    def __init__(self, root, name):
        self.avatars, self.run = os.path.join(root, name, "avatars"), os.path.join(root, name, "run")
        os.makedirs(self.avatars)
        os.makedirs(self.run, mode=0o700)
        self.env = dict(os.environ, FT_AVATARS_DIR=self.avatars, XDG_RUNTIME_DIR=self.run)

    def put(self, name, may_be_sent, filler=b""):
        """A model in the wearer's folder; its bytes."""
        data = model(name, may_be_sent, filler)
        with open(os.path.join(self.avatars, name + ".ftmodel"), "wb") as f:
            f.write(data)
        return data

    def kept(self):
        """The files kept for the session: {path: bytes}."""
        found = {}
        for folder, _, files in os.walk(self.run):
            for name in files:
                try:
                    with open(os.path.join(folder, name), "rb") as f:
                        found[os.path.join(folder, name)] = f.read()
                except OSError:
                    pass   # (gone between the listing and the read)
        return found


def looks(end):
    reply = end.ask("looks")
    return json.loads(reply[3:]) if reply.startswith("ok {") else {"wear": reply, "sent": False, "why": "", "people": []}


def files(end):
    """Whose avatars `end` knows of, and where the file of each is: {person: path, "" until it came}."""
    return {p["id"]: p["file"] for p in looks(end)["people"]}


def comes(end, home, who, data, seconds=15.0):
    """Whether `end` comes to have the file of the avatar `who` has on: `data`, in a folder and a
    file that only this user reads."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        path = files(end).get(who)
        if path and home.kept().get(path) == data:
            return stat.S_IMODE(os.stat(path).st_mode) == 0o600 and stat.S_IMODE(os.stat(os.path.dirname(path)).st_mode) == 0o700
        time.sleep(0.1)
    return False


def main(program, root):
    ends = []
    home = {}

    def start(name, **kw):
        if name not in home:
            home[name] = Home(root, name)
        e = End(program, name, env=home[name].env, **kw)
        ends.append(e)
        return e

    ada, ben, cy = start("ada"), start("ben"), start("cy")
    try:
        ok = all(e.until(lambda s: s["mode"] == "idle") for e in (ada, ben, cy))
        if not check("three of them start, each on its own port and socket", ok):
            return
        check("each says where the others call it", ada.ask("here") == "ok 127.0.0.1 %d ada" % ada.port and ben.ask("here") == "ok 127.0.0.1 %d ben" % ben.port)
        check("nobody is in a room, and nothing can be done in one",
              ada.ask("invite").startswith("error") and ada.ask("leave").startswith("error") and ada.ask("away asleep").startswith("error"))
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

        # poses: from one Frame's program to the others' watchers
        ids = {p["name"]: p["id"] for p in ada.state()["people"]}
        mine, sees, cys = Watcher(ada), Watcher(ben), Watcher(cy)
        check("a program asks to watch, as often as it likes", sees.ask("watch") == "ok" and cys.ask("watch") == "ok" and sees.ask("watch") == "ok")
        mine.send(pose(0.25))
        got = sees.poses()
        check("a pose handed to ft-together reaches the others' watchers, and says whose it is",
              len(got) == 1 and got[0][0] == ids["ada"] and got[0][1][3:] == pose(0.25)[3:] and [p for p, _ in cys.poses()] == [ids["ada"]])
        for i in range(5):
            mine.send(pose(0.5 + i, turn=7))   # whatever count the program put in, ft-together's own goes out
        got = sees.poses()
        counts = [struct.unpack_from("<H", data, 1)[0] for _, data in got]
        check("ft-together counts them itself, so each is newer than the last", len(got) == 5 and len(cys.poses(0.2)) == 5 and counts == sorted(set(counts)) and
              [round(struct.unpack_from("<f", data, 3)[0], 2) for _, data in got] == [0.5, 1.5, 2.5, 3.5, 4.5])
        mine.send(pose(1e9))
        mine.send(pose(0.25, eye_height=9))
        mine.send(pose(0.25)[:-3])
        mine.send(b"\x01")
        check("what isn't a pose goes nowhere", sees.poses(0.3) == [])
        check("nobody is told of their own pose, and someone who doesn't watch gets none", mine.poses(0.2) == [])
        ok = mine.ask("watch") == "ok"
        sees.send(pose(-1.0))
        check("a guest's pose goes to the owner and the other guest alike", ok and [p for p, _ in mine.poses()] == [ids["ben"]] and
              [p for p, _ in cys.poses(0.2)] == [ids["ben"]])

        # the wearer's mouth: told by the program that hears them, sent in their poses
        def mouth_of(got):
            return [bytes(data[26:31]) for _, data in got]
        mine.send(b"\x83" + bytes([200, 0, 30, 4, 255]))
        mine.send(pose(0.25))
        check("the wearer's mouth, told by one of their Frame's programs, goes out in their poses", mouth_of(sees.poses()) == [bytes([200, 0, 30, 4, 255])])
        for bad in (b"\x83" + bytes([9, 8, 7, 6, 5]), b"\x83", b"\x83" + bytes(4), b"\x83" + bytes(6)):
            mine.send(bad)
        mine.send(pose(0.25))
        check("what isn't a mouth changes nothing", mouth_of(sees.poses()) == [bytes([9, 8, 7, 6, 5])])
        time.sleep(0.2)   # (with the half second that took: longer than a mouth is held)
        mine.send(pose(0.25))
        check("a mouth that isn't told again shuts", mouth_of(sees.poses()) == [bytes(5)])
        sees.send(b"\x83" + bytes([1, 2, 3, 4, 5]))
        mine.send(pose(0.25))
        got = mouth_of(sees.poses())
        check("...and nobody's mouth is anyone else's", got == [bytes(5)])
        if got != [bytes(5)]:
            print(got)

        # voices: the same way, to whoever listens
        check("a program asks to listen", sees.ask("listen") == "ok" and mine.ask("listen") == "ok")
        for count in (65534, 65535, 0, 40):   # a gap is silence, and the count goes round
            mine.send(voice(count))
        got = sees.voices()
        check("a voice handed to ft-together reaches the others' listeners as it was, whose it is said",
              got == [(ids["ada"], voice(c)) for c in (65534, 65535, 0, 40)] and sees.poses(0.1) == [])
        mine.send(voice(39))
        mine.send(voice(41, 0))
        mine.send(voice(42, 401))
        mine.send(voice(43, 400))
        check("what came late or isn't a voice goes nowhere", sees.voices(0.3) == [(ids["ada"], voice(43, 400))])
        check("nobody hears themselves, and someone who doesn't listen gets none", mine.voices(0.2) == [] and cys.voices(0.1) == [])
        sees.send(voice(7))
        check("a guest's voice goes to the owner", [p for p, _ in mine.voices()] == [ids["ben"]])

        # the floor: where each one's own floor lies in the room
        check("no floor is laid until a program says where", mine.ask("floor") == "ok none" and mine.ask("floor 1 2").startswith("error floor") and
              mine.ask("floor 1 2 nan").startswith("error floor") and mine.ask("floor 5000 0 0").startswith("error floor") and mine.ask("floor") == "ok none")
        check("the first one said stands, whoever asks", mine.ask("floor 1.5 -2 0.25") == "ok 1.500000 -2.000000 0.250000" and
              mine.ask("floor 9 9 1") == "ok 1.500000 -2.000000 0.250000" and ada.ask("floor") == "ok 1.500000 -2.000000 0.250000")
        check("each Frame has its own", sees.ask("floor") == "ok none" and sees.ask("floor 0 0 3") == "ok 0.000000 0.000000 3.000000")

        ok = ben.ask("away game") == "ok" and all(e.until(lambda s: [p["away"] for p in s["people"] if p["name"] == "ben"] == [1]) for e in (ada, ben, cy))
        ok = ok and ben.ask("away back") == "ok" and all(e.until(lambda s: [p["away"] for p in s["people"] if p["name"] == "ben"] == [0]) for e in (ada, ben, cy))
        check("being away is told to everyone, and being back", ok)

        # the avatars they have on: the room hears which, and the file goes to whoever asks its wearer
        fox, wolf = home["ada"].put("fox", 1, os.urandom(300000)), home["ada"].put("wolf", 1, os.urandom(2 << 20))
        home["ada"].put("secret", 0, os.urandom(1000))
        owl = home["cy"].put("owl", 1, os.urandom(40000))
        check("nobody has an avatar on, and nothing is kept", all(looks(e) == {"wear": "", "sent": False, "why": "", "people": []} for e in (ada, ben, cy)) and
              all(home[n].kept() == {} for n in ("ada", "ben", "cy")))
        ok = ada.ask("wear fox") == "ok" and looks(ada)["wear"] == "fox" and looks(ada)["sent"] is True
        check("the owner puts one on that the others may have: its file comes to both guests, whole, where only they can read it",
              ok and comes(ben, home["ben"], ids["ada"], fox) and comes(cy, home["cy"], ids["ada"], fox) and home["ada"].kept() == {})
        ok = cy.ask("wear owl") == "ok" and comes(ada, home["ada"], ids["cy"], owl) and comes(ben, home["ben"], ids["cy"], owl)
        check("a guest's goes to the owner and to the other guest, each from its wearer", ok and len(home["ben"].kept()) == 2 and len(home["cy"].kept()) == 1)
        ok = ada.ask("wear secret").startswith("error that avatar isn't marked") and looks(ada)["wear"] == "secret" and looks(ada)["sent"] is False
        ok = ok and ben.until(lambda s: list(files(ben)) == [ids["cy"]] and list(home["ben"].kept().values()) == [owl], 10)
        check("one that isn't marked as one the others may have is worn in private: the room hears of none, and the file it had goes", ok)
        marked = home["ada"].put("secret", 1, os.urandom(1000))
        ok = ada.ask("wear secret") == "ok" and looks(ada)["sent"] is True and comes(ben, home["ben"], ids["ada"], marked) and comes(cy, home["cy"], ids["ada"], marked)
        check("once its owner marks the file (Settings asks first), saying it again sends it", ok)
        ok = ada.ask("wear wolf") == "ok" and comes(ben, home["ben"], ids["ada"], wolf, 30) and comes(cy, home["cy"], ids["ada"], wolf, 30)
        check("another, of two megabytes, crosses whole", ok)
        check("no avatar of that name, or a name that would lead out of the folder, is none",
              ben.ask("wear nobody").startswith("error there is no avatar") and ben.ask("wear ../../ada/avatars/fox").startswith("error there is no avatar") and looks(ben)["wear"] == "")
        ok = cy.ask("wear") == "ok" and ada.until(lambda s: files(ada) == {} and home["ada"].kept() == {}, 10)
        ok = ok and ben.until(lambda s: list(files(ben)) == [ids["ada"]] and list(home["ben"].kept().values()) == [wolf], 10)
        check("taken off, it is nobody's: the files of it go", ok)

        # the panels people share
        screen = "1920 1080 0 1.5 -2 1 0 0 0 2 0"
        ok = ada.ask("share 0 %s Screen 1" % screen) == "ok"
        ok = ok and all(e.until(lambda s: [(p["id"], p["owner"], p["name"]) for p in s["panels"]] == [(0, ids["ada"], "Screen 1")]) for e in (ada, ben, cy))
        check("a panel the owner shares is in everyone's room", ok)
        bens = ids["ben"] * 8 + 1
        ok = ben.ask("share 1 1280 720 1 1 1 1 0 0 0 1.2 1.8 ben's") == "ok"
        ok = ok and all(e.until(lambda s: sorted(p["id"] for p in s["panels"]) == [0, bens]) for e in (ada, ben, cy))
        check("...and a guest's, which goes through the owner", ok and [p["curve"] for p in cy.state()["panels"] if p["id"] == bens] == [1.8])

        # its video: from the program that encodes it, over the links, to the programs that show it
        src = Program(ada, b"S", 0)
        got = src.messages(5, 2)   # (it comes at once: the seconds are for a Frame that is busy)
        if sorted(got) != [(b"R", struct.pack("<I", 2000000)), (b"W", b"\0")]:
            print("      got:", got)
        check("a program that encodes a panel is told the bitrate, and that nobody watches", sorted(got) == [(b"R", struct.pack("<I", 2000000)), (b"W", b"\0")])
        src.ignore = b"R"   # (from here on the bitrate follows how the pictures fare, which isn't this test's to say)
        view = Program(ben, b"V", 0)
        got = src.messages(5, 2)
        if sorted(got) != [(b"K", b""), (b"W", b"\1")]:
            print("      got:", got)
        check("someone's program shows the panel: the encoder hears it is watched, and is asked for a keyframe", sorted(got) == [(b"K", b""), (b"W", b"\1")])
        check("its pictures cross the link and come out in order, byte for byte", crosses(src, [view], [os.urandom(20000)] + [os.urandom(1500 + i) for i in range(30)]))
        src.messages(0.2)
        late = Program(cy, b"V", 0)
        ok = src.messages(2, 1) == [(b"K", b"")]
        delta = os.urandom(900)
        src.unit(delta)
        check("a second viewer waits for a keyframe, which is asked for, while the first goes on", ok and view.messages(2, 1) == [(b"U", b"\0" + delta)] and late.messages(0.3) == [])
        check("...and from the keyframe on both get every picture", crosses(src, [view, late], [os.urandom(30000)] + [os.urandom(2000) for i in range(10)]))
        check("a picture of a megabyte crosses whole", crosses(src, [view, late], [os.urandom(1 << 20)]))
        media = json.loads(ada.ask("media")[3:]), json.loads(ben.ask("media")[3:])
        check("ft-together says which panels have a program, and who watches", media[0]["sources"] == [0] and media[0]["viewers"] == [2, 0, 0] and media[0]["sinks"] == [] and
              media[1]["sinks"] == [0] and media[1]["sources"] == [] and media[1]["dropped"] == 0 and len(media[0]["bitrate"]) == 3)

        src.messages(0.2)
        view.send(b"A", b"\0\0")
        late.close()
        ok = src.messages(3, 1) == [(b"W", b"\0")]
        src.unit(os.urandom(5000), key=True)
        check("one hides the panel and the other's program goes: the encoder hears nobody watches, and nothing is sent", ok and view.messages(0.5) == [])
        view.send(b"A", b"\2\0")
        check("looking again: watched, and a keyframe is asked for", sorted(src.messages(2, 2)) == [(b"K", b""), (b"W", b"\1")])

        # a program that doesn't take its pictures
        lazy = Program(cy, b"V", 0)
        src.messages(1.5)
        for i in range(12):
            src.unit(os.urandom(1 << 20), key=True)
            view.messages(0.3)
        check("a program that doesn't take its pictures is let go, and the others still get theirs", lazy.let_go(10) and crosses(src, [view], [os.urandom(4000), os.urandom(400)]))

        # programs that say what they may not
        twin = Program(ada, b"S", 0)
        check("a second program for the same panel takes the first one's place", src.let_go() and sorted(kind for kind, _ in twin.messages(1, 2)) == [b"R", b"W"])
        twin.ignore = b"R"
        wrong = [Program(ada, b"S", 3), Program(ada, b"V", 32), Program(ada, b"X", 0), Program(ben), Program(ben), Program(ben, b"V", bens), Program(ada, b"S", 1)]
        wrong[3].sock.sendall(bytes(4))                               # a message of no length
        wrong[4].sock.sendall(struct.pack("<I", (1 << 20) + 3))       # ...and one longer than any
        wrong[5].unit(b"a picture, from a program that shows them")
        wrong[6].send(b"A", b"\1\0")
        check("a program that says what it may not is let go", all(w.let_go() for w in wrong))
        quiet = Program(ada)
        check("...and one that doesn't say what it is, after five seconds", quiet.messages(3) == [] and quiet.let_go(4))
        check("the others are still there", crosses(twin, [view], [os.urandom(4000), os.urandom(400)]) and json.loads(ada.ask("media")[3:])["sources"] == [0])

        ok = ada.ask("unshare 0") == "ok" and all(e.until(lambda s: [p["id"] for p in s["panels"]] == [bens]) for e in (ada, ben, cy))
        twin.messages(0.5)
        twin.unit(os.urandom(3000), key=True)
        check("a panel taken back is out of everyone's room, and its pictures go nowhere", ok and view.messages(0.5) == [])
        for one in [twin, view, lazy, quiet] + wrong:
            one.close()

        # a program that dies without a word
        cy.proc.kill()
        cy.proc.wait()
        ok = ada.until(lambda s: names(s) == ["ada", "ben"], 20) and ben.until(lambda s: names(s) == ["ada", "ben"] and linked(s), 5)
        check("a guest whose program dies is out of the room once its link has been silent", ok)

        # leaving, and coming back
        ok = ben.ask("leave") == "ok" and ben.state()["mode"] == "idle" and ada.until(lambda s: names(s) == ["ada"], 3)
        check("a guest who leaves is out at once, and their panel with them", ok and ada.state()["panels"] == [])
        check("...and the files they were sent are gone", ben.until(lambda s: home["ben"].kept() == {} and files(ben) == {}, 5))
        sees.send(pose(-1.0))
        check("...and their pose goes nowhere, and their floor is lifted", mine.poses(0.3) == [] and sees.ask("floor").startswith("error you aren't in a room"))
        dee = start("dee")
        check("the room goes on: ben comes back, and someone new", let_in(ada, ben) and let_in(ada, dee) and
              all(e.until(lambda s: names(s) == ["ada", "ben", "dee"] and linked(s)) for e in (ada, ben, dee)))
        ids = {p["name"]: p["id"] for p in ada.state()["people"]}
        check("someone who comes in later gets the file of an avatar that was on before", comes(ben, home["ben"], ids["ada"], wolf, 30) and comes(dee, home["dee"], ids["ada"], wolf, 30))
        ok = mine.ask("watch") == "ok"   # (a watcher is forgotten when it hasn't asked for a while)
        sees.send(pose(2.0))
        check("back in the room, ben's poses are passed on again, and the floor is laid afresh",
              ok and [p for p, _ in mine.poses()] == [ids["ben"]] and sees.ask("floor") == "ok none" and ada.ask("floor") == "ok 1.500000 -2.000000 0.250000")
        for w in (mine, sees, cys):
            w.close()

        # a second program
        twin = End(program, "twin", sock=ben.sock, env=home["ben"].env)
        ends.append(twin)
        status, out = twin.end(signal.SIGCONT)
        check("a second ft-together on the same socket doesn't start, and leaves the first one's files alone",
              status == 1 and "already running" in out and list(home["ben"].kept().values()) == [wolf])
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
        check("nothing of anyone's avatar is left behind by a program that ended", all(os.listdir(home[n].run) == [] for n in ("ada", "ben", "dee", "twin")))
    finally:
        for e in ends:
            status, out = e.end(signal.SIGKILL)
            if roomkit.failed and out:
                print("--- %s said:\n%s" % (e.name, out))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: loopback-test.py PATH-TO-ft-together")
    scratch = tempfile.mkdtemp(prefix="ft-loopback-")
    try:
        main(sys.argv[1], scratch)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    roomkit.finish()
