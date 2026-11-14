#!/usr/bin/env python3
"""A window of its person's PC in a room, through the real programs (docs/together.md, "A PC's
panels"): two ft-together, each with a real ft-roompanels and a stand-in ft-screens, and a PC
whose streaming host is a stand-in (fakepc.py) that answers over HTTPS as the Vibepollo fork
does and runs the real ft-pcsend when it is called in.

    together/tests/roompcs-test.py PATH-TO-ft-together PATH-TO-ft-pcsend

The pictures are random bytes: nothing here decodes them. The rules are tested without programs
in test_roompcs.py; this is what only the real ones show: the token and the pin, the call, the
pictures across the wearer's Frame, and what is left when a program ends.
"""
import json
import os
import struct
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE))
import roomkit  # noqa: E402
from fakepc import Pc  # noqa: E402
from roomframe import Frame, near  # noqa: E402
from roomkit import check  # noqa: E402
from roompanels import pclink, placing  # noqa: E402

CALC, NOTES = "0x55aa10", "0x55aa20"
AT = [1, 0, 0, 0.4, 0, 1, 0, 1.3, 0, 0, 1, -1.1]


def in_room(frame, number):
    """Where the frame's remote screen `number` stands, said in the room."""
    with frame.lock:
        pose = list(frame.screens.panels.get(number, {}).get("pose", []))
    floor = frame.floor()
    if len(pose) != 12 or floor is None:
        return None
    return placing.to_room(floor, {"at": pose[3::4], "x": pose[0::4], "y": pose[1::4], "z": pose[2::4], "metres": 1.0, "curve": 0.0})


def same_place(a, b):
    return bool(a and b) and near(a["at"], b["at"], 5e-3) and (near(a["turn"], b["turn"], 5e-3) or near(a["turn"], [-v for v in b["turn"]], 5e-3))


def state(frame):
    reply = frame.ask("state")
    return json.loads(reply[3:]) if reply.startswith("ok {") else {"shares": reply, "windows": reply, "pc": reply}


def until(what, seconds=12.0):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if what():
            return True
        time.sleep(0.1)
    return bool(what())


def panels(end):
    return [(p["id"], p["size"], p["name"]) for p in end.state()["panels"]]


def watch(end, ident):
    """A program at `end` that shows the room's panel `ident`, or None."""
    view = roomkit.Program(end, b"V", ident)
    if view.messages(3, 1) != [(b"D", b"\1")]:
        return None
    view.send(b"D", b"\1")
    view.ignore = b"D"
    return view


