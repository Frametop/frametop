#!/usr/bin/env python3
"""Isolated tests for session/ft-notifyfwd. Run with the host's /usr/bin/python3.

Two private session buses stand in for the desktop's and the user bus. A fake Plasma
(org.freedesktop.Notifications with org.kde.NotificationManager's watcher API) runs on the
first, a fake notification server on the second, and the real ft-notifyfwd between them.
Nothing touches the real buses, Plasma, or the headset. Requires PyGObject (Gio) and
dbus-daemon, both in SteamOS.
"""
import os
from pathlib import Path
import subprocess
import time
import unittest

from gi.repository import Gio, GLib

ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / "session/ft-notifyfwd"
NAME = "org.freedesktop.Notifications"
PATH = "/org/freedesktop/Notifications"
MANAGER = "org.kde.NotificationManager"

FDO_XML = """<interface name="org.freedesktop.Notifications">
  <method name="Notify"><arg direction="in" type="s"/><arg direction="in" type="u"/>
    <arg direction="in" type="s"/><arg direction="in" type="s"/><arg direction="in" type="s"/>
    <arg direction="in" type="as"/><arg direction="in" type="a{sv}"/><arg direction="in" type="i"/>
    <arg direction="out" type="u"/></method>
  <method name="CloseNotification"><arg direction="in" type="u"/></method>
  <method name="GetServerInformation"><arg direction="out" type="s"/><arg direction="out" type="s"/>
    <arg direction="out" type="s"/><arg direction="out" type="s"/></method>
  <method name="Inhibit"><arg direction="in" type="s"/><arg direction="in" type="s"/>
    <arg direction="in" type="a{sv}"/><arg direction="out" type="u"/></method>
  <method name="UnInhibit"><arg direction="in" type="u"/></method>
  <signal name="ActionInvoked"><arg type="u"/><arg type="s"/></signal>
  <signal name="NotificationClosed"><arg type="u"/><arg type="u"/></signal>
</interface>"""
MANAGER_XML = """<interface name="org.kde.NotificationManager">
  <method name="RegisterWatcher"/><method name="UnRegisterWatcher"/>
  <method name="InvokeAction"><arg direction="in" type="u"/><arg direction="in" type="s"/></method>
</interface>"""
IMAGE = GLib.Variant("(iiibiiay)", (2, 1, 8, True, 8, 4, bytes(range(8))))


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


def settle(seconds=0.3):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        GLib.MainContext.default().iteration(False)
        time.sleep(0.01)


