"""menu/ft_menu.py and menu/install.sh, offline: which entries leave SteamVR's Launch a program
list for the Frametop menu, what the menu lists from an applications folder, where it opens
what's picked, the update command, and what the installer writes and removes. kdialog,
konsole and ft-floatd are stand-ins; nothing reaches the network or the real home."""
import configparser
import os
import shlex
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "menu"))
import ft_menu  # noqa: E402

# Every menu entry template Frametop installs, and the submenu it goes in. A new one goes here
# (and in scripts/tests/test_install_lib.py).
TEMPLATES = {
    "settings/ft-settings.desktop": "settings",
    "display-settings/ft-layout-reset.desktop": "screens",
    "display-settings/ft-screens-toggle.desktop": "screens",
    "tour/ft-tour.desktop": "help",
    "tour/ft-recenter-help.desktop": "help",
    "hands/rec/ft-handrec.desktop": "tools",
    "gaze/rec/ft-eyerec.desktop": "tools",
    "gaze/bench/ft-gazebench.desktop": "tools",
    "gaze/probe/ft-gazeprobe.desktop": "tools",
}
# The names the installers give them, where it isn't the template's.
INSTALLED_AS = {"hands/rec/ft-handrec.desktop": "frametop-handrec.desktop",
                "gaze/rec/ft-eyerec.desktop": "frametop-eyerec.desktop",
                "gaze/bench/ft-gazebench.desktop": "frametop-gazebench.desktop"}
# Entries that stay in Steam's list: the Frametop menu itself and the launcher's Desktop.
STAY = {"menu/ft-menu.desktop", "session/deckard-nested-desktop.desktop"}


def keys(path):
    kf = configparser.ConfigParser(interpolation=None, strict=False, delimiters=("=",))
    kf.optionxform = str
    kf.read(path)
    return dict(kf["Desktop Entry"])


def install_into(folder, rels):
    folder.mkdir(parents=True, exist_ok=True)
    for rel in rels:
        name = INSTALLED_AS.get(rel, pathlib.Path(rel).name)
        (folder / name).write_text((REPO / rel).read_text().replace("@REPO@", str(REPO)))


class Templates(unittest.TestCase):
    def test_every_entry_is_listed(self):
        found = {str(p.relative_to(REPO)) for p in REPO.glob("**/*.desktop")
                 if not {".worktrees", "build", "node_modules"} & set(p.relative_to(REPO).parts)
                 and "test" not in p.relative_to(REPO).parts}
        self.assertEqual(found - STAY - set(TEMPLATES), set(), "a new menu entry: give it a submenu here")
        self.assertEqual(set(TEMPLATES) - found, set())

    def test_they_leave_steams_list_only(self):
        for rel, group in TEMPLATES.items():
            with self.subTest(rel):
                k = keys(REPO / rel)
                self.assertIn("gamescope", k.get("NotShowIn", "").split(";"))
                self.assertNotIn("OnlyShowIn", k, "the Frametop desktop (KDE) and Desktop Mode keep it")
                self.assertNotEqual(k.get("NoDisplay", "").lower(), "true", "frunner and Kickoff keep it")
                self.assertEqual(k.get("X-Frametop-Menu"), group)
                self.assertIn(group, dict(ft_menu.GROUPS))

    def test_the_menu_stays(self):
        k = keys(REPO / "menu/ft-menu.desktop")
        self.assertEqual(k["Name"], "Frametop")
        self.assertEqual(k["Exec"], "@REPO@/menu/ft-menu")
        self.assertEqual(k["TryExec"], k["Exec"], "gone when an older version is installed over it")
        for key in ("NotShowIn", "OnlyShowIn", "NoDisplay", "Hidden", "X-Frametop-Menu"):
            self.assertNotIn(key, k)

    def test_floatd_leaves_the_menu_to_steamvr(self):
        """Picked in Steam's list, the menu opens there and hands what it opens to ft-floatd itself."""
        text = (REPO / "float/ft_floatd.py").read_text()
        start = text.index("STEAMVR_ONLY = {")
        self.assertIn('"ft-menu"', text[start:text.index("}", start)])


