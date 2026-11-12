#!/usr/bin/env python3
"""A shared panel's video for real, on this Frame's own encoder and decoder: ft-sharepanel
encodes a test picture that says which one it is (media/testpattern.h), two ft-together pass
it over a loopback link, and ft-peerpanel decodes it and reads it back.

    together/tests/panels-test.py FT-TOGETHER FT-SHAREPANEL FT-PEERPANEL

It needs the Frame's video devices, and is skipped without them. What ft-together does with
the pictures is tested with made-up ones in loopback-test.py; this is what only the hardware
shows: that the pictures decode, whole and in order, in the colours they were drawn in, at
every size a panel is sent at, that a panel nobody has focused is encoded less often, and that
nothing is encoded while nobody watches.
"""
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import roomkit  # noqa: E402
from roomkit import check  # noqa: E402

DEVICES = ["/dev/video-enc0", "/dev/video-dec0", "/dev/dri/renderD128"]
WHERE = "0 1.5 -2 1 0 0 0 2 0"   # a panel's place in the room: not this test's concern


def pictures(tool, since=0):
    """What a prober read: [{size, number, light, dark, grey}], from its line `since` on."""
    got = []
    for line in list(tool.lines)[since:]:
        w = line.split()
        if len(w) == 11 and w[0] == "picture":
            got.append({"size": w[2], "number": int(w[4]), "light": int(w[6]), "dark": int(w[8]), "grey": int(w[10])})
    return got


def jumps(numbers):
    """How often the next number isn't the one after (they wrap at 65536)."""
    return sum(1 for a, b in zip(numbers, numbers[1:]) if b != (a + 1) % 65536)


def true_colours(pics):
    """The picture's light squares are 240, its dark ones 16 and its desktop 48: a decoder that
    took the encoder's full range for video range would say about 221, 37 and 61."""
    return all(p["light"] >= 228 and p["dark"] <= 28 and abs(p["grey"] - 48) <= 7 for p in pics)


