"""uninstall.sh, offline: its menu-entry glob takes Frametop's ft-* and frametop-* desktop files and
nothing else, and it knows the paths it once missed. The script is only sourced, under a temp HOME,
so main never runs and nothing outside the temp folder is read or removed."""
import os
import pathlib
import subprocess
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parents[2] / "uninstall.sh"

OURS = ["ft-settings.desktop", "ft-gazebench.desktop", "ft-tour.desktop", "frametop-handrec.desktop",
        "frametop-eyerec.desktop", "frametop-gazebench.desktop", "frametop-profile-work-desk.desktop"]
NOT_OURS = ["deckard-nested-desktop.desktop", "native-deckard-nested-desktop.desktop", "firefox.desktop",
            "soft-ft-thing.desktop", "myframetop-x.desktop", "ft-notes.txt", "frametop-x.desktop.bak",
            "ft.desktop", "frametop.desktop", "org.kde.ft-x.desktop"]


class Uninstall(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.home = pathlib.Path(self.tmp.name) / "home"
        self.apps = self.home / ".local/share/applications"
        self.apps.mkdir(parents=True)

    def tearDown(self):
        self.tmp.cleanup()

    def bash(self, script):
        env = {"HOME": str(self.home), "PATH": os.environ.get("PATH", "/usr/bin:/bin")}
        return subprocess.run(["bash", "-c", f'source "$1" && {script}', "bash", str(SCRIPT)], env=env,
                              capture_output=True, text=True, timeout=30, stdin=subprocess.DEVNULL)

    def test_sourcing_runs_nothing(self):
        r = self.bash("echo sourced")
        self.assertEqual((r.returncode, r.stdout, r.stderr), (0, "sourced\n", ""))

    def test_the_glob_takes_only_ours(self):
        for name in OURS + NOT_OURS:
            (self.apps / name).write_text("[Desktop Entry]\n")
        r = self.bash('desktop_entries "$HOME/.local/share/applications"')
        self.assertEqual(r.returncode, 0, r.stderr)
        got = sorted(pathlib.Path(line).name for line in r.stdout.splitlines())
        self.assertEqual(got, sorted(OURS))

    def test_no_entries_is_no_lines(self):
        r = self.bash('desktop_entries "$HOME/.local/share/applications" | wc -l')
        self.assertEqual(r.stdout.strip(), "0")

    def test_the_added_paths(self):
        """Each path uninstall.sh once missed is in the statement that removes it, not only in a comment."""
        text = SCRIPT.read_text()
        for name, pattern in (
                ("FrameDrop's cache, with the rebuilt caches",
                 r'run rm -rf [^\n]*\\\n[^\n#]*"\$HOME/\.cache/frametop-framedrop"'),
                ("the trial's saved files, with the settings",
                 r'local settings=\([^)#]*"\$HOME/\.local/state/frametop-trial"\)'),
                ("the gaze bench's runs, with the recordings",
                 r'for f in [^;\n#]*"\$HOME/\.local/share/frametop/gaze/bench"; do\s*\[ -e "\$f" \] && recs\+='),
                ("the ft-ondesktop link Steam games' launch options call",
                 r'ondesktop_link=\$HOME/\.local/bin/ft-ondesktop[^\n]*\n(?:.*\n)*\s*\[ -n "\$ondesktop_link" \] && run rm -f "\$ondesktop_link"')):
            with self.subTest(name):
                self.assertRegex(text, pattern)

    def test_the_60_fps_camera_mode(self):
        """Step 1 puts the tracking cameras' mode back through hands/handsrate.py when the 60 fps
        switch is on, and says how to by hand when a mode is set and that code is gone."""
        xr = self.home / ".config/openvr/config/cv/xrservice/XRServiceSettings.json"
        xr.parent.mkdir(parents=True)
        gone, here = self.home / "no-repo", SCRIPT.parent
        for name, text, repo, want in (
                ("no settings file", None, here, "[0]"),
                ("SteamVR's own mode", '{"captureSessionSettings": {"eyeCameras": {}}}', here, "[0]"),
                ("the switch on", '{"captureSessionSettings": {"trackingCameras": '
                                  '{"streamingMode": 0, "slamFrameFPSMode": 3}}}', here, "1\n[0]"),
                ("the switch on, the code gone", '{"captureSessionSettings": {"trackingCameras": '
                                                 '{"streamingMode": 0, "slamFrameFPSMode": 3}}}', gone, "manual\n[0]"),
                ("nothing set, the code gone", "{}", gone, "[0]")):
            with self.subTest(name):
                if text is not None:
                    xr.write_text(text)
                r = self.bash(f'hands_rate_state "{repo}"; echo "[$?]"')
                self.assertEqual(r.stdout.strip(), want, r.stderr)
        text = SCRIPT.read_text()
        self.assertRegex(text, r'\[ -n "\$hands_rate" \] && \{ run python3 "\$repo/hands/handsrate\.py" off \|\|')
        self.assertRegex(text, r'\[ -n "\$eye_rate" \] \|\| \[ -n "\$dash_debugger" \] \|\| \[ -n "\$hands_rate" \]; then\s+step "Step 1')


if __name__ == "__main__":
    unittest.main()
