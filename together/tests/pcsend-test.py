#!/usr/bin/env python3
"""A split compute PC's panels for real (docs/together.md, "A PC's panels"): ft-pcsend, which
stands for the PC, calls one ft-together with the code that one made, its pictures cross that
Frame to the program that shows them at another, and who isn't the wearer's PC is kept out.

    together/tests/pcsend-test.py PATH-TO-ft-together PATH-TO-ft-pcsend

The pictures are random bytes: nothing here decodes them. The rules themselves are tested
without a network in pcpanels-test.cpp; this is what only the real programs and links show.
"""
import json
import os
import signal
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from roomkit import End, Program, Tool, check, finish, let_in, linked  # noqa: E402

SCREEN = "1920 1080 0 1.5 -2 1 0 0 0 2 0"
START = ["panel %d %s" % (number, word) for number in (3, 4, 5, 6) for word in ("R 2000000", "F 30", "P 10", "W 0")]


def loop_file(path, pictures, first_is_key=True):
    """A loop as ft-pcsend reads it: what an encoding program hands ft-together, one picture
    after another. What a program that shows them gets of each is returned."""
    bodies = [bytes([1 if i == 0 and first_is_key else 0]) + p for i, p in enumerate(pictures)]
    with open(path, "wb") as f:
        for body in bodies:
            f.write(struct.pack("<I", 1 + len(body)) + b"U" + body)
    return bodies


def in_step(got, loop):
    """Whether these are the loop's pictures, byte for byte and in its order, and from its
    keyframe again after one that was given up."""
    at = None
    for kind, body in got:
        if kind != b"U" or body not in loop:
            return False
        if body == loop[0] or at is None:
            at = loop.index(body)
        elif body != loop[(at + 1) % len(loop)]:
            return False
        else:
            at = (at + 1) % len(loop)
    return True


def key_went_by(view, loop):
    """Reads what the program gets until the loop's keyframe has just come."""
    view.messages(0.3)
    for _ in range(4 * len(loop)):
        got = view.messages(8, 1)
        if got == [(b"U", loop[0])]:
            return True
        if got[-1:] in ([], [None]):
            return False
    return False


