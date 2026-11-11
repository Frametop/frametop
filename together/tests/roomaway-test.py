#!/usr/bin/env python3
"""ft-roomaway next to a real ft-together in a room with another, with stand-ins for the pointer
helper and the input relay: what they say of the wearer reaches the other end's room.

    roomaway-test.py FT-TOGETHER
"""
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import roomkit  # noqa: E402
from fakescreens import serve  # noqa: E402
from roomkit import check  # noqa: E402


class Says:
    """A stand-in that answers every question with `reply` (None: with nothing)."""

    def __init__(self, reply):
        self.reply, self.asked = reply, []

    def __call__(self, text):
        self.asked.append(text)
        return self.reply


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip())
    ada, ben = roomkit.End(sys.argv[1], "ada"), roomkit.End(sys.argv[1], "ben")
    helper, relay = Says("ok on worn"), Says('{"t": "pause", "paused": false}')
    names = ["ft_test_%d_%s" % (os.getpid(), n) for n in ("helper", "relay")]
    serve(helper, names[0])
    serve(relay, names[1])
    tool = roomkit.Tool([str(HERE.parent / "ft-roomaway"), "--together", ben.sock, "--helper", names[0], "--relay", names[1]])

    def bens(end):
        return lambda state: [p["away"] for p in state["people"] if p["name"] == "ben"]

    try:
        check("it starts in no room, and asks nobody but ft-together", tool.said("in no room") and not helper.asked and not relay.asked)
        check("ada opens a room and ben comes in", ada.ask("open") == "ok" and roomkit.let_in(ada, ben) and ada.until(roomkit.linked) and ben.until(roomkit.linked))
        check("it sees the room, and has nothing to tell", tool.said("in a room") and helper.asked[-1] == "gaze ? headset" and relay.asked[-1] == "pause ?"
              and bens(ada)(ada.state()) == [0])
        helper.reply = "ok on away"
        check("ben's headset comes off: ada's room has him away", ada.until(lambda s: bens(ada)(s) == [2]) and tool.said("in a room: off"))
        helper.reply, relay.reply = "ok off worn", '{"t": "pause", "paused": true, "reason": "game"}'
        check("on again, in a game", ada.until(lambda s: bens(ada)(s) == [1]) and tool.said("in a room: game"))
        relay.reply = '{"t": "pause", "paused": false}'
        check("and back", ada.until(lambda s: bens(ada)(s) == [0]) and tool.said("in a room: back"))
        check("what another program said is put right", ben.ask("away off") == "ok" and ada.until(lambda s: bens(ada)(s) == [2]) and ada.until(lambda s: bens(ada)(s) == [0]))
        helper.reply = "ok on away"
        check("off again", ada.until(lambda s: bens(ada)(s) == [2]))
        helper.reply = None
        check("a helper that stops answering: after a few seconds he is taken to be there", ada.until(lambda s: bens(ada)(s) == [0], 12))
        lines = len(tool.lines)
        check("ben leaves: it is in no room again", ben.ask("leave") == "ok" and tool.said("in no room", since=lines))
        check("told to end, it ends cleanly", tool.end() == 0 and not any("Traceback" in line for line in tool.lines))
    finally:
        tool.end()
        ends = [ada.end(), ben.end()]
    check("both ft-together end cleanly, with nothing from the sanitizers", all(status == 0 and "ERROR: " not in out for status, out in ends))
    roomkit.finish()


if __name__ == "__main__":
    main()
