#!/usr/bin/env python3
"""A split compute PC's own pictures (docs/together.md, "A PC's panels"): ft-pcsend with a
feed, as a streaming host runs it. This test stands where the host does: it hands pictures over
on ft-pcsend's standard input and reads what it is to encode from the lines.

    together/tests/pcfeed-test.py PATH-TO-ft-together PATH-TO-ft-pcsend

The pictures are random bytes: nothing here decodes them. What a PC's link and panels may do is
tested in pcpanels-test.cpp and, with a loop for the pictures, in pcsend-test.py.
"""
import os
import signal
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from roomkit import End, Program, Tool, check, finish, let_in, linked  # noqa: E402

SCREEN = "1920 1080 0 1.5 -2 1 0 0 0 2 0"
START = ["panel %d %s" % (number, word) for number in (3, 4, 5, 6) for word in ("R 2000000", "F 30", "P 10", "W 0")]


def record(number, key, picture, kind=b"N", flags=None):
    body = bytes([number, (1 if key else 0) if flags is None else flags]) + picture
    return struct.pack("<I", 1 + len(body)) + kind + body


def shown(picture, key=False):
    """What a program that shows the panel gets of a picture."""
    return (b"U", bytes([1 if key else 0]) + picture)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip())
    ft, pcsend = sys.argv[1:]
    tools = []
    ada, ben = End(ft, "ada"), End(ft, "ben")

    def pc(end, code=None, *more, argv_code="-"):
        env = dict(os.environ)
        env.pop("FT_PCSEND_CODE", None)
        if code is not None:
            env["FT_PCSEND_CODE"] = code
        tools.append(Tool([pcsend, "127.0.0.1", str(end.port), argv_code, "--loopback", *more], env=env, stdin=subprocess.PIPE))
        return tools[-1]

    def hand(tool, *records):
        tool.proc.stdin.buffer.write(b"".join(records))
        tool.proc.stdin.buffer.flush()

    def code(end):
        reply = end.ask("sender")
        return reply[3:] if reply.startswith("ok ") and len(reply) == 11 else "none"

    def watch(end, panel):
        view = Program(end, b"V", panel)
        ok = view.messages(3, 1) == [(b"D", b"\1")]
        view.send(b"D", b"\1")
        view.ignore = b"D"
        return view if ok else None

    def sent(tool, number):
        """The last count of pictures it said it sent for the panel."""
        said = [line for line in tool.lines if line.startswith("panel %d sent " % number)]
        return int(said[-1].split()[-1]) if said else None

    try:
        # how it starts
        ok = pc(ada, "ABCDEFGH").wait(5) == 2 and pc(ada, "ABCDEFGH", "--feed", "--loop", "nothing").wait(5) == 2 and pc(ada, None, "--feed").wait(5) == 2
        check("pictures from a feed or from a loop, not neither or both, and a code that is to be in the environment is: or ft-pcsend doesn't start",
              ok and all("usage" in tool.lines[-1] for tool in tools))
        desk = pc(ada, code(ada), "--feed")
        check("with a feed and the code in its environment it is taken, and says what each of its four panels is to be encoded at",
              desk.until(lambda lines: lines == ["called 127.0.0.1"] + START))

        # nobody watches
        first = [os.urandom(4000)] + [os.urandom(300 + i) for i in range(11)]
        hand(desk, *[record(3, i == 0, picture) for i, picture in enumerate(first)])
        ok = ada.ask("open") == "ok" and let_in(ada, ben) and ada.until(linked) and ben.until(linked)
        ok = ok and ada.ask("share 3 %s A window" % SCREEN) == "ok" and ben.until(lambda s: [p["id"] for p in s["panels"]] == [3])
        lines = len(desk.lines)
        view = watch(ben, 3)
        check("pictures handed over while nobody watches the panel go nowhere", ok and view is not None and desk.said("panel 3 W 1", since=lines) and view.messages(0.5) == [])

        # watched: from a keyframe on
        late = [os.urandom(200) for _ in range(3)]
        hand(desk, *[record(3, False, picture) for picture in late])
        check("...and neither do the ones that lean on them, once someone does", view.messages(0.5) == [])
        second = [os.urandom(5000)] + [os.urandom(400 + i) for i in range(19)]
        hand(desk, *[record(3, i == 0, picture) for i, picture in enumerate(second)], record(4, True, os.urandom(900)))
        got = view.messages(5, len(second))
        check("from the next keyframe on they cross the wearer's Frame to the program that shows the panel, byte for byte and in order",
              got == [shown(second[0], True)] + [shown(picture) for picture in second[1:]])
        check("...and a picture of a panel nobody watches stays here", view.messages(0.5) == [])

        # a keyframe the Frame asks for: the pictures until it comes are still passed on
        lines = len(desk.lines)
        view.send(b"A", b"\2\1")
        ok = desk.said("panel 3 K 0", since=lines)
        more = [os.urandom(350) for _ in range(5)]
        hand(desk, *[record(3, False, picture) for picture in more])
        view.messages(0.5)
        third = [os.urandom(3000)] + [os.urandom(450) for _ in range(4)]
        hand(desk, *[record(3, i == 0, picture) for i, picture in enumerate(third)])
        got = view.messages(5, len(third))
        check("the Frame asks for a keyframe: the line says so, and the one handed over next is shown", ok and got[-len(third):] == [shown(third[0], True)] + [shown(picture) for picture in third[1:]])

        # nobody watches any more
        def unwatched(view):
            """Whether the PC hears that nobody watches once the program that showed the panel is gone, and says what it sent."""
            lines = len(desk.lines)
            view.close()
            return desk.said("panel 3 W 0", 8, since=lines) and desk.until(lambda got: got[-1].startswith("panel 3 sent "), 2)

        ok = unwatched(view)
        whole = len(second) + len(more) + len(third)
        check("nobody watches: it says how many pictures it sent, which is all it was handed from the keyframe on", ok and sent(desk, 3) == whole) or print("      sent:", sent(desk, 3), whole)
        hand(desk, *[record(3, i == 0, os.urandom(300)) for i in range(4)])
        time.sleep(0.3)
        lines = len(desk.lines)
        view = watch(ben, 3)
        ok = view is not None and desk.said("panel 3 W 1", since=lines)
        hand(desk, *[record(3, False, os.urandom(300)) for i in range(3)])
        time.sleep(0.3)
        ok = ok and unwatched(view)
        check("watched again and not: what was handed over in between, and what leans on it, wasn't sent", ok and sent(desk, 3) == whole) or print("      sent:", sent(desk, 3), whole)

        # a picture lost on the way: the host is asked, by the PC's own end
        lines = len(desk.lines)
        view = watch(ben, 3)
        ok = view is not None and desk.said("panel 3 W 1", since=lines)
        hand(desk, record(3, True, os.urandom(2000)))
        ok = ok and [kind for kind, body in view.messages(5, 1) if kind] == [b"U"]
        lines = len(desk.lines)
        os.kill(ada.proc.pid, signal.SIGSTOP)
        try:
            deadline = time.monotonic() + 1.2
            while time.monotonic() < deadline:
                hand(desk, record(3, False, os.urandom(600)))
                time.sleep(0.03)
            asked = [line for line in desk.lines[lines:] if line == "panel 3 K 0"]
        finally:
            os.kill(ada.proc.pid, signal.SIGCONT)
        check("pictures the Frame doesn't take in time are given up, and the host is asked for a keyframe: now and then, not for each picture", ok and 1 <= len(asked) <= 5) or print("      asked:", len(asked))
        view.messages(1.0)
        fourth = [os.urandom(2500)] + [os.urandom(500) for _ in range(6)]
        hand(desk, *[record(3, i == 0, picture) for i, picture in enumerate(fourth)])
        got = view.messages(5, len(fourth))
        check("...and the keyframe it then makes is shown, with what follows it", got[-len(fourth):] == [shown(fourth[0], True)] + [shown(picture) for picture in fourth[1:]])
        before = desk.lines.count("panel 3 K 0")
        ok = unwatched(view)
        count = sent(desk, 3)
        hand(desk, *[record(3, False, os.urandom(300)) for i in range(12)])
        time.sleep(0.6)
        check("...and a host isn't asked for keyframes of a panel nobody watches", ok and whole + 1 + len(fourth) < count and desk.lines.count("panel 3 K 0") == before)

        # more pictures at once than the link takes
        lines = len(desk.lines)
        view = watch(ben, 3)
        ok = view is not None and desk.said("panel 3 W 1", since=lines)
        hand(desk, record(3, True, os.urandom(2000)))
        ok = ok and [kind for kind, body in view.messages(5, 1) if kind] == [b"U"]
        time.sleep(0.4)
        lines = len(desk.lines)
        os.kill(ada.proc.pid, signal.SIGSTOP)
        try:
            hand(desk, *[record(3, False, os.urandom(250)) for _ in range(100)])
            asked = desk.said("panel 3 K 0", 0.1, since=lines)
        finally:
            os.kill(ada.proc.pid, signal.SIGCONT)
        view.messages(1.0)
        hand(desk, *[record(3, False, os.urandom(250)) for _ in range(5)])
        time.sleep(0.4)
        ok = ok and unwatched(view)
        check("a hundred at once: the link takes 64, the rest and what leans on them aren't sent, and the host is asked for a keyframe",
              ok and asked and sent(desk, 3) == count + 1 + 64) or print("      sent:", sent(desk, 3), count + 1 + 64, asked)
        count = sent(desk, 3)

        # the feed ends
        desk.proc.stdin.close()
        status = desk.wait(10)
        said = [line for line in desk.lines if " sent " in line]
        check("the host closes the feed: ft-pcsend ends cleanly, having said what it sent of each panel that had any", status == 0 and said[-1] == "panel 3 sent %d" % count and len(said) == 5)
        check("...and its Frame says it has no PC", ada.until(lambda s: '"sender":false' in ada.ask("media"), 20))

        # what isn't a feed
        for what, bad in (("of a kind it doesn't know", record(3, True, b"x" * 50, kind=b"U")), ("of a number no panel has", record(8, True, b"x" * 50)),
                          ("with flags nobody set", record(3, True, b"x" * 50, flags=2)), ("with nothing in it", record(3, True, b"")),
                          ("longer than a picture may be", record(3, True, bytes((1 << 20) + 1))), ("torn off", record(3, True, b"x" * 50)[:-7])):
            other = pc(ada, code(ada), "--feed")
            ok = other.said("panel 6 W 0")
            try:
                hand(other, record(3, True, os.urandom(100)), bad)
                other.proc.stdin.close()
            except BrokenPipeError:   # (it ended before it had read all of it)
                pass
            check("a picture " + what + " ends it: the feed isn't one", ok and other.wait(10) == 2 and "isn't a picture" in other.lines[-1])
        status, out = ada.end()
        check("the Frame ends cleanly", status == 0 and "ERROR" not in out)
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