def main(together, share, peer):
    ends, tools = [], []

    def start(name):
        e = roomkit.End(together, name)
        ends.append(e)
        return e

    def run(argv):
        t = roomkit.Tool(argv)
        tools.append(t)
        return t

    def sharer(end, panel, size, fps=30):
        return run([share, "--panel", str(panel), "--size", size, "--fps", str(fps), "--socket", end.sock, "--pattern"])

    def prober(end, panel, *more):
        return run([peer, "--panel", str(panel), "--socket", end.sock, "--probe"] + list(more))

    ada, ben = start("ada"), start("ben")
    try:
        ok = all(e.until(lambda s: s["mode"] == "idle") for e in (ada, ben)) and ada.ask("open") == "ok" and roomkit.let_in(ada, ben)
        if not check("a room with two in it", ok and ben.until(lambda s: roomkit.linked(s) and len(s["people"]) == 2)):
            return
        lost = prober(ada, 0, "--socket", "ft_together_test_nobody")
        check("a program with no ft-together to talk to says so and ends", lost.wait(10) == 1 and lost.said("no ft-together", 1))

        ok = ada.ask("share 0 640 360 %s Test" % WHERE) == "ok" and ben.until(lambda s: [p["id"] for p in s["panels"]] == [0])
        src = sharer(ada, 0, "640x360")
        ok = ok and src.said("waiting for someone to watch") and ada.until(lambda s: json.loads(ada.ask("media")[3:])["sources"] == [0])
        time.sleep(1.0)
        check("ada shares a panel, and while nobody watches nothing is encoded", ok and not src.said("watched", 0))

        # someone watches
        view = prober(ben, 0, "--frames", "90")
        began = time.monotonic()
        status = view.wait(40)
        took = time.monotonic() - began
        pics = pictures(view)
        numbers = [p["number"] for p in pics]
        check("ben's program shows it: the encoder starts", src.said("watched: encoding", 1))
        if not check("ninety pictures decode, at the size they were sent", status == 0 and len(pics) == 90 and {p["size"] for p in pics} == {"640x360"}):
            print("    (status %s, %d pictures, sizes %s)" % (status, len(pics), sorted({p["size"] for p in pics})))
        if not check("each says which one it is, and they come in order, none missing", -1 not in numbers and jumps(numbers) <= 2):
            print("    (numbers %s)" % numbers)
        if not check("...in the colours they were drawn in", true_colours(pics)):
            print("    (light %s, dark %s, grey %s)" % tuple(sorted({p[k] for p in pics}) for k in ("light", "dark", "grey")))
        if not check("...at the rate they were drawn at", 2.5 <= took <= 12):
            print("    (90 pictures at 30 a second took %.1f s)" % took)

        # nobody watches
        check("the program goes: a few seconds on, the encoder is closed", src.said("nobody watches: the encoder is closed", 10))
        time.sleep(1.0)
        last = numbers[-1] if numbers else 0
        mark = len(src.lines)
        again = prober(ben, 0, "--frames", "30")
        status = again.wait(40)
        numbers = [p["number"] for p in pictures(again)]
        ok = status == 0 and len(numbers) == 30 and -1 not in numbers and jumps(numbers) <= 2
        check("a viewer comes back: the encoder opens again, and the pictures come", ok and src.said("watched: encoding", 1, mark))
        if not check("no picture was drawn in between", ok and 0 < (numbers[0] - last) % 65536 < 40):
            print("    (the last one seen was %d, the first one now %s)" % (last, numbers[:1]))

        # someone who only has the panel in view
        mark = len(src.lines)
        glance = prober(ben, 0, "--view", "--seconds", "6")
        status = glance.wait(40)
        numbers = [p["number"] for p in pictures(glance)]
        check("a viewer who only has the panel in view: it is encoded at a third of its rate", src.said("at most 10 pictures a second", 1, mark))
        if not check("...and that many pictures come, whole and in order", status == 0 and 30 <= len(numbers) <= 75 and -1 not in numbers and jumps(numbers) <= 2):
            print("    (status %s, %d pictures in 6 s: %s)" % (status, len(numbers), numbers))

        # the one who shares starts its program again, as ft-screens does when it ends
        mark = len(src.lines)
        stay = prober(ben, 0, "--seconds", "25")
        ok = stay.until(lambda lines: len(pictures(stay)) >= 20, 15)
        media = json.loads(ada.ask("media")[3:]), json.loads(ben.ask("media")[3:])
        check("ft-together says who encodes, who watches and who shows", ok and media[0]["sources"] == [0] and media[0]["viewers"][0] == 1 and media[1]["sinks"] == [0])
        check("focused, the panel is encoded at its full rate again", media[0]["pictures"][0] == 30 and src.said("at most 30 pictures a second", 1, mark))
        check("the encoding program ends when told to", src.end() == 0)
        mark = len(stay.lines)
        time.sleep(0.5)
        src = sharer(ada, 0, "640x360")
        ok = stay.until(lambda lines: len(pictures(stay, mark)) >= 30, 15)
        pics = pictures(stay, mark)[-20:]
        numbers = [p["number"] for p in pics]
        if not check("a new encoder's pictures go on into the decoder that showed the old one's", ok and -1 not in numbers and jumps(numbers) <= 2 and
                     numbers[-1] < 2000 and true_colours(pics)):
            print("    (numbers %s; it said %s)" % (numbers, stay.lines[-5:]))
        stay.end()
        src.end()

        # a size the encoder isn't given (media/irisenc.h: it reset the Frame, twice): refused by
        # the program before the device is touched, which encsize-test.cpp says without the Frame
        ok = ada.ask("share 0 1366 768 %s Test" % WHERE) == "ok"
        src = sharer(ada, 0, "1366x768")
        view = prober(ben, 0, "--seconds", "3")
        check("a panel 1366 pixels across: the program says it can't encode that, and nothing is encoded",
              ok and src.said("can't encode: the encoder isn't given a picture of 1366x768", 5) and view.wait(10) == 0 and pictures(view) == [])
        src.end()

        # the sizes a panel is sent at (roompanels/keeper.py, stream_size): screens of 1920x1080,
        # 1280x800, 1366x768, 854x480, 1920x1200 and one on its side
        for size in ["1920x1080", "1280x800", "1408x792", "896x504", "1920x1200", "640x1136"]:
            width, height = size.split("x")
            ok = ada.ask("share 0 %s %s %s Test" % (width, height, WHERE)) == "ok"
            src = sharer(ada, 0, size)
            view = prober(ben, 0, "--frames", "20")
            status = view.wait(40)
            pics = pictures(view)
            numbers = [p["number"] for p in pics]
            ok = ok and status == 0 and len(pics) == 20 and {p["size"] for p in pics} == {size} and -1 not in numbers and jumps(numbers) <= 2 and true_colours(pics)
            if not check("a panel of %s pixels: its pictures come whole, in order and in their colours" % size, ok):
                print("    (status %s, pictures %s; the encoder said %s; the decoder said %s)" % (status, pics[:3], src.lines[-4:], view.lines[-4:]))
            src.end()

        # ft-together goes
        src = sharer(ada, 0, "640x360")
        stay = prober(ben, 0, "--seconds", "25")
        ok = stay.until(lambda lines: len(pictures(stay)) >= 5, 15)
        status = [e.end()[0] for e in (ada, ben)]
        check("both ft-together end cleanly", ok and status == [0, 0])
        check("...and the programs on their sockets end with them, to be started again", src.wait(10) == 1 and stay.wait(10) == 1 and
              src.said("ft-together went", 1) and stay.said("ft-together went", 1))
    finally:
        for t in tools:
            t.end()
        for e in ends:
            status, out = e.end()
            if roomkit.failed and out:
                print("--- %s said:\n%s" % (e.name, out))
        if roomkit.failed:
            for t in tools:
                print("--- %s said:\n%s" % (" ".join(t.proc.args[:1] + t.proc.args[1:6]), "\n".join(t.lines[-25:])))


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit("usage: panels-test.py FT-TOGETHER FT-SHAREPANEL FT-PEERPANEL")
    missing = [d for d in DEVICES if not os.path.exists(d)]
    if missing:
        print("SKIPPED: no %s (the Frame's video devices)" % ", ".join(missing))
        sys.exit(0)
    main(*sys.argv[1:])
    roomkit.finish()
