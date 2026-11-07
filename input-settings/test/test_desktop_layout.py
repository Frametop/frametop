"""desktop_layout: the Keyboard page's desktop layout (kxkbrc), offline, in a temporary folder.

Covers reading xkeyboard-config's layouts, putting a layout first in kxkbrc without losing the
rest of the file, what KWin and the input relay then read from it, finding the running desktop's
bus from a fake /proc, and the reload signal's command. Nothing is sent to a real bus.
Run: python3 input-settings/test/test_desktop_layout.py"""
import importlib.util
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "input-settings"))
import desktop_layout as dl  # noqa: E402

EVDEV = """<?xml version="1.0" encoding="UTF-8"?>
<xkbConfigRegistry version="1.1">
  <layoutList>
    <layout><configItem><name>pl</name><description>Polish</description></configItem>
      <variantList>
        <variant><configItem><name>qwertz</name><description>Polish (QWERTZ)</description></configItem></variant>
        <variant><configItem><name>legacy</name><description>Polish (legacy)</description></configItem></variant>
      </variantList>
    </layout>
    <layout><configItem><name>de</name><description>German</description></configItem></layout>
    <layout><configItem><name>us</name><description>English (US)</description></configItem></layout>
    <layout><configItem><name>al</name><description>Albanian</description></configItem></layout>
  </layoutList>
</xkbConfigRegistry>
"""


def relay_reader():
    """input-relay.py's kxkbrc_layout, loaded without running the relay (as roles-test does)."""
    path = ROOT / "input" / "input-relay.py"
    relay = importlib.util.module_from_spec(importlib.util.spec_from_loader("relay", loader=None))
    relay.__file__ = str(path)
    sys.path.insert(0, str(path.parent))
    exec(compile(path.read_text(), str(path), "exec"), relay.__dict__)
    return relay.kxkbrc_layout


