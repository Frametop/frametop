"""steam/ft-ondesktop, offline: the real script with a made-up /proc (Plasma shells and their
environments) and X11 socket folder, under a temp HOME. The "game" is a Python one-liner that
prints its arguments and environment, so nothing starts on a real display."""
import json
import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "ft-ondesktop"
GAME = [sys.executable, "-c", "import json, os, sys; print(json.dumps([sys.argv[1:], dict(os.environ)]))"]

# What Steam's gamescope session gives a game (the parts this touches, and a few it mustn't).
STEAM_ENV = {
    "DISPLAY": ":0",
    "XDG_RUNTIME_DIR": "/run/user/1000",
    "DBUS_SESSION_BUS_ADDRESS": "unix:path=/run/user/1000/bus",
    "ENABLE_GAMESCOPE_WSI": "1",
    "GAMESCOPE_WAYLAND_DISPLAY": "gamescope-0",
    "XCURSOR_THEME": "steam",
    "XCURSOR_SIZE": "256",
    "SteamAppId": "1234560",
    "LD_PRELOAD": "/steam/gameoverlayrenderer.so",
}


OUR_PAD = """I: Bus=0003 Vendor=045e Product=028e Version=0114
N: Name="frametop virtual gamepad"
H: Handlers=event9 js1

"""
REAL_360 = """I: Bus=0003 Vendor=045e Product=028e Version=0114
N: Name="Microsoft X-Box 360 pad"
H: Handlers=event11 js2

"""
STEAM_PAD = """I: Bus=0003 Vendor=28de Product=11ff Version=0001
N: Name="Microsoft X-Box 360 pad 0"
H: Handlers=event7 js0

"""