class Bus:
    def __init__(self):
        self.proc = subprocess.Popen(["dbus-daemon", "--session", "--nofork", "--print-address=1"],
                                     stdout=subprocess.PIPE, text=True)
        self.address = self.proc.stdout.readline().strip()

    def connect(self):
        return Gio.DBusConnection.new_for_address_sync(
            self.address, Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT
            | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            self.proc.wait()
        self.proc.stdout.close()


class Server:
    """org.freedesktop.Notifications on a bus: Plasma (watchers, inhibit) or a plain server."""

    def __init__(self, bus, name="fake", plasma=False, first_id=1):
        self.conn = bus.connect()
        self.name, self.plasma, self.next_id = name, plasma, first_id
        self.calls, self.watchers = [], []
        xml = f"<node>{FDO_XML}{MANAGER_XML if plasma else ''}</node>"
        node = Gio.DBusNodeInfo.new_for_xml(xml)
        self.regs = [self.conn.register_object(PATH, i, self.on_call, None, None) for i in node.interfaces]
        self.owned = False
        self.own = Gio.bus_own_name_on_connection(self.conn, NAME, Gio.BusNameOwnerFlags.NONE,
                                                  lambda *_: setattr(self, "owned", True), None)
        iterate_until(lambda: self.owned)

    def stop(self):
        if self.own:
            Gio.bus_unown_name(self.own)
            self.own = 0
        if not self.conn.is_closed():
            self.conn.close_sync(None)

    def methods(self, method):
        return [args for m, args in self.calls if m == method]

    def emit(self, member, signature, args):
        self.conn.emit_signal(None, PATH, NAME, member, GLib.Variant(signature, args))

    def on_call(self, conn, sender, _path, _iface, method, params, inv):
        self.calls.append((method, params))
        if method == "Notify":
            replaces = params.get_child_value(1).get_uint32()
            nid = replaces or self.next_id
            self.next_id += not replaces
            if self.plasma:  # Plasma hands it to its watchers with its own id first
                children = [GLib.Variant.new_uint32(nid), params.get_child_value(0),
                            *(params.get_child_value(i) for i in range(1, 8))]
                for w in self.watchers:
                    conn.call(w, "/NotificationWatcher", "org.kde.NotificationWatcher", "Notify",
                              GLib.Variant.new_tuple(*children), None, Gio.DBusCallFlags.NONE, 1000, None)
            inv.return_value(GLib.Variant("(u)", (nid,)))
        elif method == "CloseNotification":
            nid, = params.unpack()
            for w in self.watchers:
                conn.call(w, "/NotificationWatcher", "org.kde.NotificationWatcher", "CloseNotification",
                          GLib.Variant("(u)", (nid,)), None, Gio.DBusCallFlags.NONE, 1000, None)
            inv.return_value(None)
            self.emit("NotificationClosed", "(uu)", (nid, 3))
        elif method == "GetServerInformation":
            inv.return_value(GLib.Variant("(ssss)", (self.name, "test", "1", "1.2")))
        elif method == "Inhibit":
            inv.return_value(GLib.Variant("(u)", (42,)))
        elif method == "RegisterWatcher":
            self.watchers.append(sender)
            inv.return_value(None)
        elif method == "InvokeAction":
            nid, key = params.unpack()
            inv.return_value(None)
            self.emit("ActionInvoked", "(us)", (nid, key))
        else:
            inv.return_value(None)


class App:
    """An app in the desktop: notifies Plasma, listens for its signals."""

    def __init__(self, bus):
        self.conn = bus.connect()
        self.signals = []
        self.conn.signal_subscribe(NAME, NAME, None, PATH, None, Gio.DBusSignalFlags.NONE,
                                   lambda _c, _s, _p, _i, m, params, _u: self.signals.append((m, params.unpack())),
                                   None)

    def notify(self, summary, replaces=0, actions=(), hints=None):
        result = []
        args = GLib.Variant("(susssasa{sv}i)", ("App", replaces, "app-icon", summary, "body <b>bold</b>",
                                                list(actions), hints or {}, -1))
        self.conn.call(NAME, PATH, NAME, "Notify", args, GLib.VariantType("(u)"), Gio.DBusCallFlags.NONE,
                       2000, None, lambda c, r, _: result.append(c.call_finish(r).unpack()[0]), None)
        iterate_until(lambda: result)
        return result[0]

    def close(self, nid):
        self.conn.call(NAME, PATH, NAME, "CloseNotification", GLib.Variant("(u)", (nid,)), None,
                       Gio.DBusCallFlags.NONE, 2000, None, None, None)


class NotifyForwardTest(unittest.TestCase):
    def setUp(self):
        self.desktop_bus, self.user_bus = Bus(), Bus()
        self.addCleanup(self.desktop_bus.stop)
        self.addCleanup(self.user_bus.stop)
        self.plasma = Server(self.desktop_bus, plasma=True)
        self.addCleanup(self.plasma.stop)
        self.app = App(self.desktop_bus)
        self.forwarder = None

    def start(self, mode="auto"):
        env = dict(os.environ, DBUS_SESSION_BUS_ADDRESS=self.desktop_bus.address,
                   FT_USER_BUS=self.user_bus.address, FT_NOTIFYFWD_TEST="1")
        self.forwarder = subprocess.Popen(["/usr/bin/python3", str(HELPER), mode], env=env)
        self.addCleanup(lambda: self.forwarder.poll() is None and (self.forwarder.kill(), self.forwarder.wait()))
        iterate_until(lambda: self.plasma.watchers)

    def server(self, name="vr-notifyd"):
        s = Server(self.user_bus, name=name, first_id=100)
        self.addCleanup(s.stop)
        return s

    def test_forwards_with_types_actions_and_inhibits(self):
        vr = self.server()
        self.start()
        iterate_until(lambda: self.plasma.methods("Inhibit"))
        pid = self.app.notify("Hello", actions=["default", "Open", "reply", "Reply"],
                              hints={"image-data": IMAGE, "urgency": GLib.Variant("y", 2)})
        iterate_until(lambda: vr.methods("Notify"))
        args = vr.methods("Notify")[0]
        self.assertEqual(args.get_child_value(3).get_string(), "Hello")
        self.assertEqual(args.get_child_value(4).get_string(), "body <b>bold</b>")
        self.assertEqual(args.get_child_value(5).unpack(), ["default", "Open", "reply", "Reply"])
        hints = args.get_child_value(6)
        self.assertEqual(hints.lookup_value("image-data", None).get_type_string(), "(iiibiiay)")
        self.assertEqual(hints.lookup_value("image-data", None).unpack(), IMAGE.unpack())
        self.assertEqual(hints.lookup_value("urgency", None).get_type_string(), "y")
        self.assertEqual(len(self.plasma.methods("Inhibit")), 1)

        vr.emit("ActionInvoked", "(us)", (100, "reply"))
        iterate_until(lambda: ("ActionInvoked", (pid, "reply")) in self.app.signals)
        vr.emit("NotificationClosed", "(uu)", (100, 2))  # dismissed in VR
        iterate_until(lambda: self.plasma.methods("CloseNotification"))
        self.assertEqual(self.plasma.methods("CloseNotification")[0].unpack(), (pid,))

    def test_expired_stays_replace_and_close_map_ids(self):
        vr = self.server()
        self.start()
        pid = self.app.notify("one")
        iterate_until(lambda: vr.methods("Notify"))
        vr.emit("NotificationClosed", "(uu)", (100, 1))  # expired in VR: Plasma keeps it
        settle()
        self.assertFalse(self.plasma.methods("CloseNotification"))

        pid2 = self.app.notify("two")
        iterate_until(lambda: len(vr.methods("Notify")) == 2)
        self.app.notify("two, updated", replaces=pid2)
        iterate_until(lambda: len(vr.methods("Notify")) == 3)
        self.assertEqual(vr.methods("Notify")[2].get_child_value(1).get_uint32(), 101)
        self.app.close(pid2)
        iterate_until(lambda: vr.methods("CloseNotification"))
        self.assertEqual(vr.methods("CloseNotification")[0].unpack(), (101,))
        self.assertNotEqual(pid, pid2)

    def test_auto_skips_steams_daemon_until_replaced(self):
        steam = self.server(name="simple_notif_daemon")
        self.start("auto")
        self.app.notify("lost on steam")
        settle()
        self.assertFalse(steam.methods("Notify"))
        self.assertFalse(self.plasma.methods("Inhibit"))

        steam.stop()
        vr = self.server()
        iterate_until(lambda: self.plasma.methods("Inhibit"))
        self.app.notify("now in VR")
        iterate_until(lambda: vr.methods("Notify"))

        vr.stop()  # no server: Plasma shows its own popups again
        iterate_until(lambda: self.plasma.methods("UnInhibit"))

    def test_plasma_restart_registers_again(self):
        vr = self.server()
        self.start()
        iterate_until(lambda: self.plasma.methods("Inhibit"))
        self.plasma.stop()
        self.plasma = Server(self.desktop_bus, plasma=True)
        self.addCleanup(self.plasma.stop)
        iterate_until(lambda: self.plasma.watchers and self.plasma.methods("Inhibit"))
        self.app.notify("after restart")
        iterate_until(lambda: vr.methods("Notify"))

    def test_exits_with_the_desktop_bus(self):
        self.server()
        self.start()
        self.desktop_bus.stop()
        self.assertEqual(self.forwarder.wait(timeout=5), 0)

    def test_does_nothing_outside_the_frametop_desktop(self):
        env = dict(os.environ, DBUS_SESSION_BUS_ADDRESS=self.desktop_bus.address,
                   FT_USER_BUS=self.user_bus.address, XDG_RUNTIME_DIR="/run/user/1000")
        env.pop("FT_NOTIFYFWD_TEST", None)
        p = subprocess.run(["/usr/bin/python3", str(HELPER)], env=env, timeout=5)
        self.assertEqual(p.returncode, 0)
        self.assertFalse(self.plasma.watchers)


if __name__ == "__main__":
    unittest.main()
