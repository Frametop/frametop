"""ft-layout remote host NAME ...=...: a host's settings in the layout, without a headset."""
import importlib.util
import io
import json
import pathlib
import tempfile
import unittest
from contextlib import redirect_stdout


def load_layout_module(tmp):
    path = pathlib.Path(__file__).resolve().parents[1] / "ft_layout.py"
    spec = importlib.util.spec_from_file_location("remote_host_layout", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.CONF_PATH = str(tmp / "frametop.conf")  # none: the screens backend
    module.LAYOUT_PATH = str(tmp / "layout.json")
    return module


class RemoteHost(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.m = load_layout_module(pathlib.Path(self.tmp.name))
        self.asked = []

        def no_screens():  # ft-screens isn't running; anything that asks it is recorded
            self.asked.append("screens_socket")
            raise self.m.NoAnswer("ft-screens isn't running")

        self.m.screens_socket = no_screens
        self.m.save_layout({"hosts": [{"name": "oma7", "address": "192.0.2.30", "route": "auto",
                                       "displays": [{"id": "oma7-dp-2", "client": "frametop-1", "app": "Desktop"}]}]})

    def tearDown(self):
        self.tmp.cleanup()

    def host(self):
        with open(self.m.LAYOUT_PATH) as f:
            return json.load(f)["hosts"][0]

    def test_input_on_and_off(self):
        self.m.remote_command(["host", "oma7", "input=on"])
        self.assertIs(self.host()["input"], True)
        self.m.remote_command(["host", "oma7", "input=off"])
        self.assertIs(self.host()["input"], False)

    def test_input_needs_no_restart(self):
        # ft-stream reads the switch from the layout every 3 s: nothing asks ft-screens to start over.
        self.m.remote_command(["host", "oma7", "input=on", "mic=on"])
        self.assertEqual(self.asked, [])
        self.assertIs(self.host()["input"], True)
        self.assertIs(self.host()["mic"], True)

    def test_bad_values_save_nothing(self):
        for bad in ("input=yes", "input=", "input", "inputs=on"):
            with self.assertRaisesRegex(RuntimeError, "input=on\\|off"):
                self.m.remote_command(["host", "oma7", "input=on", bad])
            self.assertNotIn("input", self.host())

    def test_unknown_host(self):
        with self.assertRaisesRegex(RuntimeError, "no host called 'desk-pc'"):
            self.m.remote_command(["host", "desk-pc", "input=on"])

    def test_list_says_input(self):
        out = io.StringIO()
        with redirect_stdout(out):
            self.m.remote_command(["list"])
        self.assertNotIn(" input", out.getvalue())
        self.m.remote_command(["host", "oma7", "input=on"])
        out = io.StringIO()
        with redirect_stdout(out):
            self.m.remote_command(["list"])
        self.assertIn("route auto input", out.getvalue())

    def test_clipboard_on_and_off(self):
        # Off until asked for (the shared clipboard, stream/clipboard.h); no restart either way.
        self.assertNotIn("clipboard", self.host())
        self.m.remote_command(["host", "oma7", "clipboard=on"])
        self.assertIs(self.host()["clipboard"], True)
        self.assertEqual(self.asked, [])
        self.m.remote_command(["host", "oma7", "clipboard=off"])
        self.assertIs(self.host()["clipboard"], False)
        for bad in ("clipboard=yes", "clipboard", "clipboards=on"):
            with self.assertRaisesRegex(RuntimeError, "clipboard=on\\|off"):
                self.m.remote_command(["host", "oma7", bad])
        self.assertIs(self.host()["clipboard"], False)

    def test_list_says_clipboard(self):
        self.m.remote_command(["host", "oma7", "input=on", "clipboard=on"])
        out = io.StringIO()
        with redirect_stdout(out):
            self.m.remote_command(["list"])
        self.assertIn("route auto input clipboard", out.getvalue())


if __name__ == "__main__":
    unittest.main()
