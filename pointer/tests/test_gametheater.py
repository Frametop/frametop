"""View in Theater from a game's panel: the ask, and waiting for SteamVR's frame."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("gametheater", ROOT / "pointer/dashpatch/gametheater.py")
gametheater = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gametheater)


class AskTests(unittest.TestCase):
    def test_the_app_id(self):
        self.assertEqual(gametheater.ask(b"gametheater 2868840"), 2868840)

    def test_not_an_ask(self):
        for msg in (b"gametheater", b"gametheater 0", b"gametheater 012", b"gametheater 1x",
                    b"gametheater 99999999999", b"gametheater 5 6", b"steamtheater", b"gametheater -1"):
            self.assertIsNone(gametheater.ask(msg), msg)


class ExpressionTests(unittest.TestCase):
    def test_it_names_the_games_overlay_and_asks_as_steamvrs_button_does(self):
        e = gametheater.expression(2868840)
        self.assertIn("'valve.steam.desktopgame.2868840'", e)
        self.assertIn("f.docking.SetInTheater()", e)
        self.assertIn("m_rgTheaterFrameStack", e)


class WaitTests(unittest.TestCase):
    def test_waits_for_the_frame(self):
        replies = iter(["none", "none", "in"])
        slept = []
        said = gametheater.put_in_theater(7, lambda e: next(replies), slept.append)
        self.assertEqual(said, "game 7 in SteamVR's theater")
        self.assertEqual(len(slept), 2)

    def test_gives_up(self):
        slept = []
        said = gametheater.put_in_theater(7, lambda e: "none", slept.append)
        self.assertIn("no frame for game 7", said)
        self.assertAlmostEqual(sum(slept), gametheater.WAIT_S)


if __name__ == "__main__":
    unittest.main()