class Menu(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.apps = pathlib.Path(self.tmp.name) / "applications"
        install_into(self.apps, ["settings/ft-settings.desktop",
                                 "display-settings/ft-layout-reset.desktop", "tour/ft-tour.desktop",
                                 "hands/rec/ft-handrec.desktop", "menu/ft-menu.desktop"])
        (self.apps / "frametop-profile-work.desktop").write_text(
            "[Desktop Entry]\nType=Application\nName=Frametop: Work\nExec=/x/layout/ft-layout open \"Work\"\n"
            "X-Frametop-Profile=true\n")
        (self.apps / "org.kde.dolphin.desktop").write_text("[Desktop Entry]\nType=Application\nName=Dolphin\nExec=dolphin\n")
        (self.apps / "ft-gone.desktop").write_text(
            "[Desktop Entry]\nType=Application\nName=Gone\nExec=x\nHidden=true\nX-Frametop-Menu=tools\n")
        (self.apps / "ft-odd.desktop").write_text(
            "[Desktop Entry]\nType=Application\nName=Odd\nExec=x\nX-Frametop-Menu=nowhere\n")
        (self.apps / "broken.desktop").write_text("not a key file\n[")
        env = mock.patch.dict(os.environ, {"FT_MENU_APPS_DIR": str(self.apps)})
        env.start()
        self.addCleanup(env.stop)

    def tearDown(self):
        self.tmp.cleanup()

    def names(self):
        return {g: [n for n, _ in items] for g, items in ft_menu.entries().items()}

    def test_lists_whats_installed(self):
        self.assertEqual(self.names(), {"settings": ["Settings"], "screens": ["Reset Screen Layout"],
                                        "help": ["Tour"], "tools": ["Hand Recorder"], "profiles": ["Work"]})

    def test_top_menu_order(self):
        self.assertEqual(ft_menu.top_items(ft_menu.entries()),
                         [("open:settings", "Settings"), ("group:screens", "Screens ›"),
                          ("group:profiles", "Profiles ›"), ("group:help", "Help ›"),
                          ("group:tools", "Recorders and tools ›"), ("update", "Update Frametop…")])

    def test_an_empty_folder_still_updates(self):
        with mock.patch.dict(os.environ, {"FT_MENU_APPS_DIR": str(self.apps / "none")}):
            self.assertEqual(ft_menu.top_items(ft_menu.entries()), [("update", "Update Frametop…")])

    def run_menu(self, answers):
        shown, opened, updated = [], [], []
        answers = iter(answers)

        def choose(prompt, items):
            shown.append((prompt, items))
            return next(answers)
        did = ft_menu.menu(choose=choose, launch=opened.append, update=lambda: updated.append(1))
        return did, shown, opened, updated

    def test_submenu_then_app(self):
        did, shown, opened, updated = self.run_menu(["group:screens", "app:0"])
        self.assertEqual(shown[1], ("Screens", [("app:0", "Reset Screen Layout"), ("back", "‹ Back")]))
        self.assertEqual(opened, [str(self.apps / "ft-layout-reset.desktop")])
        self.assertEqual((did, updated), ("opened ft-layout-reset.desktop", []))

    def test_settings_opens_from_the_top(self):
        did, shown, opened, updated = self.run_menu(["open:settings"])
        self.assertEqual(len(shown), 1)
        self.assertEqual(opened, [str(self.apps / "ft-settings.desktop")])
        self.assertEqual(did, "opened ft-settings.desktop")

    def test_more_settings_entries_get_a_submenu(self):
        (self.apps / "ft-more.desktop").write_text(
            "[Desktop Entry]\nType=Application\nName=Frametop More\nExec=x\nX-Frametop-Menu=settings\n")
        self.assertEqual(ft_menu.top_items(ft_menu.entries())[0], ("group:settings", "Settings ›"))

    def test_back_and_update(self):
        did, shown, opened, updated = self.run_menu(["group:help", "back", "update"])
        self.assertEqual([p for p, _ in shown], ["Frametop", "Help", "Frametop"])
        self.assertEqual((did, opened, updated), ("update", [], [1]))

    def test_a_failed_launch_says_so(self):
        errors = []

        def launch(path):
            raise TypeError("constructor returned NULL")
        answers = iter(["group:help", "app:0"])
        did = ft_menu.menu(choose=lambda p, i: next(answers), launch=launch, error=errors.append)
        self.assertEqual(did, "failed ft-tour.desktop")
        self.assertEqual(errors, ["Couldn't open Tour: constructor returned NULL"])

    def test_cancel(self):
        self.assertEqual(self.run_menu([None])[0], "closed")
        self.assertEqual(self.run_menu(["group:tools", None])[0], "closed")

    def test_kdialog(self):
        """choose() asks kdialog: its answer on stdout, Cancel as exit status 1."""
        bin_ = pathlib.Path(self.tmp.name) / "bin"
        bin_.mkdir()
        log = pathlib.Path(self.tmp.name) / "kdialog.log"
        (bin_ / "kdialog").write_text(f'#!/bin/sh\nprintf "%s\\n" "$@" > {log}\n'
                                      '[ -n "$KD_ANSWER" ] || exit 1\necho "$KD_ANSWER"\n')
        (bin_ / "kdialog").chmod(0o755)
        path = f"{bin_}:{os.environ.get('PATH', '/usr/bin:/bin')}"
        with mock.patch.dict(os.environ, {"PATH": path, "KD_ANSWER": "update"}):
            self.assertEqual(ft_menu.choose("Frametop", [("group:help", "Help ›"), ("update", "Update…")]), "update")
        self.assertEqual(log.read_text().splitlines(),
                         ["--title", "Frametop", "--menu", "Frametop", "group:help", "Help ›", "update", "Update…"])
        with mock.patch.dict(os.environ, {"PATH": path, "KD_ANSWER": ""}):
            self.assertIsNone(ft_menu.choose("Frametop", [("update", "Update…")]))
        with mock.patch.dict(os.environ, {"PATH": path, "KD_ANSWER": "app:0"}):
            ft_menu.choose("Profiles", [("app:0", "-work")])
        self.assertEqual(log.read_text().splitlines()[-2:], ["app:0", " -work"], "not an option")

    def test_listing(self):
        text = ft_menu.listing()
        self.assertIn("Screens\n  Reset Screen Layout  (ft-layout-reset.desktop)", text)
        self.assertIn("Update Frametop: curl -fsSL https://frametop.github.io/frametop/get.sh | bash", text)


class Launch(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.apps = pathlib.Path(self.tmp.name)
        install_into(self.apps, ["tour/ft-recenter-help.desktop"])
        self.entry = str(self.apps / "ft-recenter-help.desktop")

    def tearDown(self):
        self.tmp.cleanup()

    def go(self, desktop, answer):
        asked, started = [], []

        def ask(cmd):
            asked.append(cmd)
            return answer
        with mock.patch.dict(os.environ, {"XDG_CURRENT_DESKTOP": desktop}):
            did = ft_menu.launch(self.entry, ask=ask, start=started.append)
        return did, asked, started

    def test_from_steam_floatd_takes_it(self):
        did, asked, started = self.go("gamescope", "ok")
        self.assertEqual([shlex.split(a) for a in asked], [["env", "FT_TOUR_PAGE=recenter", f"{REPO}/tour/ft-tour"]])
        self.assertEqual((did, started), ("floated", []))

    def test_from_steam_floatd_declines(self):
        did, asked, started = self.go("gamescope", "steamvr the setting is off")
        self.assertEqual((did, started), ("started", [self.entry]))

    def test_in_the_desktop(self):
        did, asked, started = self.go("KDE", "ok")
        self.assertEqual((did, asked, started), ("started", [], [self.entry]))

    def test_field_codes_dropped(self):
        p = self.apps / "x.desktop"
        p.write_text("[Desktop Entry]\nType=Application\nName=X\nExec=/a/b --open %U 50%%\n")
        self.assertEqual(shlex.split(ft_menu.commandline(str(p))), ["/a/b", "--open", "50%"])

    def test_a_profiles_quoting_undone(self):
        """layout/ft_layout.py's launcher Exec: quoted for Exec, then escaped for the key file.
        ft-floatd's shlex.split of what's sent is the command with the name as one word."""
        sys.path.insert(0, str(REPO / "layout"))
        import ft_layout
        name = 'My "work" $HOME `x` \\ s'
        folder = pathlib.Path(self.tmp.name) / "launchers"
        with mock.patch.object(ft_layout, "LAUNCHERS", str(folder)), \
                mock.patch.object(ft_layout, "layout_names", lambda layout: [name]):
            ft_layout.write_launchers({})
        entry = folder / ft_layout.launcher_name(name)
        self.assertEqual(shlex.split(ft_menu.commandline(str(entry))),
                         [os.path.join(ft_layout.REPO, "layout", "ft-layout"), "open", name])


class Update(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.home = pathlib.Path(os.path.realpath(self.tmp.name))

    def tearDown(self):
        self.tmp.cleanup()

    def tree(self, rel, release=False):
        t = self.home / rel
        t.mkdir(parents=True)
        if release:
            (t / ".frametop-release").write_text("VERSION=1\nCHANNEL=experimental\n")
        return str(t)

    def cmd(self, tree):
        return ft_menu.update_command(tree, home=str(self.home))

    def tree_git(self, rel):
        t = self.tree(rel)
        (pathlib.Path(t) / ".git").mkdir()
        return t

    def test_clone(self):
        self.assertEqual(self.cmd(self.tree_git("frametop")), "curl -fsSL https://frametop.github.io/frametop/get.sh | bash")

    def test_synced_from_a_pc(self):
        t = self.tree("dev/frametop")
        self.assertIsNone(self.cmd(t))
        with mock.patch.object(ft_menu, "update_command", lambda tree=None, home=None: None):
            self.assertIn("synced from a PC", ft_menu.update_argv(t)[-1])

    def test_clone_elsewhere(self):
        t = self.tree_git("my stuff/frametop")
        self.assertEqual(self.cmd(t), f"curl -fsSL https://frametop.github.io/frametop/get.sh | bash -s -- --dir '{t}'")

    def test_release(self):
        t = self.tree(".local/share/frametop/releases/0.3.0", release=True)
        self.assertEqual(self.cmd(t), "curl -fsSL https://frametop.github.io/frametop/get.sh | bash -s -- --release")

    def test_release_elsewhere(self):
        t = self.tree("rel/0.3.0", release=True)
        self.assertEqual(self.cmd(t),
                         f"curl -fsSL https://frametop.github.io/frametop/get.sh | bash -s -- --release --dir {self.home}/rel")

    def test_konsole(self):
        argv = ft_menu.update_argv(self.tree_git("frametop"))
        self.assertEqual(argv[:5], ["konsole", "-p", "tabtitle=Update Frametop", "-e", "bash"])
        self.assertTrue(argv[-1].startswith("set -o pipefail; "), argv[-1])
        self.assertIn(f"{ft_menu.update_command(self.home / 'frametop')}; status=$?;", argv[-1])
        self.assertIn("read -r -p", argv[-1])
        r = subprocess.run(["bash", "-n", "-c", argv[-1]], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)


class Installer(unittest.TestCase):
    """menu/install.sh with FRAME_LOCAL=1 into a temp applications folder (FT_TEST_APPS_DIR)."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = pathlib.Path(self.tmp.name)
        self.home, self.apps, self.bin = root / "home", root / "apps", root / "bin"
        for d in (self.home, self.apps, self.bin):
            d.mkdir()
        for name in ("ssh", "sudo", "systemctl"):
            (self.bin / name).write_text(f'#!/bin/sh\necho "{name} is not for tests" >&2\nexit 99\n')
            (self.bin / name).chmod(0o755)

    def tearDown(self):
        self.tmp.cleanup()

    def run_it(self, *args):
        env = {"HOME": str(self.home), "PATH": f"{self.bin}:/usr/bin:/bin", "FRAME_LOCAL": "1",
               "FT_TEST_APPS_DIR": str(self.apps)}
        return subprocess.run([str(REPO / "menu/install.sh"), *args], env=env, capture_output=True, text=True,
                              timeout=60, stdin=subprocess.DEVNULL)

    def test_install_refreshes_old_entries_and_uninstall(self):
        old = "[Desktop Entry]\nType=Application\nName=Frametop Hand Recorder\nExec=/old/ft-handrec\n"
        (self.apps / "frametop-handrec.desktop").write_text(old)
        (self.apps / "org.kde.dolphin.desktop").write_text("[Desktop Entry]\nName=Dolphin\n")
        r = self.run_it("install")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(sorted(p.name for p in self.apps.iterdir()),
                         ["frametop-handrec.desktop", "ft-menu.desktop", "org.kde.dolphin.desktop"])
        self.assertEqual((self.apps / "ft-menu.desktop").read_text(),
                         (REPO / "menu/ft-menu.desktop").read_text().replace("@REPO@", str(REPO)))
        moved = old.replace("[Desktop Entry]\n", "[Desktop Entry]\nNotShowIn=gamescope;\nX-Frametop-Menu=tools\n")
        self.assertEqual((self.apps / "frametop-handrec.desktop").read_text(), moved, "what it runs stays")
        r = self.run_it("install")
        self.assertEqual((self.apps / "frametop-handrec.desktop").read_text(), moved, "once")
        new = (REPO / "gaze/bench/ft-gazebench.desktop").read_text()
        (self.apps / "frametop-gazebench.desktop").write_text(new)
        self.run_it("install")
        self.assertEqual((self.apps / "frametop-gazebench.desktop").read_text(), new, "a new entry stays as it is")
        (self.apps / "frametop-gazebench.desktop").unlink()
        self.assertEqual(list(self.home.iterdir()), [])
        r = self.run_it("uninstall")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(sorted(p.name for p in self.apps.iterdir()),
                         ["frametop-handrec.desktop", "org.kde.dolphin.desktop"])

    def test_usage(self):
        self.assertEqual(self.run_it("bogus").returncode, 2)


class UpdateCheck(unittest.TestCase):
    """scripts/update-check.py's check_launch_list, with Steam's list made up."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.apps = pathlib.Path(self.tmp.name) / ".local/share/applications"
        src = (REPO / "scripts/update-check.py").read_text().split("\nif __name__ ==")[0]
        self.uc = {"__file__": str(REPO / "scripts/update-check.py"), "__name__": "update_check"}
        exec(compile(src, "update-check.py", "exec"), self.uc)
        self.uc["HOME"] = self.tmp.name
        self.reports = []
        self.uc["report"] = lambda state, label, detail="": self.reports.append((state, detail))

    def tearDown(self):
        self.tmp.cleanup()

    def check(self, listed):
        fake = mock.Mock()
        fake.evaluate.return_value = __import__("json").dumps(listed)
        with mock.patch.dict(sys.modules, {"steamui": fake}):
            self.uc["check_launch_list"]()
        return self.reports[-1] if self.reports else None

    def test_not_installed_says_nothing(self):
        self.assertIsNone(self.check(["Frametop"]))

    def test_states(self):
        install_into(self.apps, ["menu/ft-menu.desktop", "tour/ft-tour.desktop"])
        (self.apps / "other.desktop").write_text("[Desktop Entry]\nType=Application\nName=Other\nExec=x\n"
                                                 "NotShowIn=gamescope;\n")
        self.assertEqual(self.check(["Frametop", "Konsole", "Other"]),
                         ("ok", "1 of Frametop's entries are in the Frametop menu, not in the list"))
        self.assertEqual(self.check(["Frametop", "Frametop Tour"])[0], "warn")
        self.assertIn("Frametop Tour again", self.reports[-1][1])
        self.assertIn("isn't in it", self.check(["Konsole"])[1])


class Cli(unittest.TestCase):
    def test_list_and_command(self):
        with tempfile.TemporaryDirectory() as d:
            install_into(pathlib.Path(d), ["tour/ft-tour.desktop"])
            env = {**os.environ, "FT_MENU_APPS_DIR": d}
            r = subprocess.run([sys.executable, "-I", str(REPO / "menu/ft_menu.py"), "list"], env=env,
                               capture_output=True, text=True, timeout=30)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertTrue(r.stdout.startswith("Help\n  Tour  (ft-tour.desktop)\nUpdate Frametop: curl "), r.stdout)
            r = subprocess.run([sys.executable, "-I", str(REPO / "menu/ft_menu.py"), "nope"],
                               capture_output=True, text=True, timeout=30)
            self.assertNotEqual(r.returncode, 0)


if __name__ == "__main__":
    unittest.main()
