"""scripts/install-lib.sh, offline: the helper writes each unit and menu entry the installers name
as the old copies did (the template with @REPO@ filled, under the names listed here, which are
the installers'), into a temp HOME or the FT_TEST_UNIT_DIR / FT_TEST_APPS_DIR folders, and
enable_units is the host command they ran. It doesn't run the installers themselves. systemctl,
update-desktop-database, ssh and sudo are stubs that log and change nothing; the library is
only sourced, with FRAME_LOCAL=1, so nothing reaches the network or the real home."""
import os
import pathlib
import re
import subprocess
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
LIB = REPO / "scripts/install-lib.sh"

# (template, installed name): each one an installer puts there.
UNITS = [("power/frametop-power.service", None), ("pointer/helper/frametop-pointer.service", None),
         ("keyboard/frametop-keyboard.service", None), ("pointer/dashpatch/frametop-dashpatch.service", None),
         ("gaze/frametop-gaze.service", None), ("voice/frametop-voice.service", None),
         ("voice/frametop-voice-whisper.service", None), ("hands/frametop-camd.service", None),
         ("hands/frametop-hands.service", None), ("screens/frametop-handview.service", None),
         ("session/frametop-autostart.service", None), ("input/frametop-input-relay.service", None)]
ENTRIES = [("gaze/bench/ft-gazebench.desktop", "frametop-gazebench.desktop"),
           ("gaze/rec/ft-eyerec.desktop", "frametop-eyerec.desktop"),
           ("hands/rec/ft-handrec.desktop", "frametop-handrec.desktop"),
           ("gaze/probe/ft-gazeprobe.desktop", None), ("display-settings/ft-display-settings.desktop", None),
           ("display-settings/ft-layout-reset.desktop", None), ("display-settings/ft-screens-toggle.desktop", None),
           ("tour/ft-tour.desktop", None), ("tour/ft-recenter-help.desktop", None),
           ("input-settings/ft-input-settings.desktop", None), ("remote-displays/ft-remote-displays.desktop", None),
           ("remote/ft-remote-settings.desktop", None)]
# The installers that use it, and the ones whose copies it replaced.
USERS = ["power/run.sh", "pointer/helper/run.sh", "keyboard/run.sh", "pointer/dashpatch/run.sh", "gaze/run.sh",
         "voice/run.sh", "hands/run.sh", "desktops.sh", "gaze/bench/install.sh", "gaze/rec/install.sh",
         "hands/rec/install.sh", "gaze/probe/install.sh", "display-settings/install.sh", "tour/install.sh",
         "input-settings/install.sh", "remote-displays/install.sh", "remote/install.sh"]

STUB = '#!/bin/sh\nprintf "%s %s\\n" "$(basename "$0")" "$*" >>"$STUB_LOG"\n'
REFUSE = '#!/bin/sh\necho "$(basename "$0") is not for tests" >&2\nexit 99\n'


class InstallLib(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = pathlib.Path(self.tmp.name)
        self.home, self.bin, self.log = root / "home", root / "bin", root / "stub.log"
        self.home.mkdir()
        self.bin.mkdir()
        for name, text in (("systemctl", STUB), ("update-desktop-database", STUB), ("ssh", REFUSE), ("sudo", REFUSE)):
            (self.bin / name).write_text(text)
            (self.bin / name).chmod(0o755)
        self.root = root

    def tearDown(self):
        self.tmp.cleanup()

    def bash(self, script, **env):
        full = {"HOME": str(self.home), "PATH": f"{self.bin}:/usr/bin:/bin", "FRAME_LOCAL": "1",
                "STUB_LOG": str(self.log), **env}
        return subprocess.run(["bash", "-c", f'set -euo pipefail; . "$1/scripts/_env.sh"; . "$1/scripts/install-lib.sh"; {script}',
                               "bash", str(REPO)], env=full, capture_output=True, text=True, timeout=60,
                              stdin=subprocess.DEVNULL)

    def expected(self, template):
        return (REPO / template).read_text().replace("@REPO@", str(REPO))

    def install_all(self, **env):
        calls = [f'install_unit "$1/{t}"' + (f" {n}" if n else "") for t, n in UNITS]
        calls += [f'install_menu_entry "$1/{t}"' + (f" {n}" if n else "") for t, n in ENTRIES]
        r = self.bash("; ".join(calls), **env)
        self.assertEqual(r.returncode, 0, r.stderr)

    def check(self, units, apps):
        for kind, folder, items in (("unit", units, UNITS), ("entry", apps, ENTRIES)):
            for template, name in items:
                with self.subTest(kind=kind, template=template):
                    got = (folder / (name or pathlib.Path(template).name)).read_text()
                    self.assertEqual(got, self.expected(template))
                    self.assertNotIn("@REPO@", got)
        self.assertEqual(sorted(p.name for p in units.iterdir()),
                         sorted(n or pathlib.Path(t).name for t, n in UNITS))
        self.assertEqual(sorted(p.name for p in apps.iterdir()),
                         sorted(n or pathlib.Path(t).name for t, n in ENTRIES))

    def test_into_the_home(self):
        self.install_all()
        self.check(self.home / ".config/systemd/user", self.home / ".local/share/applications")
        self.assertFalse(self.log.exists(), "writing a unit or an entry runs nothing else")

    def test_into_a_prefix(self):
        units, apps = self.root / "prefix/units", self.root / "prefix/apps"
        self.install_all(FT_TEST_UNIT_DIR=str(units), FT_TEST_APPS_DIR=str(apps))
        self.check(units, apps)
        self.assertEqual(list(self.home.iterdir()), [])

    def test_enable_units(self):
        r = self.bash("enable_units frametop-power.service; echo; enable_units --quiet a.service; echo;"
                      ' enable_units "v.service 2>/dev/null"')
        self.assertEqual(r.stdout.splitlines(), [
            "systemctl --user daemon-reload; systemctl --user enable frametop-power.service",
            "systemctl --user daemon-reload; systemctl --user enable --quiet a.service",
            "systemctl --user daemon-reload; systemctl --user enable v.service 2>/dev/null"])
        r = self.bash('bash -c "$(enable_units frametop-power.service)"')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.log.read_text().splitlines(),
                         ["systemctl --user daemon-reload", "systemctl --user enable frametop-power.service"])

    def test_run_instead_of_sourced(self):
        r = subprocess.run(["bash", str(LIB)], capture_output=True, text=True, timeout=30, stdin=subprocess.DEVNULL)
        self.assertEqual(r.returncode, 2)
        self.assertIn("sourced", r.stderr)

    def test_no_copies_left(self):
        """Every installer uses it, and none writes a unit or a menu entry with its own copy."""
        copy = re.compile(r"cat > [^\n\"]*(systemd/user|share/applications|\$apps|\$entry)")
        for rel in USERS:
            with self.subTest(rel):
                text = (REPO / rel).read_text()
                self.assertIn('. "$root/scripts/install-lib.sh"', text)
                self.assertIsNone(copy.search(text))
                self.assertNotIn("systemctl --user daemon-reload; systemctl --user enable", text)


if __name__ == "__main__":
    unittest.main()
