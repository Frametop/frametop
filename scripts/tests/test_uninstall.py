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
                ("the Frametop Installer's scripts, log and state, unless it runs this",
                 r'\[ "\$from_installer" = 1 \] \|\| run rm -rf "\$HOME/\.cache/frametop-installer"'),
                ("the stereo wallpaper, with the settings",
                 r'local settings=\([^)#]*"\$HOME/\.local/share/frametop/wallpaper"\)'),
                ("distrobox, while no other container uses it",
                 r'others=\$\(other_boxes "\$kept_releases" "\$kept_dev"(?:.*\n)*\s*run rm -rf "\$\{dbx\[@\]\}"'),
                ("the trial's saved files, with the settings",
                 r'local settings=\([^)#]*"\$HOME/\.local/state/frametop-trial"[^)#]*\)'),
                ("the gaze bench's runs, with the recordings",
                 r'for f in [^;\n#]*"\$HOME/\.local/share/frametop/gaze/bench"; do\s*\[ -e "\$f" \] && recs\+='),
                ("the ft-ondesktop link Steam games' launch options call",
                 r'ondesktop_link=\$HOME/\.local/bin/ft-ondesktop[^\n]*\n(?:.*\n)*\s*\[ -n "\$ondesktop_link" \] && run rm -f "\$ondesktop_link"'),
                ("NPU hand tracking's unit and tree, in the system files' sudo",
                 r'exists [^\n]*"\$npu_unit" "\$npu_tree" && sys=1(?:.*\n)*\s*systemctl disable --now ft-npud\.service'
                 r'(?:.*\n)*\s*rm -rf "\$npu_tree" "\$npu_tree\.prev"(?:.*\n)*[^\n]*true\' sys "\$\{camd_caps:\+\$camd\}" "\$npu_tree" [^\n]*"\$npu_unit"; then')):
            with self.subTest(name):
                self.assertRegex(text, pattern)

    def test_the_60_fps_camera_mode(self):
        """Step 1 puts the tracking cameras' mode back through hands/handsrate.py when the 60 fps
        switch is on, and says how to by hand when a mode is set and that code is gone."""
        xr = self.home / ".config/openvr/config/cv/xrservice/XRServiceSettings.json"
        xr.parent.mkdir(parents=True)
        gone, broken, here = self.home / "no-repo", self.home / "broken", SCRIPT.parent
        (broken / "hands").mkdir(parents=True)
        (broken / "hands/handsrate.py").write_text("import ft_conf_that_is_gone\n")
        for name, text, repo, want in (
                ("no settings file", None, here, "[0]"),
                ("SteamVR's own mode", '{"captureSessionSettings": {"eyeCameras": {}}}', here, "[0]"),
                ("the switch on", '{"captureSessionSettings": {"trackingCameras": '
                                  '{"streamingMode": 0, "slamFrameFPSMode": 3}}}', here, "1\n[0]"),
                ("the switch on, the code gone", '{"captureSessionSettings": {"trackingCameras": '
                                                 '{"streamingMode": 0, "slamFrameFPSMode": 3}}}', gone, "manual\n[0]"),
                ("the switch on, the code there but broken", '{"captureSessionSettings": {"trackingCameras": '
                                                             '{"streamingMode": 0, "slamFrameFPSMode": 3}}}', broken,
                 "manual\n[0]"),
                ("nothing set, the code gone", "{}", gone, "[0]")):
            with self.subTest(name):
                if text is not None:
                    xr.write_text(text)
                r = self.bash(f'hands_rate_state "{repo}"; echo "[$?]"')
                self.assertEqual(r.stdout.strip(), want, r.stderr)
        text = SCRIPT.read_text()
        self.assertRegex(text, r'\[ -n "\$hands_rate" \] && \{ run python3 "\$repo/hands/handsrate\.py" off \|\|')
        self.assertRegex(text, r'\[ -n "\$eye_rate" \] \|\| \[ -n "\$dash_debugger" \] \|\| \[ -n "\$hands_rate" \]; then\s+said step1 yes\s+step "Step 1')


    def test_plan_lines(self):
        """--plan's questions are lines on fd 3, with their default and size, and ask nothing."""
        r = self.bash('plan=1 dry=1 yes=1; { ask voice "Delete?" y 1.2G; ask code "Delete?" n; '
                      'said step1 yes; } 3>&1 >/dev/null; echo "[$?]"')
        self.assertEqual(r.stdout, "ask voice y 1.2G\nask code n -\nstep1 yes\n[0]\n", r.stderr)

    def test_yes_takes_the_default_or_the_choice(self):
        r = self.bash('yes=1; choice[settings]=y choice[releases]=n; '
                      'for k in settings releases voice avatars; do '
                      'if ask "$k" "Q $k" "$([ $k = voice ] && echo y || echo n)" >/dev/null; then echo "$k y"; else echo "$k n"; fi; done')
        self.assertEqual(r.stdout.split("\n")[:4], ["settings y", "releases n", "voice y", "avatars n"], r.stderr)

    def test_a_wrong_key(self):
        r = self.bash('main --yes --delete everything || echo "[$?]"')
        self.assertIn("[2]", r.stdout)
        self.assertIn("--delete takes releases", r.stderr)

    def test_sudo_asks_askpass_when_there_is_one(self):
        """Both sudo calls go through sudo_, which adds -A when SUDO_ASKPASS is set (the window's helper)."""
        text = SCRIPT.read_text()
        outside = "\n".join(line for line in text.splitlines() if not line.startswith("sudo_()"))
        self.assertNotRegex(outside, r'run sudo ')
        self.assertEqual(len(__import__("re").findall(r'sudo_ (?:bash|setcap)', text)), 2)
        r = self.bash('dry=1; sudo_ true; SUDO_ASKPASS=/x sudo_ true')
        self.assertEqual(r.stdout, "  would run: sudo true\n  would run: sudo -A true\n", r.stderr)

    def test_other_boxes(self):
        """distrobox stays while a container besides the ones being deleted uses it."""
        bin_ = self.home / "bin"
        bin_.mkdir()
        (bin_ / "podman").write_text("#!/bin/sh\nprintf 'dev\\nframetop-229b33a32503\\nmine\\n'\n")
        (bin_ / "podman").chmod(0o755)
        for keep_rel, keep_dev, want in (("0", "0", ["mine"]), ("1", "0", ["frametop-229b33a32503", "mine"]),
                                         ("0", "1", ["dev", "mine"])):
            with self.subTest(keep_releases=keep_rel, keep_dev=keep_dev):
                r = self.bash(f'PATH="$HOME/bin:$PATH"; other_boxes {keep_rel} {keep_dev}')
                self.assertEqual(sorted(r.stdout.split()), want, r.stderr)


if __name__ == "__main__":
    unittest.main()
