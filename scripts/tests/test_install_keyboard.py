"""install.sh, offline: Frametop's keyboard, or a Full Keyboard of the user's own (GitHub #80).

It doesn't run install.sh. It takes the blocks between install.sh's "keyboard choice" and
"keyboard install" markers and runs them in a stand-in script with stand-ins for on_frame, ask
and build, and a keyboard/run.sh that only logs what it was asked to do."""
import os
import pathlib
import subprocess
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
INSTALL = REPO / "install.sh"


def block(name):
    text = INSTALL.read_text()
    begin = text.index(f"# {name}: begin")
    end = text.index(f"# {name}: end")
    return text[begin:end]


STUBS = r'''
# The Frame, faked: STANDALONE=1 has framekeyboard-autostart.service enabled, OURS=1 has
# frametop-keyboard.service installed. Every command is logged.
on_frame() {
  echo "on_frame $1" >> "$LOG"
  case $1 in
    *is-enabled*framekeyboard-autostart.service*) [ "$STANDALONE" = 1 ] ;;
    *frametop-keyboard.service*) [ "$OURS" = 1 ] ;;
    *) return 0 ;;
  esac
}
ask() { echo "ask $1" >> "$LOG"; [ "$2" = y ] && [ "$assume_yes" = 1 ] && return 0; [[ $ANSWER =~ ^[Yy] ]]; }
build() { echo "build $*" >> "$LOG"; }
'''


class KeyboardChoice(unittest.TestCase):
    def run_install(self, keyboard=1, standalone=0, ours=0, answer="y", assume_yes=0, release=0):
        d = pathlib.Path(tempfile.mkdtemp(prefix="ft-install-keyboard-"))
        (d / "keyboard").mkdir()
        run = d / "keyboard" / "run.sh"
        run.write_text('#!/bin/bash\necho "run.sh $*" >> "$LOG"\n')
        run.chmod(0o755)
        log = d / "log"
        log.write_text("")
        script = d / "stand-in.sh"
        script.write_text("set -euo pipefail\n" + STUBS
                          + f"root={d}\nkeyboard={keyboard}\nassume_yes={assume_yes}\nFRAME_RELEASE={release}\n"
                          + block("keyboard choice")
                          + 'if [ "$keyboard" = 1 ]; then build "$root/keyboard/build.sh"; fi\n'
                          + block("keyboard install"))
        env = dict(os.environ, LOG=str(log), STANDALONE=str(standalone), OURS=str(ours), ANSWER=answer)
        r = subprocess.run(["bash", str(script)], capture_output=True, text=True, env=env, timeout=30)
        self.assertEqual(r.returncode, 0, r.stderr)
        lines = log.read_text().splitlines()
        return r.stdout, lines

    def asked(self, lines):
        return [l for l in lines if l.startswith("ask ")]

    def run_sh(self, lines):
        return [l for l in lines if l.startswith("run.sh ")]

    def test_default_installs_frametops_keyboard_without_asking(self):
        out, lines = self.run_install()
        self.assertEqual(self.asked(lines), [])
        self.assertEqual(self.run_sh(lines), ["run.sh install"])
        self.assertTrue(any(l.endswith("keyboard/build.sh") for l in lines if l.startswith("build ")))

    def test_a_standalone_keyboard_asks_and_yes_installs_ours(self):
        out, lines = self.run_install(standalone=1, answer="y")
        self.assertEqual(len(self.asked(lines)), 1)
        self.assertIn("framekeyboard-autostart.service", self.asked(lines)[0])
        self.assertEqual(self.run_sh(lines), ["run.sh install"])

    def test_yes_flag_keeps_todays_default(self):
        out, lines = self.run_install(standalone=1, answer="n", assume_yes=1)
        self.assertEqual(self.run_sh(lines), ["run.sh install"])

    def test_no_to_the_question_leaves_the_standalone_one_alone(self):
        out, lines = self.run_install(standalone=1, answer="n")
        self.assertEqual(self.run_sh(lines), [])
        self.assertFalse(any(l.startswith("build ") for l in lines))
        self.assertIn("Frametop's keyboard: skipped", out)
        # Nothing touches the user's own unit.
        touched = [l for l in lines if "framekeyboard-autostart" in l and not l.startswith(("ask ", "on_frame systemctl --user is-enabled"))]
        self.assertEqual(touched, [])

    def test_no_keyboard_flag_skips_without_asking(self):
        out, lines = self.run_install(keyboard=0, standalone=1)
        self.assertEqual(self.asked(lines), [])
        self.assertEqual(self.run_sh(lines), [])
        self.assertFalse(any("framekeyboard-autostart" in l for l in lines))
        self.assertIn("keyboard/build.sh && keyboard/run.sh install", out)

    def test_skipping_removes_frametops_service_from_before(self):
        out, lines = self.run_install(keyboard=0, ours=1)
        self.assertEqual(self.run_sh(lines), ["run.sh uninstall"])
        self.assertIn("removed", out)

    def test_a_release_says_only_run_sh_install(self):
        out, lines = self.run_install(keyboard=0, release=1)
        self.assertIn("Install Frametop's later with: keyboard/run.sh install", out)
        self.assertNotIn("keyboard/build.sh", out)

    def test_help_lists_the_option(self):
        r = subprocess.run(["bash", str(INSTALL), "--help"], capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("--no-keyboard", r.stdout)
        self.assertIn("--bluetooth     install the Bluetooth fixes", r.stdout)
        self.assertTrue(r.stdout.rstrip().endswith("without a password prompt)"))


if __name__ == "__main__":
    unittest.main()
