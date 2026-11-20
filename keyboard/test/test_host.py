"""host.py, offline: what Frametop asks the hosted keyboard for, with fakes for the keyboard
(its requests), the relay link and the Steam hook."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "host"))
import host as hostmod  # noqa: E402
from host import Host  # noqa: E402

POSE = [[1.0, 0.0, 0.0, 0.0], [0.0, 1.0, 0.0, 1.6], [0.0, 0.0, 1.0, 0.0]]
POSE_WORDS = [f"{v}" for row in POSE for v in row]
GEOMETRY = {"event": "geometry", "width": 0.6, "height": 0.25,
            "matrix": [[1, 0, 0, 0], [0, 1, 0, 1.2], [0, 0, 1, -0.7]]}


class FakeKeyboard:
    def __init__(self):
        self.sent = []

    def send(self, op, **fields):
        self.sent.append((op, fields))
        return len(self.sent)


class FakeRelay:
    def __init__(self):
        self.heard = True
        self.hellos, self.asked, self.texts, self.keys = [], 0, [], []

    def fresh(self, now):
        return self.heard

    def hello(self, shown):
        self.hellos.append(shown)

    def steam(self):
        self.asked += 1

    def text(self, now, text):
        self.texts.append(text)
        return True

    def key(self, now, code, down):
        self.keys.append((code, down))
        return True

    def flush(self):
        pass


class FakeSteam:
    def __init__(self):
        self.take = False
        self.typed, self.pressed, self.closed = [], [], []

    def type(self, session, text, done):
        self.typed.append((session, text, done))

    def key(self, session, name, modifiers, done):
        self.pressed.append((session, name, modifiers, done))

    def close_session(self, session):
        self.closed.append(session)


class HostTest(unittest.TestCase):
    def setUp(self):
        self.keyboard, self.relay, self.steam = FakeKeyboard(), FakeRelay(), FakeSteam()
        self.allowed = True
        self.host = Host(self.keyboard, self.relay, self.steam, lambda: self.allowed)
        self.host.on_keyboard(0.0, {"event": "ready"})

    def ops(self):
        out = [op for op, _ in self.keyboard.sent]
        self.keyboard.sent.clear()
        return out

    def last(self, op):
        return [f for o, f in self.keyboard.sent if o == op][-1]

    def shown(self, now=0.0):
        self.host.on_keyboard(now, {"event": "visibility", "visible": True})

    def test_desktop_show_from_the_relay_places_it_once_geometry_is_known(self):
        self.host.on_relay(0.0, ["1", "1", "0"])   # typing to the desktop, screens shown
        self.host.on_relay(0.0, ["show", "head", *POSE_WORDS])
        self.assertEqual(self.ops(), ["sink", "context", "show"])   # no geometry yet: placed later
        self.host.on_keyboard(0.1, GEOMETRY)
        self.assertEqual(self.ops(), ["set_pose"])
        self.shown()
        self.assertEqual(self.relay.hellos, [True])
        self.host.on_relay(0.2, ["show", "head", *POSE_WORDS])
        self.assertEqual(self.ops(), ["set_pose", "show"])   # geometry known: placed at once

    def test_a_show_with_no_spot_recentres_only_when_hidden(self):
        self.host.on_relay(0.0, ["show"])
        self.assertEqual(self.ops(), ["context", "recenter", "show"])
        self.shown()
        self.host.on_relay(0.1, ["show"])
        self.assertEqual(self.ops(), ["show"])

    def test_routes(self):
        self.host.on_relay(0.0, ["1", "1", "0"])
        self.host.on_relay(0.0, ["show"])
        self.assertEqual(self.keyboard.sent[0], ("sink", {"on": True}))
        self.assertEqual(self.keyboard.sent[1], ("context", {"context": "desktop", "hints": {"purpose": "text"}}))
        self.shown()
        self.ops()
        self.host.on_relay(0.1, ["0", "1", "0"])   # typing went to Steam or a game
        self.assertEqual(self.keyboard.sent, [("sink", {"on": False}), ("context", {"context": None, "hints": {}})])
        self.ops()
        self.host.on_relay(0.2, ["1", "1", "0"])
        self.relay.heard = False   # the relay went quiet: gamescope's again
        self.host.tick(0.3)
        self.assertEqual(self.ops()[-2:], ["sink", "context"])
        self.assertEqual(self.host.route, "normal")

    def test_desktop_typing_goes_through_the_relay_and_is_acked(self):
        self.host.on_relay(0.0, ["1", "1", "0"])
        self.host.on_relay(0.0, ["show"])
        self.shown()
        self.ops()
        self.host.on_keyboard(0.1, {"event": "commit", "seq": 1, "context": "desktop", "text": "hé"})
        self.host.on_keyboard(0.1, {"event": "key", "seq": 2, "context": "desktop", "key": "KeyA", "modifiers": ["Ctrl"]})
        self.assertEqual(self.relay.texts, ["hé"])
        self.assertEqual(self.relay.keys, [(29, 1), (30, 1), (30, 0), (29, 0)])
        self.assertEqual(self.keyboard.sent, [("ack", {"context": "desktop", "seq": 1}), ("ack", {"context": "desktop", "seq": 2})])
        self.ops()
        self.host.on_keyboard(0.2, {"event": "commit", "seq": 3, "context": "steam-9", "text": "x"})
        self.assertEqual(self.keyboard.sent, [("ack", {"context": "steam-9", "seq": 3})])   # a stale field: dropped
        self.assertEqual(self.relay.texts, ["hé"])

    def test_a_steam_field_asks_the_relay_then_types_into_steam(self):
        self.host.on_steam(0.0, [("event", {"open": 4, "purpose": "password", "key": "valve.steam.gamepadui.main"})])
        self.assertEqual(self.relay.asked, 1)
        self.assertEqual(self.ops(), [])   # the relay decides first
        self.host.on_relay(0.1, ["show", "dash", *POSE_WORDS])
        self.assertEqual(self.keyboard.sent[:2], [("sink", {"on": True}),
                                                  ("context", {"context": "steam-4", "hints": {"purpose": "password"}})])
        self.shown()
        self.ops()
        self.host.on_keyboard(0.2, {"event": "commit", "seq": 1, "context": "steam-4", "text": "pw"})
        session, text, done = self.steam.typed[0]
        self.assertEqual((session, text), (4, "pw"))
        self.assertEqual(self.keyboard.sent, [])   # acked once Steam says
        self.host.on_steam(0.3, [("done", done, True)])
        self.assertEqual(self.keyboard.sent, [("ack", {"context": "steam-4", "seq": 1})])
        self.ops()
        self.host.on_keyboard(0.3, {"event": "key", "seq": 2, "context": "steam-4", "key": "Backspace", "modifiers": []})
        *_, done = self.steam.pressed[0]
        self.host.on_steam(0.4, [("done", done, False)])
        self.assertEqual(self.keyboard.sent, [("reject", {"context": "steam-4", "seq": 2})])
        self.ops()
        # The field closed in Steam: it hides, as it opened for it.
        self.host.on_steam(0.5, [("event", {"close": 4, "reason": "blur"})])
        self.assertEqual(self.ops(), ["hide"])

    def test_a_steam_field_the_relay_turns_down(self):
        self.host.on_steam(0.0, [("event", {"open": 1, "purpose": "text"})])
        self.host.on_relay(0.1, ["nosteam"])
        self.assertEqual(self.steam.closed, [1])
        self.host.tick(1.0)
        self.assertNotIn("show", self.ops())

    def test_without_ft_screens_a_steam_field_opens_where_it_is(self):
        self.host.on_steam(0.0, [("event", {"open": 1, "purpose": "text"})])
        self.host.tick(hostmod.STEAM_FALLBACK - 0.1)
        self.assertNotIn("show", self.ops())
        self.host.tick(hostmod.STEAM_FALLBACK)
        self.assertEqual(self.ops()[-2:], ["recenter", "show"])
        self.assertEqual(self.host.opened_for, "steam")

    def test_steam_going_away_hides_what_opened_for_it(self):
        self.host.on_steam(0.0, [("event", {"open": 1, "purpose": "text"})])
        self.host.on_relay(0.1, ["show"])
        self.shown()
        self.ops()
        self.host.on_steam(0.2, [("steam", False)])
        self.assertEqual(self.ops(), ["hide"])

    def test_the_pin_lock_hides_it_and_keeps_steam_fields(self):
        self.host.on_relay(0.0, ["1", "1", "1"])
        self.assertTrue(self.steam.take)
        self.host.on_relay(0.0, ["show"])
        self.shown()
        self.ops()
        self.allowed = False
        self.host.tick(1.0)
        self.assertIn("hide", self.ops())
        self.assertFalse(self.steam.take)
        self.host.on_relay(1.1, ["show"])
        self.assertEqual(self.ops(), [])   # opens for nothing while locked

    def test_the_screens_hiding_hide_what_opened_over_them(self):
        self.host.on_relay(0.0, ["1", "1", "0"])
        self.host.on_relay(0.0, ["show"])
        self.shown()
        self.ops()
        self.host.on_relay(0.1, ["1", "0", "0"])
        self.assertIn("hide", self.ops())

    def test_stop_shell_typing_holds_until_the_route_changes(self):
        self.host.on_relay(0.0, ["1", "1", "0"])
        self.host.on_relay(0.0, ["show"])
        self.shown()
        self.ops()
        self.host.on_keyboard(0.1, {"event": "target", "owner": "normal"})
        self.host.tick(0.2)
        self.assertEqual(self.ops(), [])   # still the desktop's route, but the user stopped it
        self.host.on_relay(0.3, ["0", "1", "0"])
        self.host.on_relay(0.4, ["1", "1", "0"])
        self.assertEqual(self.ops()[-2:], ["sink", "context"])
        self.assertTrue(self.host.sink)

    def test_fingertips_while_shown(self):
        self.host.on_relay(0.0, ["poke", "right", "press", "0", "1.2", "-0.7"])
        self.assertEqual(self.ops(), [])   # hidden
        self.host.on_relay(0.0, ["show"])
        self.shown()
        self.host.on_keyboard(0.0, GEOMETRY)
        self.ops()
        self.host.on_relay(0.1, ["poke", "right", "press", "0", "1.2", "-0.7"])
        self.assertEqual(self.ops(), ["pointer_mode", "pointer", "pointer"])

    def test_a_new_keyboard_process_gets_its_state_again(self):
        self.host.on_relay(0.0, ["1", "1", "0"])
        self.host.on_relay(0.0, ["show"])
        self.shown()
        self.ops()
        self.host.on_keyboard(1.0, {"event": "ready"})   # an update's successor
        self.assertFalse(self.host.visible)
        self.host.on_relay(1.1, ["show"])
        self.assertEqual(self.ops(), ["sink", "context", "recenter", "show"])


if __name__ == "__main__":
    unittest.main()
