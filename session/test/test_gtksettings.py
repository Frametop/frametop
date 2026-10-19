#!/usr/bin/env python3
"""Isolated tests for session/ft-gtksettings. Run with the host's /usr/bin/python3.

A private session bus stands in for the desktop's, a temporary folder for its config (the
kwinrc the helper reads), and GSettings' memory backend for its dconf database. Nothing
touches the real buses, KWin, or the portal. Requires PyGObject (Gio), dbus-daemon, and
kreadconfig6/kwriteconfig6, all in SteamOS.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

from gi.repository import Gio, GLib

ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / "session/ft-gtksettings"
NAME = "org.freedesktop.impl.portal.desktop.frametop"
PATH = "/org/freedesktop/portal/desktop"
IFACE = "org.freedesktop.impl.portal.Settings"
WM = "org.gnome.desktop.wm.preferences"


def iterate_until(predicate, timeout=5):
    ctx = GLib.MainContext.default()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        while ctx.iteration(False):
            pass
        if predicate():
            return
        time.sleep(0.01)
    raise AssertionError("timed out")


class GtkSettingsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.config = Path(self.tmp.name)
        self.bus = subprocess.Popen(["dbus-daemon", "--session", "--nofork", "--print-address=1"],
                                    stdout=subprocess.PIPE, text=True)
        self.address = self.bus.stdout.readline().strip()
        self.conn = Gio.DBusConnection.new_for_address_sync(
            self.address, Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT
            | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
        self.helper = None

    def tearDown(self):
        if self.helper and self.helper.poll() is None:
            self.helper.terminate()
            self.helper.wait()
        if not self.conn.is_closed():
            self.conn.close_sync(None)
        if self.bus.poll() is None:
            self.bus.terminate()
            self.bus.wait()
        self.bus.stdout.close()
        self.tmp.cleanup()

    def kwin(self, left=None, right=None):
        for key, value in (("ButtonsOnLeft", left), ("ButtonsOnRight", right)):
            if value is not None:
                subprocess.run(["kwriteconfig6", "--file", str(self.config / "kwinrc"),
                                "--group", "org.kde.kdecoration2", "--key", key, value], check=True)

    def start(self):
        env = dict(os.environ, DBUS_SESSION_BUS_ADDRESS=self.address, XDG_CONFIG_HOME=str(self.config),
                   GSETTINGS_BACKEND="memory")
        self.helper = subprocess.Popen(["/usr/bin/python3", str(HELPER)], env=env, stderr=subprocess.DEVNULL)
        owned = []
        Gio.bus_watch_name_on_connection(self.conn, NAME, Gio.BusNameWatcherFlags.NONE,
                                         lambda *_: owned.append(1), None)
        iterate_until(lambda: owned)

    def call(self, method, signature, args):
        return self.conn.call_sync(NAME, PATH, IFACE, method, GLib.Variant(signature, args), None,
                                   Gio.DBusCallFlags.NONE, 5000, None).unpack()

    def test_kdes_default_order(self):
        self.start()  # no kwinrc: KDecoration's MS / HIAX
        self.assertEqual(self.call("ReadAll", "(as)", ([],)),
                         ({WM: {"button-layout": "icon:minimize,maximize,close"}},))
        self.assertEqual(self.call("Read", "(ss)", (WM, "button-layout")), ("icon:minimize,maximize,close",))

    def test_configured_order_and_buttons_gtk_lacks(self):
        self.kwin(left="XIA", right="BFLX")  # keep below (our float button), keep above, shade
        self.start()
        self.assertEqual(self.call("Read", "(ss)", (WM, "button-layout")), ("close,minimize,maximize:close",))

    def test_readall_filter(self):
        self.start()
        for patterns in (["org.gnome.*"], [WM], [""]):
            self.assertIn(WM, self.call("ReadAll", "(as)", (patterns,))[0], patterns)
        for patterns in (["org.kde.*"], ["org.gnome.desktop.interface"]):
            self.assertEqual(self.call("ReadAll", "(as)", (patterns,)), ({},), patterns)

    def test_other_keys_not_found(self):
        self.start()
        with self.assertRaises(GLib.Error) as e:
            self.call("Read", "(ss)", ("org.gnome.desktop.interface", "color-scheme"))
        self.assertIn("org.freedesktop.portal.Error.NotFound", e.exception.message)

    def test_follows_kwinrc_changes(self):
        self.start()
        changes = []
        self.conn.signal_subscribe(NAME, IFACE, "SettingChanged", PATH, None, Gio.DBusSignalFlags.NONE,
                                   lambda *a: changes.append(a[5].unpack()), None)
        self.kwin(right="IX")
        iterate_until(lambda: changes)
        self.assertEqual(changes, [(WM, "button-layout", "icon:minimize,close")])
        self.assertEqual(self.call("Read", "(ss)", (WM, "button-layout")), ("icon:minimize,close",))
        self.kwin(right="IX")  # the same again: no signal
        time.sleep(0.5)
        while GLib.MainContext.default().iteration(False):
            pass
        self.assertEqual(len(changes), 1)

    def test_version(self):
        self.start()
        version = self.conn.call_sync(NAME, PATH, "org.freedesktop.DBus.Properties", "Get",
                                      GLib.Variant("(ss)", (IFACE, "version")), None,
                                      Gio.DBusCallFlags.NONE, 5000, None).unpack()[0]
        self.assertEqual(version, 1)

    def test_exits_with_the_desktop_bus(self):
        self.start()
        self.bus.terminate()
        self.bus.wait()
        self.assertEqual(self.helper.wait(timeout=5), 0)


if __name__ == "__main__":
    unittest.main()