class DesktopLayout(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(prefix="ft-desktop-layout-"))
        self.rc = self.dir / "frametop" / "kxkbrc"
        self.xml = self.dir / "evdev.xml"
        self.xml.write_text(EVDEV)

    def test_rules_choices(self):
        rules = dl.read_rules([str(self.dir / "missing.xml"), str(self.xml)])
        self.assertEqual(sorted(rules), ["al", "de", "pl", "us"])
        self.assertEqual([i["value"] for i in dl.layout_choices(rules)], ["us", "al", "de", "pl"])
        self.assertEqual(dl.variant_choices(rules, "pl"), [
            {"value": "", "text": "Polish"}, {"value": "qwertz", "text": "Polish (QWERTZ)"},
            {"value": "legacy", "text": "Polish (legacy)"}])
        self.assertEqual(dl.variant_choices(rules, "de"), [{"value": "", "text": "German"}])
        self.assertEqual(dl.read_rules([str(self.dir / "missing.xml")]), {})

    def test_host_rules_exist(self):
        rules = dl.read_rules()
        if not rules:
            self.skipTest("no evdev.xml here")
        self.assertIn("pl", rules)
        self.assertEqual(rules["us"][0], "English (US)")

    def test_current_defaults(self):
        self.assertEqual(dl.current(str(self.rc)), ("us", ""))
        self.rc.parent.mkdir()
        self.rc.write_text("[Layout]\nLayoutList=de,pl\nUse=false\nVariantList=nodeadkeys,\n")
        self.assertEqual(dl.current(str(self.rc)), ("us", ""))
        self.rc.write_text("[Layout]\nLayoutList=de,pl\nUse=true\nVariantList=nodeadkeys,\n")
        self.assertEqual(dl.current(str(self.rc)), ("de", "nodeadkeys"))

    def test_new_file(self):
        dl.put_first("pl", "", str(self.rc))
        self.assertEqual(self.rc.read_text(), "[Layout]\nLayoutList=pl\nUse=true\nVariantList=\n")
        self.assertEqual(dl.current(str(self.rc)), ("pl", ""))
        self.assertFalse(list(self.rc.parent.glob("*.ft-tmp")))

    def test_keeps_the_rest(self):
        self.rc.parent.mkdir()
        self.rc.write_text("[$Version]\nupdate_info=kxkb.upd:remove-empty-lists\n\n"
                           "[Layout]\nDisplayNames=,DE\nLayoutList=us,de\nModel=pc105\n"
                           "Options=grp:alt_shift_toggle\nResetOldOptions=true\nUse=true\n"
                           "VariantList=,nodeadkeys\n\n[Other]\nKey=1\n")
        dl.put_first("pl", "", str(self.rc))
        text = self.rc.read_text()
        self.assertIn("[$Version]\nupdate_info=kxkb.upd:remove-empty-lists\n", text)
        self.assertIn("LayoutList=pl,us,de\n", text)
        self.assertIn("VariantList=,,nodeadkeys\n", text)
        self.assertIn("DisplayNames=,,DE\n", text)
        self.assertIn("Options=grp:alt_shift_toggle\nResetOldOptions=true\n", text)
        self.assertTrue(text.endswith("\n\n[Other]\nKey=1\n"), text)
        # picking one already in the list moves it to the front, with its display name dropped
        dl.put_first("de", "nodeadkeys", str(self.rc))
        text = self.rc.read_text()
        self.assertIn("LayoutList=de,pl,us\n", text)
        self.assertIn("VariantList=nodeadkeys,,\n", text)
        self.assertIn("DisplayNames=,,\n", text)
        self.assertEqual(dl.current(str(self.rc)), ("de", "nodeadkeys"))
        # the same layout with another variant is another entry
        dl.put_first("de", "", str(self.rc))
        self.assertIn("LayoutList=de,de,pl,us\n", self.rc.read_text())

    def test_switched_off_lists_go(self):
        self.rc.parent.mkdir()
        self.rc.write_text("[Layout]\nLayoutList=de,fr\nUse=false\nVariantList=,\n")
        dl.put_first("pl", "qwertz", str(self.rc))
        self.assertEqual(self.rc.read_text(), "[Layout]\nLayoutList=pl\nUse=true\nVariantList=qwertz\n")

    def test_group_added_after_others(self):
        self.rc.parent.mkdir()
        self.rc.write_text("[$Version]\nupdate_info=x\n")
        dl.put_first("pl", "", str(self.rc))
        self.assertEqual(self.rc.read_text(),
                         "[$Version]\nupdate_info=x\n\n[Layout]\nLayoutList=pl\nUse=true\nVariantList=\n")

    def test_relay_reads_it(self):
        read = relay_reader()
        dl.put_first("pl", "", str(self.rc))
        self.assertEqual(read(str(self.rc))[:2], ("pl", ""))
        dl.put_first("de", "nodeadkeys", str(self.rc))
        self.assertEqual(read(str(self.rc))[:2], ("de", "nodeadkeys"))

    def fake_proc(self, pid, comm, env):
        d = self.dir / "proc" / str(pid)
        d.mkdir(parents=True)
        (d / "comm").write_text(comm + "\n")
        (d / "environ").write_bytes(b"\0".join(f"{k}={v}".encode() for k, v in env.items()) + b"\0")

    def test_desktop_env(self):
        proc = self.dir / "proc"
        cfg = str(self.dir / "frametop")
        self.assertEqual(dl.desktop_env(str(self.dir / "noproc"), cfg), {})
        self.fake_proc(10, "plasmashell", {"XDG_CONFIG_HOME": "/home/x/.config", "DBUS_SESSION_BUS_ADDRESS": "host"})
        self.fake_proc(11, "kwin_wayland", {"XDG_CONFIG_HOME": cfg, "DBUS_SESSION_BUS_ADDRESS": "kwin"})
        (proc / "self").mkdir()
        self.assertEqual(dl.desktop_env(str(proc), cfg), {})
        self.fake_proc(12, "plasmashell", {"XDG_CONFIG_HOME": cfg + "/", "DBUS_SESSION_BUS_ADDRESS": "unix:path=/tmp/dbus-x,guid=1",
                                           "WAYLAND_DISPLAY": "wayland-1"})
        env = dl.desktop_env(str(proc), cfg)
        self.assertEqual(env["DBUS_SESSION_BUS_ADDRESS"], "unix:path=/tmp/dbus-x,guid=1")
        self.assertEqual(dl.desktop_env(str(proc), cfg, uid=os.getuid() + 1), {})  # another user's

    def test_reload(self):
        calls = []

        def run(cmd, **kw):
            calls.append(cmd)
            return subprocess.CompletedProcess(cmd, 0)
        self.assertFalse(dl.reload({}, run=run))
        self.assertEqual(calls, [])
        self.assertTrue(dl.reload({"DBUS_SESSION_BUS_ADDRESS": "unix:path=/tmp/b"}, run=run))
        self.assertEqual(calls, [["gdbus", "emit", "--address", "unix:path=/tmp/b", "--object-path", "/Layouts",
                                  "--signal", "org.kde.keyboard.reloadConfig"]])
        self.assertFalse(dl.reload({"DBUS_SESSION_BUS_ADDRESS": "b"},
                                   run=lambda cmd, **kw: subprocess.CompletedProcess(cmd, 1)))

        def missing(cmd, **kw):
            raise FileNotFoundError(cmd[0])
        self.assertFalse(dl.reload({"DBUS_SESSION_BUS_ADDRESS": "b"}, run=missing))

    def test_settings_command(self):
        self.assertEqual(dl.settings_command({"B": "2", "A": "1"}),
                         ["env", "-i", "A=1", "B=2", "systemsettings", "kcm_keyboard"])


if __name__ == "__main__":
    unittest.main()