def media(end):
    return json.loads(end.ask("media")[3:])


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip())
    ft, pcsend = sys.argv[1:]
    tools = []
    with tempfile.TemporaryDirectory() as root:
        ada, ben = End(ft, "ada"), End(ft, "ben")

        def pc(end, code, file="loop", port=None):
            tools.append(Tool([pcsend, "127.0.0.1", str(port or end.port), code, "--loop", os.path.join(root, file), "--loopback"]))
            return tools[-1]

        def code(end):
            reply = end.ask("sender")
            return reply[3:] if reply.startswith("ok ") and len(reply) == 11 else "none"

        def shows(view, loop, pictures=15, after=None):
            """Whether the program gets the loop's pictures: from its keyframe on, after those it
            still had of the loop `after`, or as they go when no loop came before."""
            got = view.messages(8, pictures + (40 if after else 0))
            first = got.index((b"U", loop[0])) if (b"U", loop[0]) in got else len(got)
            if after is None:
                return len(got) == pictures and in_step(got, loop)
            return in_step(got[:first], after) and len(got) - first >= pictures and in_step(got[first:], loop)

        try:
            loop = loop_file(os.path.join(root, "loop"), [os.urandom(30000)] + [os.urandom(900 + 100 * i) for i in range(8)])
            # (a long one: four seconds at sixty pictures a second, so where it is in it shows)
            other = loop_file(os.path.join(root, "other"), [os.urandom(5000)] + [os.urandom(500 + i) for i in range(239)])
            loop_file(os.path.join(root, "nokey"), [os.urandom(500)] * 3, first_is_key=False)
            whole = open(os.path.join(root, "loop"), "rb").read()
            for name, data in (("torn", whole[:-5]), ("empty", b""), ("word", whole + struct.pack("<I", 5) + b"R" + bytes([1, 0, 0, 0]))):
                with open(os.path.join(root, name), "wb") as f:
                    f.write(data)

            # who may send
            ok = all(pc(ada, "ABCDEFGH", file).wait(5) == 2 for file in ("nokey", "torn", "none", "empty", "word"))
            check("a loop that doesn't begin with a keyframe, a torn or an empty one, one with more than pictures in it, or none: ft-pcsend doesn't start",
                  ok and "isn't a loop of pictures" in tools[0].lines[-1])
            check("no PC is at a Frame until one calls", media(ada)["sender"] is False and len(media(ada)["viewers"]) == 7)
            wrong = pc(ada, "ABCDEFGH")
            check("a PC with a code the Frame didn't make is refused, and told why", wrong.wait(10) == 1 and wrong.said("refused: That code isn't good") and media(ada)["sender"] is False)
            first = code(ada)
            desk = pc(ada, first.lower()[:4] + "-" + first.lower()[4:])
            check("one with the code is taken, and hears what each of its four panels is to be encoded at",
                  desk.until(lambda lines: lines == ["called 127.0.0.1"] + START) and media(ada)["sender"] is True)
            again = pc(ada, first)
            check("a code works once", again.wait(10) == 1 and again.said("refused") and media(ada)["sender"] is True and desk.proc.poll() is None)
            check("a PC that calls a port nothing listens on gives up", pc(ada, first, port=1).wait(30) == 1)

            # its panels in a room
            ok = ada.ask("open") == "ok" and let_in(ada, ben) and ada.until(linked) and ben.until(linked)
            ok = ok and ada.ask("share 3 %s A window" % SCREEN) == "ok" and ben.until(lambda s: [(p["id"], p["name"]) for p in s["panels"]] == [(3, "A window")])
            check("the wearer's Frame puts a PC's panel in the room, as one of the wearer's own", ok and ada.ask("share 7 %s No" % SCREEN).startswith("error"))
            lines = len(desk.lines)
            view = Program(ben, b"V", 3)
            ok = view.messages(3, 1) == [(b"D", b"\1")]
            view.send(b"D", b"\1")
            view.ignore = b"D"
            check("someone shows it: the PC hears that it is watched", ok and desk.said("panel 3 W 1", since=lines) and not any(" W 1" in line and "panel 3" not in line for line in desk.lines))
            check("...and its pictures cross the wearer's Frame to that program, byte for byte, in order, from the keyframe", shows(view, loop, 24, after=[]))
            state = media(ada)
            check("the Frame counts who watches the PC's panel, with no program of its own for it", state["viewers"] == [0, 0, 0, 1, 0, 0, 0] and state["sources"] == [] and state["dropped"] == 0)
            view.messages(0.3)
            came = len(view.messages(2.0))
            check("in view only, it makes a third of them: about ten a second", 12 <= came <= 30) or print("      came:", came)
            lines = len(desk.lines)
            view.send(b"A", b"\2\0")
            check("focused: the PC is to make all of the panel's pictures", desk.said("panel 3 P 30", since=lines))
            lines = len(desk.lines)
            ok = ada.ask("quality high") == "ok high"
            check("the wearer picks High: the PC hears it for every panel", ok and desk.until(lambda got: all("panel %d F 60" % n in got[lines:] for n in (3, 4, 5, 6)) and "panel 3 P 60" in got[lines:]))
            view.messages(0.5)
            check("...and the pictures go on", shows(view, loop))
            came = len(view.messages(2.0))
            check("...about sixty a second now", came >= 80) or print("      came:", came)

            # another program takes its place, in one go: the Frame learns of both at once
            lines = len(desk.lines)
            os.kill(ben.proc.pid, signal.SIGSTOP)
            try:
                view.close()
                view = Program(ben, b"V", 3)
            finally:
                os.kill(ben.proc.pid, signal.SIGCONT)
            ok = view.messages(3, 1) == [(b"D", b"\1")]
            view.send(b"D", b"\1")
            view.ignore = b"D"
            check("a program that takes the place of the one that showed it: the panel is still watched, and the PC is asked for a keyframe",
                  ok and desk.said("panel 3 K 0", since=lines) and shows(view, loop, after=[]))

            # the PC changes hands
            lines = len(desk.lines)
            wrong = pc(ada, "ZZZZZZZZ")
            check("a wrong code doesn't put the PC out", wrong.wait(10) == 1 and media(ada)["sender"] is True and shows(view, loop))
            second = pc(ada, code(ada), "other")
            check("another PC with a new code takes its place: the first is hung up on", desk.wait(10) == 1 and desk.lines[-1] == "the link to the headset ended" and second.said("panel 3 W 1"))
            check("...having said how many pictures it made", desk.lines[-2].startswith("panel 3 sent ") and int(desk.lines[-2].split()[-1]) > 100)
            check("...and the room gets the new one's pictures, from its keyframe", shows(view, other, after=loop) and media(ada)["sender"] is True)

            # a keyframe when asked, not when the loop next comes round
            ok = key_went_by(view, other)
            lines = len(second.lines)
            view.close()
            view = Program(ben, b"V", 3)
            ok = ok and view.messages(3, 1) == [(b"D", b"\1")]
            view.send(b"D", b"\1")
            view.ignore = b"D"
            first = view.messages(1.5, 1)
            check("a program that begins to show it just after the loop's keyframe: the PC is asked for one, and its next picture is the loop's first",
                  ok and second.said("panel 3 K 0", since=lines) and first == [(b"U", other[0])] and shows(view, other, 14))

            # nobody watches
            lines = len(second.lines)
            view.send(b"A", b"\0\0")
            check("the panel leaves its viewer's sight: three seconds on the PC hears nobody watches", second.said("panel 3 W 0", 8, since=lines))
            view.messages(0.5)
            check("...and sends nothing", view.messages(1.0) == [])

            # the room ends, the PC stays
            lines = len(second.lines)
            ok = ada.ask("leave") == "ok" and ben.until(lambda s: s["mode"] == "idle") and media(ada)["sender"] is True and second.proc.poll() is None
            view.close()
            ok = ok and ada.ask("open") == "ok" and let_in(ada, ben) and ben.until(linked) and ada.ask("share 4 %s Another" % SCREEN) == "ok"
            ok = ok and ben.until(lambda s: [p["id"] for p in s["panels"]] == [4])
            view = Program(ben, b"V", 4)
            ok = ok and view.messages(3, 1) == [(b"D", b"\1")]
            view.send(b"D", b"\1")
            view.ignore = b"D"
            check("the room ends and the PC stays with its Frame: in the next room another of its panels is shown", ok and second.said("panel 4 W 1", since=lines) and shows(view, other, after=[]))

            # a guest's PC
            bens = [p["id"] for p in ada.state()["people"] if p["name"] == "ben"][0] * 8 + 5
            guest = pc(ben, code(ben))
            ok = guest.until(lambda got: len(got) == 1 + len(START)) and ben.ask("share 5 %s Ben's window" % SCREEN) == "ok" and ada.until(lambda s: bens in [p["id"] for p in s["panels"]])
            mine = Program(ada, b"V", bens)
            ok = ok and mine.messages(3, 1) == [(b"D", b"\1")]
            mine.send(b"D", b"\1")
            mine.ignore = b"D"
            check("a guest's PC sends through the guest's Frame, and the owner sees its panel", ok and guest.said("panel 5 W 1") and shows(mine, loop, after=[]))
            check("...while the owner's PC's panel still goes the other way", shows(view, other))
            lines = len(guest.lines)
            mine.close()
            ok = guest.said("panel 5 W 0", 8, since=lines) and guest.until(lambda got: got[-1].startswith("panel 5 sent "), 2)
            sent = guest.lines[-1]
            view.messages(2.0)

            # the ends
            status = guest.end()
            said = [line for line in guest.lines[lines:] if " sent " in line]
            check("a PC whose panel nobody watches any more makes no pictures for it, and says how many it made as it ends", ok and sent != "panel 5 sent 0" and said == [sent, sent])
            check("a PC that is told to stop ends cleanly, and its Frame says it has none", status == 0 and ben.until(lambda s: media(ben)["sender"] is False))
            second.proc.kill()
            check("...and so it does of one that died, once its link has been silent", ada.until(lambda s: media(ada)["sender"] is False, 20))
            third = pc(ada, code(ada))
            check("the next one is taken as the first was", third.said("panel 4 W 1") and shows(view, loop, after=other))
            lines = len(third.lines)
            status, out = ada.end()
            check("its Frame ends: the PC is told, and ends too", status == 0 and third.wait(15) == 1 and third.lines[-1] == "the link to the headset ended")
            status, out = ben.end()
            check("the other ends cleanly", status == 0 and "ERROR" not in out)
        finally:
            for tool in tools:
                tool.end()
            for end in (ada, ben):
                end.end()
    finish()


if __name__ == "__main__":
    main()
