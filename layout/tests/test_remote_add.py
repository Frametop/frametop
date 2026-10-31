"""ft-layout remote add, without a headset or a host: a bad option fails before any pairing, and a
client paired for a display that then isn't saved is unpaired. ft-stream is a fake that records."""
import importlib.util
import io
import json
import pathlib
import tempfile
import unittest
from contextlib import redirect_stdout


def load_layout_module(tmp):
    path = pathlib.Path(__file__).resolve().parents[1] / "ft_layout.py"
    spec = importlib.util.spec_from_file_location("remote_add_layout", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.CONF_PATH = str(tmp / "frametop.conf")  # none: the screens backend
    module.LAYOUT_PATH = str(tmp / "layout.json")
    return module


class RemoteAdd(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.m = load_layout_module(pathlib.Path(self.tmp.name))
        self.calls = []
        self.unpair_code = 0

        def fake_stream(*args, timeout=60, stdin=None):  # never a real ft-stream or host
            self.calls.append(args)
            if args[0] == "unpair":
                return self.unpair_code, "" if self.unpair_code else "unpaired", "no host"
            return 0, "paired", ""

        def no_screens():
            raise self.m.NoAnswer("ft-screens isn't running")

        self.m.run_stream = fake_stream
        self.m.screens_socket = no_screens
        self.m.save_layout({"hosts": []})

    def tearDown(self):
        self.tmp.cleanup()

    def add(self, *opts):
        out = io.StringIO()
        with redirect_stdout(out):
            self.m.remote_command(["add", "oma7", "192.168.1.30", "Desktop", *opts])
        return out.getvalue()

    def hosts(self):
        with open(self.m.LAYOUT_PATH) as f:
            return json.load(f)["hosts"]

    def test_adds_a_display(self):
        self.add("--fps", "90", "--bitrate", "40000", "--size", "1920x1080")
        self.assertEqual([c[0] for c in self.calls], ["pair"])
        d = self.hosts()[0]["displays"][0]
        self.assertEqual((d["fps"], d["bitrate"], d["size"]), (90, 40000, [1920, 1080]))

    def test_bad_options_never_pair(self):
        for opts in (("--fps", "60fps"), ("--fps", "-1"), ("--bitrate", "50M"), ("--bitrate", "1.5"),
                     ("--size", "big"), ("--size", "1920x")):
            with self.subTest(opts=opts):
                with self.assertRaisesRegex(RuntimeError, f"remote add: {opts[0]} is"):
                    self.add(*opts)
                self.assertEqual(self.calls, [])
                self.assertEqual(self.hosts(), [])

    def test_failure_after_pairing_unpairs(self):
        def broken_save(layout):
            raise self.m.LayoutFileError("the layout file doesn't read")

        self.m.save_layout = broken_save
        with self.assertRaises(self.m.LayoutFileError):
            self.add("--fps", "60")
        self.assertEqual([c[0] for c in self.calls], ["pair", "unpair"])
        self.assertEqual(self.calls[1], ("unpair", "192.168.1.30", "--id", "frametop-oma7-desktop"))
        self.assertEqual(self.hosts(), [])

    def test_a_failed_unpair_is_said(self):
        self.unpair_code = 1

        def full_disk(layout):
            raise OSError("disk full")

        self.m.save_layout = full_disk
        out = io.StringIO()
        with redirect_stdout(out), self.assertRaisesRegex(OSError, "disk full"):
            self.m.remote_command(["add", "oma7", "192.168.1.30", "Desktop", "--port", "48000"])
        self.assertEqual(self.calls[1][:2], ("unpair", "192.168.1.30:48000"))
        self.assertIn("unpairing frametop-oma7-desktop from 192.168.1.30 failed too: no host", out.getvalue())


if __name__ == "__main__":
    unittest.main()
