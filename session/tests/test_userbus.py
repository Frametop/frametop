#!/usr/bin/env python3
"""session/ft-userbus (DEE-327, issue 98): systemctl, systemd-run and podman in the Frametop
desktop reach the user's systemd and containers. Run under a tool's name, with the desktop's
private XDG_RUNTIME_DIR, bus, config and state folders, it runs that tool (a stub here,
FT_USERBUS_BIN) with the real runtime folder and bus, no XDG_CONFIG_HOME or XDG_STATE_HOME,
and the same arguments. The session links the tools to it in its own bin folder, ahead of
the rest of PATH, and nothing else in the desktop's environment changes.

  python3 session/tests/test_userbus.py
"""
import os
import re
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(os.path.realpath(__file__))
USERBUS = os.path.realpath(os.path.join(HERE, "..", "ft-userbus"))
SESSION = os.path.join(HERE, "..", "frametop-session.sh")
STUB = '#!/bin/sh\necho "$0|$XDG_RUNTIME_DIR|$DBUS_SESSION_BUS_ADDRESS|$WAYLAND_DISPLAY|$*"\n'
# What a tool sees of the desktop's own config and state folders ("unset" when it sees none).
FOLDERS = '#!/bin/sh\necho "${XDG_CONFIG_HOME-unset}|${XDG_STATE_HOME-unset}|${XDG_DATA_HOME-unset}"\n'
TOOLS = ("systemctl", "systemd-run", "podman")


class UserBus(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        d = self.dir.name
        self.real, self.bin = os.path.join(d, "usr-bin"), os.path.join(d, "frametop-bin")
        os.mkdir(self.real)
        os.mkdir(self.bin)
        for tool in TOOLS:
            with open(os.path.join(self.real, tool), "w") as f:
                f.write(STUB)
            os.chmod(os.path.join(self.real, tool), 0o755)
            os.symlink(USERBUS, os.path.join(self.bin, tool))
        private = f"/run/user/{os.getuid()}/frametop"
        self.env = dict(os.environ, FT_USERBUS_BIN=self.real, XDG_RUNTIME_DIR=private,
                        DBUS_SESSION_BUS_ADDRESS=f"unix:path={private}/bus,guid=0",
                        XDG_CONFIG_HOME=os.path.join(d, "config", "frametop"),
                        XDG_STATE_HOME=os.path.join(d, "state", "frametop"),
                        XDG_DATA_HOME=os.path.join(d, "data"),
                        WAYLAND_DISPLAY="wayland-0", PATH=f"{self.bin}:{os.environ['PATH']}")

    def run_tool(self, *args):
        p = subprocess.run(list(args), env=self.env, capture_output=True, text=True, timeout=10)
        self.assertEqual(p.returncode, 0, p.stderr)
        return p.stdout.strip().split("|")

    def test_systemctl_user_gets_the_real_runtime_dir_and_bus(self):
        real = f"/run/user/{os.getuid()}"
        prog, runtime, bus, wayland, args = self.run_tool("systemctl", "--user", "enable", "--now", "ssh-agent")
        self.assertEqual(prog, os.path.join(self.real, "systemctl"))
        self.assertEqual((runtime, bus), (real, f"unix:path={real}/bus"))
        self.assertEqual(args, "--user enable --now ssh-agent")
        self.assertEqual(wayland, "wayland-0")  # the rest of the environment is the desktop's

    def test_systemd_run_too(self):
        prog, runtime, _, _, args = self.run_tool("systemd-run", "--user", "--scope", "true")
        self.assertEqual(prog, os.path.join(self.real, "systemd-run"))
        self.assertEqual(runtime, f"/run/user/{os.getuid()}")
        self.assertEqual(args, "--user --scope true")

    def test_podman_too(self):
        # (distrobox finds podman on PATH, so it gets this one)
        real = f"/run/user/{os.getuid()}"
        prog, runtime, bus, wayland, args = self.run_tool("podman", "exec", "-it", "dev", "true")
        self.assertEqual(prog, os.path.join(self.real, "podman"))
        self.assertEqual((runtime, bus), (real, f"unix:path={real}/bus"))
        self.assertEqual(args, "exec -it dev true")
        self.assertEqual(wayland, "wayland-0")

    def test_they_see_the_users_config_and_state_not_the_desktops(self):
        # podman reads containers.conf and storage.conf under XDG_CONFIG_HOME, and systemctl
        # --user writes units there: both belong in ~/.config, as in a login shell.
        for tool in TOOLS:
            with self.subTest(tool=tool):
                with open(os.path.join(self.real, tool), "w") as f:
                    f.write(FOLDERS)
                config, state, data = self.run_tool(tool)
                self.assertEqual((config, state), ("unset", "unset"))
                self.assertEqual(data, self.env["XDG_DATA_HOME"])  # nothing else is dropped

    def test_the_shell_keeps_the_desktops(self):
        out = subprocess.run(["sh", "-c", "systemctl >/dev/null; podman >/dev/null; "
                              "echo $XDG_RUNTIME_DIR $XDG_CONFIG_HOME $XDG_STATE_HOME"], env=self.env,
                             capture_output=True, text=True, timeout=10).stdout.strip()
        self.assertEqual(out.split(), [self.env[k] for k in ("XDG_RUNTIME_DIR", "XDG_CONFIG_HOME", "XDG_STATE_HOME")])

    def test_the_session_links_them_before_its_path(self):
        text = open(SESSION).read()
        link = re.search(r'for tool in ([\w -]+); do ln -s "\$here/ft-userbus" "\$runtime/bin/\$tool"; done', text)
        self.assertIsNotNone(link)
        self.assertEqual(link.group(1).split(), list(TOOLS))
        path = text.index("export PATH=$runtime/bin:$PATH")
        self.assertLess(text.index('mkdir -m 0700 "$runtime"'), link.start())
        self.assertLess(link.start(), path)
        self.assertLess(path, text.index("dbus-run-session startplasma-wayland"))
        self.assertTrue(os.access(USERBUS, os.X_OK))


if __name__ == "__main__":
    unittest.main()
