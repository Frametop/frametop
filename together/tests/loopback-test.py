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

        # moving through the room: the floor moves under its person, and the programs that draw the room are told
        check("a floor that isn't laid doesn't move", cys.ask("floor") == "ok none" and cys.ask("floor move 1 1 1").startswith("error no floor") and cys.ask("floor") == "ok none")
        ok = mine.ask("watch") == "ok" and mine.ask("listen") == "ok" and sees.ask("watch") == "ok"
        mine.words(0.1), sees.words(0.1)
        check("a floor that is moved is the one that stands", ok and ada.ask("floor move 3 -1 0.5") == "ok 3.000000 -1.000000 0.500000" and
              ada.ask("floor") == "ok 3.000000 -1.000000 0.500000" and ada.ask("floor 9 9 1") == "ok 3.000000 -1.000000 0.500000")
        check("...and its Frame's programs that watch or listen are told at once, unasked", mine.words(0.3) == ["ok 3.000000 -1.000000 0.500000"] * 2)
        check("...and nobody else's", sees.words(0.2) == [] and sees.ask("floor") == "ok 0.000000 0.000000 3.000000")
        check("a move that isn't one moves nothing", ada.ask("floor move 5000 0 0").startswith("error floor") and ada.ask("floor move 1 2").startswith("error floor") and
              mine.words(0.2) == [] and ada.ask("floor move 1.5 -2 0.25") == "ok 1.500000 -2.000000 0.250000")
        sees.send(pose(-1.0, z=2.5))
        check("where the others are is known from their poses", ada.until(lambda s: ada.ask("where") == "ok 1 %d:-1.000:1.600:2.500:0.0000" % ids["ben"]) and
              ben.ask("where") == "ok 0")
        sees.send(pose(1e9))
        check("...and not from what isn't one", ada.ask("where") == "ok 1 %d:-1.000:1.600:2.500:0.0000" % ids["ben"])
        time.sleep(2.2)
        check("someone whose poses stopped is nowhere", ada.ask("where") == "ok 0")
        mine.poses(0.1), cys.poses(0.1)   # (the pose that said where ben was)

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
        got = src.messages(5, 4)   # (it comes at once: the seconds are for a Frame that is busy)
        hello = [(b"F", b"\x1e"), (b"P", b"\x0a"), (b"R", struct.pack("<I", 2000000)), (b"W", b"\0")]
        if sorted(got) != hello:
            print("      got:", got)
        check("a program that encodes a panel is told the bitrate, the pictures a second its encoder is set for and those for a panel in view, and that nobody watches", sorted(got) == hello)
        src.ignore = b"R"   # (from here on the bitrate follows how the pictures fare, which isn't this test's to say)
        view = Program(ben, b"V", 0)
        got = src.messages(5, 2)
        if sorted(got) != [(b"K", b""), (b"W", b"\1")]:
            print("      got:", got)
        check("someone's program shows the panel: the encoder hears it is watched, and is asked for a keyframe", sorted(got) == [(b"K", b""), (b"W", b"\1")])
        check("...and the program is told to decode it", view.messages(2, 1) == [(b"D", b"\1")])
        view.send(b"D", b"\1")
        check("its pictures cross the link and come out in order, byte for byte", crosses(src, [view], [os.urandom(20000)] + [os.urandom(1500 + i) for i in range(30)]))
        src.messages(0.2)
        late = Program(cy, b"V", 0)
        late.ignore = b"D"
        ok = src.messages(2, 1) == [(b"K", b"")]
        delta = os.urandom(900)
        src.unit(delta)
        check("a second viewer waits for a keyframe, which is asked for, while the first goes on", ok and view.messages(2, 1) == [(b"U", b"\0" + delta)] and late.messages(0.3) == [])
        check("...and from the keyframe on both get every picture", crosses(src, [view, late], [os.urandom(30000)] + [os.urandom(2000) for i in range(10)]))
        check("a picture of a megabyte crosses whole", crosses(src, [view, late], [os.urandom(1 << 20)]))
        media = json.loads(ada.ask("media")[3:]), json.loads(ben.ask("media")[3:])
        check("ft-together says which panels have a program, and who watches", media[0]["sources"] == [0] and media[0]["viewers"] == [2] + [0] * 6 and media[0]["sinks"] == [] and
              media[1]["sinks"] == [0] and media[1]["sources"] == [] and media[1]["dropped"] == 0 and len(media[0]["bitrate"]) == 7 and
              media[0]["pictures"] == [10] * 7 and media[1]["stills"] == [] and media[0]["sender"] is False)

        src.messages(0.2)
        view.send(b"A", b"\0\0")
        late.close()
        hidden = time.monotonic()
        ok = src.messages(1.5) == []
        still = os.urandom(700)
        src.unit(still)
        check("one hides the panel and the other's program goes: the one who hid it still gets it", ok and view.messages(2, 1) == [(b"U", b"\0" + still)])
        ok = src.messages(6, 1) == [(b"W", b"\0")] and 2.5 <= time.monotonic() - hidden <= 5
        src.unit(os.urandom(5000), key=True)
        check("...for three seconds: then the encoder hears nobody watches, and nothing is sent", ok)
        check("...and the program is told to give its decoder up: the panel is a still", view.messages(2) == [(b"D", b"\0")] and json.loads(ben.ask("media")[3:])["stills"] == [0])
        view.send(b"D", b"\0")
        view.send(b"A", b"\2\0")
        got = sorted(src.messages(2, 3))
        if got != [(b"K", b""), (b"P", b"\x1e"), (b"W", b"\1")]:
            print("      got:", got)
        check("focused again: watched, at the full rate, and a keyframe is asked for", got == [(b"K", b""), (b"P", b"\x1e"), (b"W", b"\1")])
        check("...and the program is told to decode again", view.messages(2, 1) == [(b"D", b"\1")] and json.loads(ben.ask("media")[3:])["stills"] == [])
        view.send(b"D", b"\1")
        view.send(b"A", b"\1\0")
        check("only in view: a third of the rate", src.messages(2, 1) == [(b"P", b"\x0a")])
        view.send(b"A", b"\2\0")
        check("...and focused, all of it", src.messages(2, 1) == [(b"P", b"\x1e")] and json.loads(ada.ask("media")[3:])["pictures"] == [30] + [10] * 6)

        # how good its owner lets the video get
        def says(pictures):
            return all(e.until(lambda s: {p["id"]: p["pictures"] for p in s["panels"]} == pictures, 3) for e in (ada, ben, cy))

        ok = ada.ask("quality") == "ok balanced" and ada.ask("quality best") == "error quality low|balanced|high" and ada.ask("quality  low").startswith("error")
        ok = ok and ada.ask("quality ") == "error quality low|balanced|high" and ada.ask("qualityhigh").startswith("error") and "quality low" not in ada.ask("qualityhigh")
        check("the video has Balanced until its owner picks, and only one of the three is picked", ok and ada.ask("quality") == "ok balanced" and says({0: 30, bens: 30}))
        ok = ada.ask("quality high") == "ok high" and ada.ask("quality") == "ok high"
        got = src.messages(2, 2)
        if got != [(b"F", b"\x3c"), (b"P", b"\x3c")]:
            print("      got:", got)
        media = json.loads(ada.ask("media")[3:])
        check("High: the encoder is to be set for 60 pictures a second, and makes them for the one who has the panel focused",
              ok and got == [(b"F", b"\x3c"), (b"P", b"\x3c")] and media["pictures"] == [60] + [20] * 6 and media["bitrate"][1] == 2000000)
        check("...and everyone's room says so of its owner's panels, and of nobody else's", says({0: 60, bens: 30}))
        check("a guest picks: the same, through the owner", ben.ask("quality low") == "ok low" and says({0: 60, bens: 15}))
        ok = ada.ask("quality low") == "ok low"
        got = src.messages(2, 2)
        media = json.loads(ada.ask("media")[3:])
        check("Low: fewer pictures, and the bitrate comes down to its cap at once",
              ok and got == [(b"F", b"\x0f"), (b"P", b"\x0f")] and media["pictures"] == [15] + [5] * 6 and media["bitrate"][1] == 1000000 and media["bitrate"][0] <= 1000000 and
              says({0: 15, bens: 15}))
        ok = ada.ask("quality balanced") == "ok balanced" and ben.ask("quality balanced") == "ok balanced"
        got = src.messages(2, 2)
        check("Balanced again: the bitrate may climb from where it is", ok and got == [(b"F", b"\x1e"), (b"P", b"\x1e")] and
              json.loads(ada.ask("media")[3:])["bitrate"][1] == 1000000 and says({0: 30, bens: 30}))
        for name, kept, has in (("eve", "high", "ok high"), ("flo", "best", "ok balanced")):
            home[name] = Home(root, name)
            home[name].env["FT_TOGETHER_QUALITY"] = kept
            other = start(name)
            ok = other.until(lambda s: s["mode"] == "idle") and other.ask("quality") == has
            check("an ft-together starts with the quality frametop.conf has" if kept == "high" else "...or with Balanced when that isn't one", ok and other.end()[0] == 0)

        # a program that doesn't take its pictures
        lazy = Program(cy, b"V", 0)
        lazy.ignore = b"D"
        src.messages(1.5)
        for i in range(12):
            src.unit(os.urandom(1 << 20), key=True)
            view.messages(0.3)
        check("a program that doesn't take its pictures is let go, and the others still get theirs", lazy.let_go(10) and crosses(src, [view], [os.urandom(4000), os.urandom(400)]))

        # a program the hardware gives no decoder
        none = Program(cy, b"V", 0)
        ok = none.messages(2, 1) == [(b"D", b"\1")] and ada.until(lambda s: json.loads(ada.ask("media")[3:])["viewers"][0] == 2, 3)
        none.send(b"D", b"\2")
        check("a program that says the hardware has no decoder for it: its panel is a still, and its owner hears nobody sees it there",
              ok and ada.until(lambda s: json.loads(ada.ask("media")[3:])["viewers"][0] == 1, 3) and json.loads(cy.ask("media")[3:])["stills"] == [0] and
              none.messages(0.5) == [])
        none.close()

        # programs that say what they may not
        twin = Program(ada, b"S", 0)
        got = twin.messages(1, 4)
        check("a second program for the same panel takes the first one's place, and is told what the first was",
              src.let_go() and sorted(kind for kind, _ in got) == [b"F", b"P", b"R", b"W"] and (b"F", b"\x1e") in got and (b"P", b"\x1e") in got and (b"W", b"\1") in got)
        twin.ignore = b"R"
        wrong = [Program(ada, b"S", 3), Program(ada, b"V", 32), Program(ada, b"X", 0), Program(ben), Program(ben), Program(ben, b"V", bens), Program(ada, b"S", 1),
                 Program(cy, b"V", 0), Program(ada, b"S", 2)]
        wrong[3].sock.sendall(bytes(4))                               # a message of no length
        wrong[4].sock.sendall(struct.pack("<I", (1 << 20) + 3))       # ...and one longer than any
        wrong[5].unit(b"a picture, from a program that shows them")
        wrong[6].send(b"A", b"\1\0")
        wrong[7].send(b"D", b"\3")
        wrong[8].send(b"D", b"\1")
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
        check("...and their pose goes nowhere, and their floor is lifted", mine.poses(0.3) == [] and sees.ask("floor").startswith("error you aren't in a room") and
              sees.ask("floor move 1 1 1").startswith("error you aren't in a room") and sees.ask("where").startswith("error you aren't in a room"))
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
