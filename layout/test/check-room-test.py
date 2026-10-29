#!/usr/bin/env python3
"""ft-layout doesn't save the arrangement while SteamVR has lost the room (DEE-206): capture
asks ft-screens' "universe" first. Against a fake ft-screens socket; no SteamVR.

  layout/test/check-room-test.py
"""
import os
import sys

HERE = os.path.dirname(os.path.realpath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import ft_layout  # noqa: E402

failed = 0


def check(what, ok):
    global failed
    failed += not ok
    print(f"{'ok  ' if ok else 'FAIL'}  {what}")


class Fake:
    """ft-screens' control socket: "universe" answers `reply`, or fails as `error` does."""

    def __init__(self, reply=None, error=None):
        self.reply, self.error, self.asked = reply, error, []

    def ask(self, text, timeout=10.0):
        self.asked.append(text)
        if self.error:
            raise self.error
        if not self.reply.startswith("ok"):
            raise RuntimeError(self.reply)
        return self.reply


def refused(sock):
    try:
        ft_layout.check_room(sock)
    except ft_layout.NoAnswer:
        raise
    except RuntimeError as e:
        return str(e)
    return None


sock = Fake("ok 7619347082598966940 room")
check("in the room: saves", refused(sock) is None and sock.asked == ["universe"])
check("in a temporary universe: refuses, and says why",
      "lost the room" in (refused(Fake("ok 4396560858472634643 temporary")) or ""))
check("the dummy universe too", refused(Fake("ok 424242 temporary")) is not None)
check("a universe of unknown kind: saves", refused(Fake("ok 12345 unknown")) is None)
check("an older ft-screens without the command: saves", refused(Fake("error unknown command")) is None)
try:
    refused(Fake(error=ft_layout.NoAnswer("ft-screens didn't answer")))
    check("ft-screens not answering isn't taken for a room", False)
except ft_layout.NoAnswer:
    check("ft-screens not answering isn't taken for a room", True)

# capture asks before it reads anything else
calls = []
real = ft_layout.screens_socket
ft_layout.screens_socket = lambda: Fake("ok 4396560858472634643 temporary")
ft_layout.load_layout = lambda: calls.append("load") or {"screens": []}
ft_layout.save_layout = lambda layout: calls.append("save")
try:
    ft_layout.capture_screens()
    check("capture in a temporary universe raises", False)
except RuntimeError:
    check("capture in a temporary universe raises", True)
check("...and saves nothing", "save" not in calls)
ft_layout.screens_socket = real

print("all passed" if not failed else f"{failed} failed")
sys.exit(1 if failed else 0)