class OnDesktop(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = pathlib.Path(self.tmp.name)
        self.home, self.proc, self.x11 = root / "home", root / "proc", root / "x11"
        self.input = root / "input-devices"
        for d in (self.home, self.proc, self.x11):
            d.mkdir()
        self.sockets = []

    def tearDown(self):
        for s in self.sockets:
            s.close()
        self.tmp.cleanup()

    def x_socket(self, n):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.bind(str(self.x11 / f"X{n}"))
        self.sockets.append(s)

    def process(self, pid, comm, env):
        d = self.proc / str(pid)
        d.mkdir()
        (d / "comm").write_text(comm + "\n")
        (d / "environ").write_bytes(b"".join(f"{k}={v}".encode() + b"\0" for k, v in env.items()))

    def frametop_shell(self, pid=200, display=":2", **extra):
        env = {"XDG_CONFIG_HOME": f"{self.home}/.config/frametop", "XDG_RUNTIME_DIR": "/run/user/1000/frametop",
               "DISPLAY": display, "XAUTHORITY": "/run/user/1000/frametop/xauth_abc",
               "XCURSOR_THEME": "breeze_cursors", "XCURSOR_SIZE": "24", "WAYLAND_DISPLAY": "wayland-0"}
        env.update(extra)
        self.process(pid, "plasmashell", env)

    def run_script(self, *args, env=None):
        full = {"HOME": str(self.home), "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
                "FT_ONDESKTOP_PROC": str(self.proc), "FT_ONDESKTOP_X11": str(self.x11),
                "FT_ONDESKTOP_INPUT": str(self.input)}
        full.update(STEAM_ENV if env is None else env)
        return subprocess.run([str(SCRIPT), *args], env=full, capture_output=True, text=True, timeout=30,
                              stdin=subprocess.DEVNULL)

    def game(self, env=None, args=()):
        r = self.run_script(*GAME, *args, env=env)
        self.assertEqual(r.returncode, 0, r.stderr)
        argv, game_env = json.loads(r.stdout)
        return argv, game_env, r.stderr

    def log(self):
        return (self.home / ".local/state/frametop/ondesktop.log").read_text()

    def test_on_the_frametop_desktop(self):
        self.process(100, "plasmashell", {"XDG_CONFIG_HOME": f"{self.home}/.config", "DISPLAY": ":0",
                                          "XDG_RUNTIME_DIR": "/run/user/1000"})  # SteamOS's own desktop
        self.process(150, "kwin_wayland", {"XDG_CONFIG_HOME": f"{self.home}/.config/frametop", "DISPLAY": ":2",
                                           "XDG_RUNTIME_DIR": "/run/user/1000/frametop"})
        self.frametop_shell()
        self.x_socket(0)
        self.x_socket(2)
        _, env, err = self.game()
        self.assertEqual(env["DISPLAY"], ":2")
        self.assertEqual(env["XAUTHORITY"], "/run/user/1000/frametop/xauth_abc")
        self.assertEqual((env["XCURSOR_THEME"], env["XCURSOR_SIZE"]), ("breeze_cursors", "24"))
        self.assertEqual(env["DISABLE_GAMESCOPE_WSI"], "1")
        for gone in ("ENABLE_GAMESCOPE_WSI", "GAMESCOPE_WAYLAND_DISPLAY", "WAYLAND_DISPLAY"):
            self.assertNotIn(gone, env)
        # Steam's runtime folder and bus, not the desktop's private ones; the rest as Steam set it.
        for kept in ("XDG_RUNTIME_DIR", "DBUS_SESSION_BUS_ADDRESS", "SteamAppId", "LD_PRELOAD"):
            self.assertEqual(env[kept], STEAM_ENV[kept])
        self.assertIn("on the Frametop desktop (display :2)", err)
        self.assertIn("app 1234560: on the Frametop desktop (display :2)", self.log())

    def test_arguments_pass_through_unchanged(self):
        self.frametop_shell()
        self.x_socket(2)
        args = ["--verb=waitforexitandrun", "--", "/games/My Game/game.exe", "", "a'b\"c", "$HOME", "*", "-x"]
        argv, _, _ = self.game(args=args)
        self.assertEqual(argv, args)

    def test_the_games_exit_status(self):
        self.frametop_shell()
        self.x_socket(2)
        r = self.run_script(sys.executable, "-c", "raise SystemExit(7)")
        self.assertEqual(r.returncode, 7)
        r = self.run_script(str(self.home / "no-such-game"))
        self.assertEqual(r.returncode, 127)

    def test_no_frametop_desktop_starts_it_as_is(self):
        self.process(100, "plasmashell", {"XDG_CONFIG_HOME": f"{self.home}/.config", "DISPLAY": ":0",
                                          "XDG_RUNTIME_DIR": "/run/user/1000"})
        self.x_socket(0)
        _, env, err = self.game()
        for k, v in STEAM_ENV.items():
            self.assertEqual(env[k], v)
        self.assertNotIn("DISABLE_GAMESCOPE_WSI", env)
        self.assertIn("isn't running, so in the theater", err)
        self.assertIn("in the theater", self.log())

    def test_a_display_without_its_socket_starts_it_as_is(self):
        """The desktop just ended (its Plasma shell not yet gone, its Xwayland is): not onto a dead display."""
        self.frametop_shell()
        _, env, _ = self.game()
        self.assertEqual(env["DISPLAY"], ":0")
        self.assertEqual(env["ENABLE_GAMESCOPE_WSI"], "1")

    def test_a_shell_with_no_x_display_is_skipped(self):
        self.frametop_shell(pid=200, display="wayland-0")
        self.frametop_shell(pid=300, display=":3.0")
        self.x_socket(3)
        _, env, _ = self.game()
        self.assertEqual(env["DISPLAY"], ":3.0")

    def test_no_xauthority_unsets_steams(self):
        self.frametop_shell(XAUTHORITY="")
        self.x_socket(2)
        _, env, _ = self.game(env=dict(STEAM_ENV, XAUTHORITY="/run/user/1000/gamescope-xauth"))
        self.assertNotIn("XAUTHORITY", env)

    def test_frametops_pad_leaves_sdls_ignore_list(self):
        self.frametop_shell()
        self.x_socket(2)
        self.input.write_text(STEAM_PAD + OUR_PAD)
        for given, wanted in (("0x28de/0x11ff,0x045e/0x028e,0x054c/0x0ce6", "0x28de/0x11ff,0x054c/0x0ce6"),
                              ("0x045E/0x028E,0x28de/0x11ff", "0x28de/0x11ff"),
                              ("0x45e/0x28e", None), ("0x045e/0x028e", None),
                              ("0x045e/0x02ea", "0x045e/0x02ea"), ("junk,0x045e/0x028e", "junk"),
                              ("*,0x045e/0x028e", "*")):  # not a file name pattern
            with self.subTest(given):
                _, env, _ = self.game(env=dict(STEAM_ENV, SDL_GAMECONTROLLER_IGNORE_DEVICES=given))
                self.assertEqual(env.get("SDL_GAMECONTROLLER_IGNORE_DEVICES"), wanted)
        _, env, _ = self.game()
        self.assertNotIn("SDL_GAMECONTROLLER_IGNORE_DEVICES", env)

    def test_a_real_xbox_360_pad_keeps_the_ignore_list(self):
        """Same ids as Frametop's pad: Steam Input gives the game its own copy of a real one, so the
        game reading it raw too would get every press twice."""
        self.frametop_shell()
        self.x_socket(2)
        given = "0x28de/0x11ff,0x045e/0x028e"
        for devices in (OUR_PAD + REAL_360, REAL_360, STEAM_PAD, ""):
            with self.subTest(devices[:60]):
                self.input.write_text(devices)
                _, env, _ = self.game(env=dict(STEAM_ENV, SDL_GAMECONTROLLER_IGNORE_DEVICES=given))
                self.assertEqual(env["SDL_GAMECONTROLLER_IGNORE_DEVICES"], given)
        self.input.unlink()
        _, env, _ = self.game(env=dict(STEAM_ENV, SDL_GAMECONTROLLER_IGNORE_DEVICES=given))
        self.assertEqual(env["SDL_GAMECONTROLLER_IGNORE_DEVICES"], given)

    def test_an_unreadable_shell_is_skipped_quietly(self):
        """Another user's Plasma shell (or one exiting): its environ can't be read."""
        self.process(150, "plasmashell", {})
        (self.proc / "150" / "environ").unlink()
        (self.proc / "160").mkdir()  # a process gone between the listing and the read: no comm
        self.frametop_shell()
        self.x_socket(2)
        _, env, err = self.game()
        self.assertEqual(env["DISPLAY"], ":2")
        ours = [line for line in err.splitlines() if not line.startswith("ERROR: ld.so:")]  # the fake LD_PRELOAD
        self.assertEqual(ours, ["ft-ondesktop: on the Frametop desktop (display :2): " + GAME[0]])

    def test_the_ignore_list_stays_in_the_theater(self):
        given = "0x045e/0x028e"
        _, env, _ = self.game(env=dict(STEAM_ENV, SDL_GAMECONTROLLER_IGNORE_DEVICES=given))
        self.assertEqual(env["SDL_GAMECONTROLLER_IGNORE_DEVICES"], given)

    def test_check(self):
        r = self.run_script("--check")
        self.assertEqual(r.returncode, 1)
        self.assertIn("isn't running", r.stdout)
        self.frametop_shell()
        self.x_socket(2)
        r = self.run_script("--check")
        self.assertEqual((r.returncode, r.stdout.strip()),
                         (0, "the Frametop desktop is running: games start on its display :2"))

    def test_launch_option(self):
        r = self.run_script("--launch-option")
        self.assertEqual((r.returncode, r.stdout), (0, f"{self.home}/.local/bin/ft-ondesktop %command%\n"))
        self.home = self.home.parent / "my home"
        r = self.run_script("--launch-option")
        self.assertEqual(r.stdout, f'"{self.home}/.local/bin/ft-ondesktop" %command%\n')

    def test_usage(self):
        for args in ((), ("--",), ("-h",)):
            with self.subTest(args):
                r = self.run_script(*args)
                self.assertEqual(r.returncode, 2)
                self.assertIn("usage: ft-ondesktop COMMAND", r.stderr)

    def test_double_dash_then_the_command(self):
        self.frametop_shell()
        self.x_socket(2)
        r = self.run_script("--", *GAME, "x")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(json.loads(r.stdout)[0], ["x"])


if __name__ == "__main__":
    unittest.main()
