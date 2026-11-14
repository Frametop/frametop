"""Which grips are down (together/roommove/grips.py): what a message of SteamVR's web socket
says and whose controller it is, and the connection around that, against a stand-in for
input/vrws.py.

    python3 together/tests/test_roomgrips.py
"""
import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from roommove.grips import Held, VrGrips  # noqa: E402


class Grips(unittest.TestCase):
    L, R = "/user/hand/left", "/devices/cv/right1"

    def setUp(self):
        self.held = Held()
        self.held.know({self.L: "left", self.R: "right"})

    def say(self, device, **components):
        return self.held.take(json.dumps({"type": "update_component_states", "device": device,
                                          "components": {f"/input/{k.replace('_', '/', 1)}": v for k, v in components.items()}}))

    def test_both_are_down_when_each_said_so(self):
        self.assertFalse(self.held.both)
        self.assertTrue(self.say(self.L, grip_click=True))
        self.assertFalse(self.held.both)
        self.assertTrue(self.say(self.R, grip_click=True))
        self.assertTrue(self.held.both)
        self.assertTrue(self.say(self.L, grip_click=False))
        self.assertFalse(self.held.both)

    def test_which_hand_is_the_controllers_side_whatever_its_path(self):
        self.say(self.R, grip_click=True)
        self.assertEqual(self.held.down, {"left": False, "right": True})

    def test_other_buttons_and_the_fingers_sensing_are_not_the_grip(self):
        for components in ({"trigger_click": True}, {"grip_touch": True}, {"grip_value": 1.0}, {"a_click": True}):
            self.assertFalse(self.say(self.L, **components), components)
        self.assertEqual(self.held.down, {"left": False, "right": False})

    def test_a_controller_nobody_knows_holds_nothing(self):
        self.assertFalse(self.say("/user/hand/other", grip_click=True))
        self.assertFalse(any(self.held.down.values()))

    def test_what_isnt_a_message_is_dropped(self):
        for text in ("", "/input/grip/click", '{"device": 3, "components": {"/input/grip/click": true}}', '["/input/grip/click"]',
                     '{"device": "%s", "components": ["/input/grip/click"]}' % self.L, '{"device": "%s", "components": {"/input/grip/click": 1}}' % self.L,
                     '{"device": "%s", "components": {"/input/grip/click": "true"}}' % self.L, '{"device": "%s"' % self.L, '{"device": ["%s"], "components": {"/input/grip/click": true}}' % self.L):
            self.assertFalse(self.held.take(text), text)
        self.assertFalse(any(self.held.down.values()))

    def test_a_controller_that_went_holds_nothing(self):
        self.say(self.L, grip_click=True), self.say(self.R, grip_click=True)
        new, gone = self.held.know({self.L: "left"})
        self.assertEqual((new, gone), ([], [self.R]))
        self.assertEqual(self.held.down, {"left": True, "right": False})

    def test_new_ones_are_followed_and_a_side_that_isnt_a_hand_is_not(self):
        new, gone = self.held.know({self.L: "left", "/devices/cv/right2": "right", "/x": "middle"})
        self.assertEqual((new, gone), (["/devices/cv/right2"], [self.R]))
        self.assertTrue(self.say("/devices/cv/right2", grip_click=True))
        self.assertFalse(self.say("/x", grip_click=True))

    def test_forgotten_grips_count_again_once_pressed_again(self):
        self.say(self.L, grip_click=True), self.say(self.R, grip_click=True)
        self.held.none()
        self.assertFalse(self.held.both)
        self.say(self.R, grip_click=False)
        self.say(self.L, grip_click=True), self.say(self.R, grip_click=True)
        self.assertTrue(self.held.both)


