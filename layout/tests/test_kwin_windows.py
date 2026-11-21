"""ft-layout remote kwin-*: a KDE PC's windows from KWin Window Stream (issue 81), without a
headset or a host. A window panel uses one of its host's displays' paired clients (never pairs
one), removing it keeps that pairing while another panel uses it, and a PC-side resize is saved.
ft-stream and ft-screens are fakes that record."""
import importlib
import importlib.util
import io
import json
import pathlib
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

HERE = pathlib.Path(__file__).resolve().parents[1]
UUID = "{8d3e0c52-5b1f-4c3e-9a57-1f2e3d4c5b6a}"
WINDOWS = [{"id": UUID, "title": "Kate\x07 notes", "app": "org.kde.kate", "width": 1282, "height": 802,
            "minimized": False}]


def load_modules(tmp):
    """(ft_layout, ft_kwinwindows): a fresh ft_layout of the test's own, and the module bound to it."""
    spec = importlib.util.spec_from_file_location("kwin_layout", HERE / "ft_layout.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.CONF_PATH = str(tmp / "frametop.conf")  # none: the screens backend
    module.LAYOUT_PATH = str(tmp / "layout.json")
    sys.path.insert(0, str(HERE))
    import ft_kwinwindows
    kwin = importlib.reload(ft_kwinwindows)
    kwin.L = module
    return module, kwin


class KwinWindows(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.m, self.k = load_modules(pathlib.Path(self.tmp.name))
        self.streams = []
        self.taken = {"frametop-kde-desktop"}  # the clients the PC's service takes

        def fake_stream(*args, timeout=60, stdin=None):
            self.streams.append(args)
            if args[0] == "kwin-windows":
                if args[3] in self.taken:
                    return 0, "host 192.0.2.40: 2.0, paired\n" + json.dumps({"version": 1, "windows": WINDOWS}), ""
                return 1, "", "kwin windows: the TLS handshake failed (is this client paired with its Sunshine?)"
            return 0, "unpaired" if args[0] == "unpair" else "ok", ""

        def no_screens():
            raise self.m.NoAnswer("ft-screens isn't running")

        self.m.run_stream = fake_stream
        self.m.screens_socket = no_screens
        self.m.log = lambda *args: None  # the log goes to stdout, after the id
        self.m.save_layout({"hosts": [{"name": "KDE", "address": "192.0.2.40", "kind": "sunshine", "displays": [
            {"id": "kde-old", "client": "frametop-kde-old", "app": "Desktop", "label": "Old", "size": [1920, 1080]},
            {"id": "kde-desktop", "client": "frametop-kde-desktop", "app": "Desktop", "label": "Desktop",
             "size": [2560, 1440]}]}]})

    def tearDown(self):
        self.tmp.cleanup()

    def run_layout(self, *args):
        out = io.StringIO()
        with redirect_stdout(out):
            self.m.remote_command(list(args))
        return out.getvalue()

    def host(self):
        return self.m.load_layout()["hosts"][0]

    def test_the_list_comes_with_a_client_the_pc_takes(self):
        answer = json.loads(self.run_layout("kwin-windows", "KDE").strip().splitlines()[-1])
        self.assertEqual((answer["client"], [w["id"] for w in answer["windows"]]), ("frametop-kde-desktop", [UUID]))
        self.assertEqual([c[3] for c in self.streams], ["frametop-kde-old", "frametop-kde-desktop"])

    def test_only_this_hosts_clients_are_tried(self):
        self.m.save_layout({"hosts": [{"name": "KDE", "address": "192.0.2.40", "displays": []}]})
        with self.assertRaisesRegex(RuntimeError, "Add one of this PC's displays first"):
            self.run_layout("kwin-windows", "KDE")
        self.assertEqual(self.streams, [])

    def test_none_taken_says_why(self):
        self.taken = set()
        with self.assertRaisesRegex(RuntimeError, "TLS handshake failed"):
            self.run_layout("kwin-windows", "KDE")

    def test_a_short_poll_takes_turns(self):
        # One that's refused can't take every poll's time: the next poll starts after it.
        cursor = {}
        self.taken = set()
        with self.assertRaises(RuntimeError):
            self.k.find_client(self.host(), timeout=20, cursor=cursor)
        self.taken = {"frametop-kde-old"}
        self.streams.clear()
        client, _ = self.k.find_client(self.host(), timeout=20, cursor=cursor)
        self.assertEqual(client, "frametop-kde-old")
        self.assertEqual(cursor["next"], "frametop-kde-old", "the one taken goes first next time")

    def test_adding_a_window_uses_a_paired_client_and_never_pairs(self):
        out = self.run_layout("kwin-add", "KDE", UUID.upper(), "--label", WINDOWS[0]["title"], "--size", "1282x802")
        d = next(d for d in self.host()["displays"] if d["app"].startswith("kwin:"))
        self.assertEqual(out.strip().splitlines()[-1], d["id"])
        self.assertEqual((d["app"], d["client"], d["label"], d["size"]),
                         ("kwin:" + UUID, "frametop-kde-desktop", "Kate notes", [1282, 802]))
        self.assertFalse(any(c[0] in ("pair", "unpair") for c in self.streams))

    def test_the_same_window_again_is_its_panel(self):
        first = self.run_layout("kwin-add", "KDE", UUID, "--client", "frametop-kde-desktop").strip().splitlines()[-1]
        self.streams.clear()
        again = self.run_layout("kwin-add", "KDE", UUID).strip().splitlines()[-1]
        self.assertEqual(again, first)
        self.assertEqual(len([d for d in self.host()["displays"] if d["app"].startswith("kwin:")]), 1)
        self.assertEqual(self.streams, [], "a client given that's the host's is used without asking")

    def test_a_bad_window_id_or_client_is_refused(self):
        with self.assertRaisesRegex(RuntimeError, "isn't a KWin window's id"):
            self.run_layout("kwin-add", "KDE", "0x55aa10")
        self.run_layout("kwin-add", "KDE", UUID, "--client", "frametop-someone-else")
        d = next(d for d in self.host()["displays"] if d["app"].startswith("kwin:"))
        self.assertEqual(d["client"], "frametop-kde-desktop", "another host's client is never sent")

    def test_a_resize_on_the_pc_is_kept(self):
        self.run_layout("kwin-add", "KDE", UUID, "--size", "1280x800")
        n = next(n for n, _, d in self.m.remote_displays(self.m.load_layout()) if d["app"].startswith("kwin:"))
        self.run_layout("kwin-size", str(n), "1600x900", "1.1429")
        d = next(d for d in self.host()["displays"] if d["app"].startswith("kwin:"))
        self.assertEqual((d["size"], d["metres"]), ([1600, 900], 1.1429))
        with self.assertRaisesRegex(RuntimeError, "isn't a KWin window's panel"):
            self.run_layout("kwin-size", str(n - 1), "1600x900", "1.0")
        with self.assertRaisesRegex(RuntimeError, "0.15 to 12 metres"):
            self.run_layout("kwin-size", str(n), "1600x900", "nan")

    def test_removing_a_window_keeps_the_pairing_its_display_uses(self):
        window = self.run_layout("kwin-add", "KDE", UUID).strip().splitlines()[-1]
        self.streams.clear()
        self.run_layout("remove", window)
        self.assertEqual(self.streams, [], "the desktop display still uses that client")
        self.run_layout("remove", "kde-desktop")
        self.assertEqual(self.streams[-1][:4], ("unpair", "192.0.2.40", "--id", "frametop-kde-desktop"))


class RestoredVisibility(unittest.TestCase):
    """A panel shown or hidden while disconnected comes back that way (utrack's fix, issue 81's
    branch): ft-screens brings back the parked panel's old hidden flag with its place."""

    def test_the_saved_flag_wins(self):
        m, _ = load_modules(pathlib.Path(tempfile.mkdtemp(prefix="ft-kwin-test-")))
        for hidden in (True, False):
            asked = []

            class Screens:
                def ask(self, text):
                    asked.append(text)
                    return "ok restored" if text.startswith("remote 101 start ") else "ok"

            d = {"id": "kde-desktop", "client": "c", "app": "Desktop", "size": [1920, 1080], "hidden": hidden}
            self.assertEqual(m.start_remote(Screens(), 101, {"name": "KDE", "address": "192.0.2.40"}, d), "ok restored")
            self.assertEqual(asked[1:], ["conceal 101" if hidden else "reveal 101"])

    def test_a_new_panel_is_left_to_its_placing(self):
        m, _ = load_modules(pathlib.Path(tempfile.mkdtemp(prefix="ft-kwin-test-")))
        asked = []

        class Screens:
            def ask(self, text):
                asked.append(text)
                return "ok"

        m.start_remote(Screens(), 101, {"name": "KDE", "address": "192.0.2.40"},
                       {"id": "x", "client": "c", "app": "Desktop", "size": [1920, 1080], "hidden": True})
        self.assertEqual(len(asked), 1)


if __name__ == "__main__":
    unittest.main()