def loop_file(path):
    """A loop of pictures as ft-pcsend plays it; what a program that shows them gets of each."""
    bodies = [bytes([1 if i == 0 else 0]) + os.urandom(20000 if i == 0 else 700 + 50 * i) for i in range(9)]
    with open(path, "wb") as f:
        for body in bodies:
            f.write(struct.pack("<I", 1 + len(body)) + b"U" + body)
    return bodies


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip())
    ft, pcsend = sys.argv[1:]
    ada = Frame(ft, "ada", "ok 0.2000 1.6000 0.1000 30.00")
    ben = Frame(ft, "ben", "ok -0.4000 1.7000 0.3000 -120.00")
    frames, pc, view = (ada, ben), None, None
    with tempfile.TemporaryDirectory() as root:
        try:
            loop = loop_file(os.path.join(root, "loop"))
            pc = Pc(root, pcsend, os.path.join(root, "loop"))
            pc.out = {CALC: (1280, 720), NOTES: (1600, 900)}
            hosts, layout = os.path.join(root, "hosts"), os.path.join(root, "layout.json")
            token = pc.sign_in(hosts)
            windows = [{"id": f"w{k}", "client": f"frametop-pc-win{k}", "app": "window:" + address, "label": label, "size": [1280, 720],
                        "port": pc.port, "window": True, "screen": 100 + k} for k, (address, label) in enumerate(((CALC, "Calculator"), (NOTES, "")), 1)]
            with open(layout, "w") as f:
                json.dump({"hosts": [{"name": "pc", "address": "127.0.0.1", "windows": {"port": pc.port}, "displays": windows}]}, f)
            # The windows stand on ada's Frame as panels: streams of her PC's, on remote screens.
            ada.screens.remotes = {101: ["frametop-pc-win1", "1280x720"], 102: ["frametop-pc-win2", "1600x900"]}
            ada.screens.panels = {101: {"pose": list(AT), "width": [0.9]}, 102: {"pose": [1, 0, 0, -0.8, 0, 1, 0, 1.2, 0, 0, 1, -1.4], "width": [1.1]}}
            ada.keep(layout, hosts)
            ben.keep()
            check("each ft-roompanels starts in no room", all(f.tool.said("in no room") for f in frames))
            check("outside a room no window is shared, and the PC isn't asked", ada.ask("share 101") == "error not in a room" and pc.asked == [])
            check("ada opens a room and ben comes in", ada.end.ask("open") == "ok" and roomkit.let_in(ada.end, ben.end)
                  and ada.end.until(roomkit.linked) and ben.end.until(roomkit.linked) and all(f.tool.said("in a room") for f in frames))
            check("each Frame's floor is laid", until(lambda: ada.floor() is not None and ben.floor() is not None))
            told = state(ada)
            check("ada's ft-roompanels says which windows of her PC's there are to share, and ben's has none",
                  told["windows"] == [{"screen": 101, "name": "Calculator"}, {"screen": 102, "name": "Window 102"}] and told["pc"] == "" and state(ben)["windows"] == [])
            check("a window ben doesn't have isn't his to share", ben.ask("share 101") == "error screen 101 isn't a window of your PC's")
            check("the windows' panels stay on ada's Frame", ada.until(lambda s: sorted(s.remotes) == [101, 102], 1.0) and pc.asked == [])

            # shared: the PC is called in and sends
            check("ada shares her PC's window", ada.ask("share 101") == "ok 3")
            check("her PC is told where her Frame takes calls, with a code", until(lambda: len(pc.asked) >= 1) and pc.asked[0][0] == pclink.SENDER
                  and {k: v for k, v in pc.asked[0][1].items() if k != "code"} == {"address": "127.0.0.1", "port": ada.end.port} and len(pc.asked[0][1]["code"]) == 8)
            check("it calls, and is taken", until(lambda: pc.call is not None and pc.call.said("called 127.0.0.1")) and until(lambda: json.loads(ada.end.ask("media")[3:])["sender"] is True))
            check("and is told which window is its panel 3, by the window's address", until(lambda: len(pc.asked) >= 2) and pc.asked[1] == (pclink.PANELS, {"panels": [{"number": 3, "address": CALC}]}))
            check("the room has the panel, as large as the PC encodes it, with the window's title", ben.end.until(lambda s: panels(ben.end) == [(3, [1280, 720], "Calculator")], 10))
            check("it stands at ben's where the window's panel stands at ada's", ben.until(lambda s: s.remotes.get(116) == ["peer-3", "1280x720"] and "curve" in s.panels[116])
                  and until(lambda: same_place(in_room(ben, 116), in_room(ada, 101))))
            with ben.lock:
                wide = ben.screens.panels[116]["width"]
            check("as wide", wide == [0.9])
            check("ada isn't shown her own PC's panel a second time", ada.until(lambda s: sorted(s.remotes) == [101, 102], 1.0))
            view = watch(ben.end, 3)
            check("someone shows it: the PC hears that it is watched", view is not None and pc.call.said("panel 3 W 1"))
            got = view.messages(8, 12) if view else []
            check("and its pictures cross ada's Frame to the program at ben's, from the keyframe", len(got) == 12 and got[0] == (b"U", loop[0]) and all(kind == b"U" and body in loop for kind, body in got))
            check("ft-roompanels says which screen here shows the panel", ada.ask("panels") == "ok 1 3:101:1280x720" and state(ada)["shares"] == [{"screen": 101, "panel": 3, "told": True}])
            with ada.lock:
                ada.screens.panels[101]["pose"][3] = 1.0
            check("ada moves the window's panel and it moves at ben's", until(lambda: same_place(in_room(ben, 116), in_room(ada, 101))))

            # a second window, and one that closes
            check("she shares another window", ada.ask("share 102") == "ok 4")
            check("the room has both, the one without a title under a name", ben.end.until(lambda s: panels(ben.end) == [(3, [1280, 720], "Calculator"), (4, [1600, 900], "Window 2")], 10))
            check("with the one call", [path for path, _ in pc.asked].count(pclink.SENDER) == 1)
            del pc.out[NOTES]
            check("that window closes on the PC: it leaves the room, and is shared no more", ben.end.until(lambda s: panels(ben.end) == [(3, [1280, 720], "Calculator")], 12)
                  and until(lambda: state(ada)["shares"] == [{"screen": 101, "panel": 3, "told": True}]))

            # the PC's host fails, and what its API refuses
            before = len(pc.asked)
            pc.call.proc.kill()
            if view:
                view.messages(1.0)   # (what was on its way; the next call is two looks off)
            check("the PC's sender dies: it is called in again, with a new code", until(lambda: [path for path, _ in pc.asked[before:]].count(pclink.SENDER) == 1, 15)
                  and pc.asked[0][1]["code"] != [body for path, body in pc.asked[before:] if path == pclink.SENDER][0]["code"])
            got = view.messages(15, 12) if view else []
            check("and the pictures go on from its keyframe, on the panel the room had all along",
                  len(got) == 12 and got[0] == (b"U", loop[0]) and all(body in loop for _, body in got) and panels(ben.end) == [(3, [1280, 720], "Calculator")])
            pin = open(pc.pin_path).read()
            with open(pc.pin_path, "w") as f:
                f.write("sha256//AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=\n")
            time.sleep(0.5)   # (a question that was on its way)
            before = len(pc.asked)
            ok = until(lambda: state(ada)["pc"] == "the PC doesn't answer")
            check("a PC whose key isn't the one that was pinned is told nothing, and ft-roompanels says so", ok and len(pc.asked) == before)
            with open(pc.pin_path, "w") as f:
                f.write(pin)
            with open(pc.token_path, "w") as f:
                f.write("not-the-token\n")
            check("a token the PC doesn't take: ft-roompanels says what the PC said", until(lambda: state(ada)["pc"] == "the PC says: Unauthorized") and pc.refused >= 1)
            with open(pc.token_path, "w") as f:
                f.write(token + "\n")
            check("with both right again nothing holds the PC up, and the panel was in the room all along",
                  until(lambda: state(ada)["pc"] == "") and panels(ben.end) == [(3, [1280, 720], "Calculator")])

            # taken back, and what ending leaves
            call = pc.call
            check("ada takes the window back", ada.ask("unshare 101") == "ok" and ben.end.until(lambda s: panels(ben.end) == []))
            check("her PC is told to stop, and its sender ends cleanly", until(lambda: pc.asked[-1] == (pclink.STOP, {})) and call.wait(10) == 0)
            check("her Frame says it has no PC", until(lambda: json.loads(ada.end.ask("media")[3:])["sender"] is False, 20))
            check("the window's panel is still hers, where she put it", ada.until(lambda s: sorted(s.remotes) == [101, 102] and s.panels[101]["pose"][3] == 1.0, 1.0))
            check("she shares it again: the PC is called in again and the room has it", ada.ask("share 101") == "ok 3" and ben.end.until(lambda s: panels(ben.end) == [(3, [1280, 720], "Calculator")], 12))
            call = pc.call
            check("ada's ft-roompanels ends cleanly", ada.tool.end() == 0)
            check("and leaves nothing of hers in the room, and her PC told to stop", ben.end.until(lambda s: panels(ben.end) == []) and pc.asked[-1] == (pclink.STOP, {}) and call.wait(10) == 0)
            check("ben's ends cleanly", ben.tool.end() == 0)
            codes = [body["code"] for path, body in pc.asked if path == pclink.SENDER]
            for f in frames:
                trouble = [line for line in f.tool.lines if "Traceback" in line or "Error" in line or "the PC's side" in line or any(code in line for code in codes)]
                check("%s's ft-roompanels printed no error, and no code" % f.end.name, not trouble) or print("      ", trouble)
            check("the PC was called three times, each with a code of its own", len(codes) == 3 and len(set(codes)) == 3)
        finally:
            if view:
                view.close()
            for f in frames:
                if f.tool:
                    f.tool.end()
            if pc:
                pc.end()
            ends = [f.end.end() for f in frames]
    check("both ft-together end cleanly", all(status == 0 for status, _ in ends))
    for (status, out), f in zip(ends, frames):
        if status != 0 or "ERROR: " in out:
            print("--- %s ---\n%s" % (f.end.name, out))
    check("with nothing from the sanitizers", not any("ERROR: " in out for _, out in ends))
    roomkit.finish()


if __name__ == "__main__":
    main()