class FakeVrws:
    """input/vrws.py, as far as VrGrips asks it."""

    def __init__(self):
        self.there = True
        self.found = {Grips.L: "left", Grips.R: "right"}
        self.sockets, self.lookups = [], 0
        outer = self

        class VrSocket:
            def __init__(self, timeout=3):
                assert timeout <= 1, "it must not wait long"
                if not outer.there:
                    raise ConnectionRefusedError("no vrserver")
                self.said, self.queue, self.closed = [], [], False
                outer.sockets.append(self)

            def open(self, mailbox):
                self.said.append(("open", mailbox))

            def subscribe(self, path, on=True):
                self.said.append(("subscribe" if on else "cancel", path))

            def messages(self, timeout=None):
                assert timeout == 0, "it must not wait"
                if not outer.there:
                    raise OSError("vrserver closed the web socket")
                got, self.queue = self.queue, []
                return got

            def fileno(self):
                return 7

            def close(self):
                self.closed = True

        self.VrSocket = VrSocket

    def controllers(self, timeout=3):
        self.lookups += 1
        if not self.there:
            raise OSError("no vrserver")
        return dict(self.found)


def grip(device, down):
    return json.dumps({"device": device, "components": {"/input/grip/click": down}})


class OverTheWebSocket(unittest.TestCase):
    def setUp(self):
        self.vrws = FakeVrws()
        self.grips = VrGrips(self.vrws, "box")

    def test_it_follows_the_controllers_steamvr_has(self):
        self.assertIsNone(self.grips.fileno())
        self.grips.look(0.0)
        self.assertEqual(self.vrws.sockets[0].said, [("open", "box"), ("subscribe", Grips.R), ("subscribe", Grips.L)])
        self.assertEqual(self.grips.fileno(), 7)

    def test_both_grips_said_down_are_both_down(self):
        self.grips.look(0.0)
        self.vrws.sockets[0].queue = [grip(Grips.L, True), '{"device": "x"}', grip(Grips.R, True)]
        self.grips.look(0.1)
        self.assertTrue(self.grips.both)

    def test_the_controllers_are_asked_about_now_and_then(self):
        for now in (0.0, 1.0, 2.9, 3.1, 4.0):
            self.grips.look(now)
        self.assertEqual(self.vrws.lookups, 2)
        self.vrws.found = {Grips.L: "left", "/devices/cv/right2": "right"}
        self.grips.look(6.2)
        self.assertEqual(self.vrws.sockets[0].said[-2:], [("cancel", Grips.R), ("subscribe", "/devices/cv/right2")])

    def test_with_steamvr_gone_no_grip_is_down_and_it_tries_again_later(self):
        self.grips.look(0.0)
        self.vrws.sockets[0].queue = [grip(Grips.L, True), grip(Grips.R, True)]
        self.grips.look(0.1)
        self.grips.look(3.5)                              # the controllers asked about again: next at 6.5
        self.vrws.there = False
        self.grips.look(3.6)
        self.assertFalse(self.grips.both)
        self.assertTrue(self.vrws.sockets[0].closed)
        self.assertIsNone(self.grips.fileno())
        self.vrws.there = True
        self.grips.look(4.0)
        self.assertEqual(len(self.vrws.sockets), 1)       # one try in five seconds
        self.grips.look(5.1)
        self.assertEqual(len(self.vrws.sockets), 2)       # ...and then the controllers are asked about at once
        self.assertEqual(self.vrws.sockets[1].said[1:], [("subscribe", Grips.R), ("subscribe", Grips.L)])
        self.assertFalse(self.grips.both)                 # what was down then isn't known now

    def test_without_steamvr_at_the_start_nothing_is_raised(self):
        self.vrws.there = False
        self.grips.look(0.0)
        self.assertFalse(self.grips.both)
        self.assertIsNone(self.grips.fileno())

    def test_an_answer_that_isnt_one_closes_it(self):
        self.vrws.controllers = lambda timeout=3: ["not", "a", "dict"]
        self.grips.look(0.0)
        self.assertIsNone(self.grips.fileno())

    def test_forgetting_and_closing(self):
        self.grips.look(0.0)
        self.vrws.sockets[0].queue = [grip(Grips.L, True), grip(Grips.R, True)]
        self.grips.look(0.1)
        self.grips.forget()
        self.assertFalse(self.grips.both)
        self.assertEqual(self.grips.fileno(), 7)          # still listening
        self.grips.close()
        self.assertTrue(self.vrws.sockets[0].closed)
        self.grips.close()                                # twice is once


if __name__ == "__main__":
    unittest.main()
