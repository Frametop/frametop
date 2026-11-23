"""Remote displays wait while the headset is off (ft_remoteaway, ft-layout remote away on|off):
which streams stop, which PCs put their monitors back, and what starts again. ft-screens and
the PCs are stand-ins; no headset.

  python3 layout/tests/test_remote_away.py
"""
import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest

LAYOUT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(LAYOUT.parent / "conf"))
sys.path.insert(0, str(LAYOUT))
import ft_hostmonitors as hm  # noqa: E402


def hosts():
    return [{"name": "PC", "address": "192.0.2.7", "monitors": True, "displays": [
                {"id": "pc-edp", "client": "c1", "app": "display:eDP-2", "size": [2560, 1600]},
                {"id": "pc-dp", "client": "c2", "app": "display:DP-1", "size": [3840, 2160], "off": True},
                {"id": "pc-virtual", "client": "c3", "app": "monitor", "size": [1920, 1080]}]},
            {"name": "Mac", "address": "192.0.2.9", "kind": "apple", "displays": [
                {"id": "mac-main", "client": "c4", "app": "apple:physical", "port": 47989, "size": [3024, 1964]}]},
            {"name": "Idle", "address": "192.0.2.8", "monitors": True, "displays": [
                {"id": "idle-dp", "client": "c5", "app": "display:DP-1", "size": [1920, 1080], "off": True}]}]


class Screens:
    """ft-screens' socket: the remote screens it runs, and what each start answers."""
    def __init__(self, running, starts=None):
        self.running, self.starts, self.asked = running, starts or {}, []

    def ask(self, text, timeout=10.0):
        self.asked.append(text)
        if text == "remotes":
            return f"ok {len(self.running)}" + "".join(f" {n}:c:live:1x1" for n in self.running)
        words = text.split()
        if words[0] == "remote" and words[2] == "start":
            reply = self.starts.get(int(words[1]), "ok")
            if reply.startswith("error"):
                raise RuntimeError(reply[6:])
            return reply
        return "ok"


class Away(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        spec = importlib.util.spec_from_file_location("remote_away_layout", LAYOUT / "ft_layout.py")
        self.m = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = self.m   # (remote away hands itself to ft_remoteaway by its name)
        self.addCleanup(sys.modules.pop, spec.name, None)
        spec.loader.exec_module(self.m)
        self.m.CONF_PATH = str(pathlib.Path(self.tmp.name) / "frametop.conf")
        self.m.LAYOUT_PATH = str(pathlib.Path(self.tmp.name) / "layout.json")
        self.m.save_layout({"hosts": hosts()})
        self.said, self.put_back, self.synced, self.placed = [], [], [], []
        self.m.log = lambda *a: self.said.append(" ".join(str(x) for x in a))
        self.screens = Screens({101, 103, 104, 115})   # 115: a Together room's panel, not in the layout
        self.m.screens_socket = lambda: self.screens
        self.m.remote_anchor = lambda sock, layout: ((0, 1.6, 0), 0.0)
        self.m.place_remotes = lambda sock, layout, count, eye, heading, only=None: self.placed.append(only)
        for name, fn in (("put_back_all", lambda hs, log, request=None: self.put_back.append([h["name"] for h in hs])),
                         ("sync_all", lambda hs, log, request=None: self.synced.append(
                             sorted({h["name"] for h in hs if hm.controls(h)})) or set())):
            self.addCleanup(setattr, hm, name, getattr(hm, name))
            setattr(hm, name, fn)

    def layout(self):
        with open(self.m.LAYOUT_PATH) as f:
            return json.load(f)

    def test_away_stops_the_connected_displays_and_puts_the_monitors_back(self):
        before = self.layout()
        self.m.remote_command(["away", "on"])
        self.assertEqual(self.screens.asked, ["remotes", "remote 101 stop", "remote 103 stop", "remote 104 stop"],
                         "not the disconnected one's number, nor a room's panel")
        self.assertEqual(self.put_back, [["PC"]], "the host with a connected monitor panel; not one with none")
        self.assertEqual(self.layout(), before, "they stay connected: off is the user's")
        self.assertIn("pc-edp: stopped while the headset is off", self.said)

    def test_away_with_no_desktop_still_puts_the_monitors_back(self):
        def gone():
            raise self.m.NoAnswer("ft-screens isn't running")
        self.m.screens_socket = gone
        self.m.remote_command(["away", "on"])
        self.assertEqual(self.put_back, [["PC"]])
        self.assertEqual(self.said, ["not stopped now: ft-screens isn't running"])

    def test_back_sends_the_plans_and_starts_them_where_they_were(self):
        self.screens = Screens(set(), {101: "ok restored", 103: "ok", 104: "ok running"})
        self.m.remote_command(["away", "off"])
        self.assertEqual(self.synced, [["PC"]], "before the streams start")
        self.assertEqual([a.split()[1] for a in self.screens.asked if " start " in a], ["101", "103", "104"])
        self.assertEqual(self.placed, [{"pc-virtual"}], "only the one with a new panel: the others are where they were")

    def test_back_goes_on_past_one_that_doesnt_start(self):
        self.screens = Screens(set(), {101: "error too many remote screens", 103: "ok restored", 104: "ok restored"})
        self.m.remote_command(["away", "off"])
        self.assertEqual(self.said, ["remote display pc-edp: too many remote screens"])
        self.assertEqual(len([a for a in self.screens.asked if " start " in a]), 3)
        self.assertEqual(self.placed, [])

    def test_back_with_nothing_connected_does_nothing(self):
        layout = self.layout()
        for h in layout["hosts"]:
            for d in h["displays"]:
                d["off"] = True
        self.m.save_layout(layout)
        self.m.remote_command(["away", "off"])
        self.assertEqual((self.synced, self.screens.asked), ([], []))

    def test_anything_else_is_the_usage(self):
        with self.assertRaisesRegex(RuntimeError, "remote away on\\|off"):
            self.m.remote_command(["away"])


if __name__ == "__main__":
    unittest.main()
