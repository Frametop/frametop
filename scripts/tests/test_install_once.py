"""install.sh, offline: every question comes before any step, the password is asked once, and only
when a picked part needs it, and sudo forgets it when the install ends (DEE-331).

It runs the real install.sh in a temp copy of the repo where every script install.sh calls is a
stub that logs its name. sudo, systemctl, getcap and passwd are stubs on PATH that log and change
nothing; HOME is a temp folder. The privileged installers' stubs call the real frame_sudo
(scripts/_env.sh), as they do. install.sh runs with a pseudo-terminal as its terminal and stdin,
so its questions and sudo's /dev/tty path are the ones a person in Konsole gets."""
import fcntl
import os
import pathlib
import re
import shutil
import signal
import subprocess
import tempfile
import termios
import time
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
PROMPT = re.compile(r"\[(Y/n|y/N)\] ")
PRIVILEGED = {"hands/run.sh", "hands/rec/install.sh", "gaze/tracker/install.sh", "setup/bluetooth/install.sh"}

STEP_STUB = r"""#!/bin/bash
rel=${0#"$FAKE_ROOT"/}
printf 'STEP %s %s\n' "$rel" "$*" | tee -a "$FAKE_LOG"
case " $FAKE_FAIL " in *" $rel "*) exit 1 ;; esac
[ "$rel" = hands/run.sh ] && [ -n "${FAKE_STEP_SLEEP:-}" ] && sleep "$FAKE_STEP_SLEEP"
# hands/run.sh sets ft-camd's capabilities only when they're missing
if [ -n "$PRIVILEGED" ] && ! { [ "${FAKE_CAPS:-0}" = 1 ] && [ "${rel#hands/}" != "$rel" ]; }; then
  . "$FAKE_ROOT/scripts/_env.sh"
  frame_sudo true
fi
[ "$rel" = gaze/tracker/install.sh ] && touch "$FAKE_STATE/eyegrab"
exit 0
"""
# sudo with its timestamp (a file): -v asks and keeps it, -n works only with it (or NOPASSWD),
# -k forgets it, and a plain sudo COMMAND without it logs SUDO-ASKED: a prompt mid-install.
# FAKE_SUDO: ok (the password is right), deny (it isn't), nopasswd (sudoers needs none).
SUDO = r"""#!/bin/bash
ts=$FAKE_STATE/sudo-ts mode=${FAKE_SUDO:-ok}
printf 'SUDO %s\n' "$*" >>"$FAKE_LOG"
case "$1" in
  -k) rm -f "$ts"; exit 0 ;;
  -A) exit 0 ;;
  -n) [ "$mode" = nopasswd ] || [ -e "$ts" ]; exit ;;
  -v) [ "$mode" = deny ] && exit 1; touch "$ts"; exit 0 ;;
esac
if [ "$mode" != nopasswd ] && [ ! -e "$ts" ]; then
  printf 'SUDO-ASKED %s\n' "$*" >>"$FAKE_LOG"
  [ "$mode" = deny ] && exit 1
  touch "$ts"
fi
exit 0
"""
SYSTEMCTL = r"""#!/bin/bash
case "$*" in *restart*steamvr*) printf 'SYSTEMCTL %s\n' "$*" >>"$FAKE_LOG" ;; esac
case "$*" in *is-enabled*frametop-eyegrab*) [ -e "$FAKE_STATE/eyegrab" ]; exit ;; esac
exit 0
"""
GETCAP = r"""#!/bin/bash
[ "${FAKE_CAPS:-0}" = 1 ] && echo "$1 cap_dac_read_search,cap_sys_ptrace,cap_perfmon=ep"
exit 0
"""
PASSWD = r"""#!/bin/bash
echo "$2 ${FAKE_PASSWD:-P} 2026-01-01 0 99999 7 -1"
"""


class InstallOnce(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="ft-install-once-")
        base = pathlib.Path(self.tmp.name)
        self.root, self.home, self.bin, self.state = (base / n for n in ("frametop", "home", "bin", "state"))
        for d in (self.root / "scripts", self.home / ".config", self.home / ".local/bin", self.bin, self.state):
            d.mkdir(parents=True)
        self.log = base / "log"
        self.log.write_text("")
        shutil.copy(REPO / "install.sh", self.root / "install.sh")
        shutil.copy(REPO / "scripts/_env.sh", self.root / "scripts/_env.sh")
        for rel in set(re.findall(r'\$root/([\w./-]+)', (REPO / "install.sh").read_text())):
            if rel != "scripts/_env.sh" and not rel.startswith("."):   # .frametop-release: a release's marker
                stub = STEP_STUB.replace('[ -n "$PRIVILEGED" ]', "true" if rel in PRIVILEGED else "false")
                self.write(self.root / rel, stub)
        (self.root / "hands/handsmode.py").write_text("print('handsmode stub')\n")
        for name, text in (("sudo", SUDO), ("systemctl", SYSTEMCTL), ("getcap", GETCAP), ("passwd", PASSWD)):
            self.write(self.bin / name, text)
        self.write(self.home / ".local/bin/distrobox", "#!/bin/bash\necho 'distrobox: 1.8.2.5'\n")
        self.conf = self.home / ".config/frametop.conf"
        self.conf.write_text("POINTER=1\n")

    def tearDown(self):
        self.tmp.cleanup()

    @staticmethod
    def write(path, text):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        path.chmod(0o755)

    def release(self):
        (self.root / ".frametop-release").write_text("VERSION=0.0.0-test\nCOMMIT=0\n")

    def run_install(self, answers="", args=(), tty=True, during=None, **env):
        e = {"HOME": str(self.home), "PATH": f"{self.bin}:/usr/bin:/bin", "FRAME_LOCAL": "1",
             "FAKE_ROOT": str(self.root), "FAKE_LOG": str(self.log), "FAKE_STATE": str(self.state),
             "FAKE_FAIL": "", "FRAMETOP_SUDO_REFRESH": "50", "LANG": "C.UTF-8"}
        e.update(env)
        cmd = ["bash", str(self.root / "install.sh"), *args]
        if not tty:
            r = subprocess.run(cmd, env=e, input=answers, capture_output=True, text=True, timeout=60)
            return r.returncode, r.stdout + r.stderr
        master, slave = os.openpty()
        try:
            p = subprocess.Popen(cmd, env=e, stdin=slave, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 start_new_session=True, text=True,
                                 preexec_fn=lambda: fcntl.ioctl(0, termios.TIOCSCTTY, 0))
            os.close(slave)
            slave = None
            # One line per answer; then end-of-file, so a question nobody planned for takes its
            # default rather than waiting (the prompt count catches it).
            os.write(master, "".join(a + "\n" for a in answers).encode() + b"\x04" * 4)
            if during:
                during(p)
            out, _ = p.communicate(timeout=60)
            return p.returncode, out
        finally:
            os.close(master)
            if slave is not None:
                os.close(slave)

    def entries(self):
        return self.log.read_text().splitlines()

    def cached(self):
        (self.state / "sudo-ts").write_text("")

    def asked(self):
        """Every password prompt: sudo_once's, and any a step's sudo would show."""
        return [l for l in self.entries() if l.startswith("SUDO -v -p") or l.startswith("SUDO-ASKED")]

    def sudo_calls(self):
        return [line[5:] for line in self.entries() if line.startswith("SUDO ")]

    def steps(self):
        return [line.split()[1] for line in self.entries() if line.startswith("STEP ")]

    def assert_questions_first(self, out):
        prompts = [m.start() for m in PROMPT.finditer(out)]
        first_step = out.index("STEP ")
        self.assertTrue(prompts, out)
        self.assertLess(max(prompts), first_step, "a question came after a step:\n" + out)

    def test_release_asks_everything_first_then_the_password_once(self):
        self.release()
        # voice, hands, Hand Recorder, gaze, eye tracker, Eye Recorder, Bluetooth, restart
        code, out = self.run_install(["y", "y", "y", "y", "y", "y", "y", "n"])
        self.assertEqual(code, 0, out)
        self.assertEqual(len(PROMPT.findall(out)), 8, out)
        self.assert_questions_first(out)
        for part in ("hand tracking: file capabilities", "our eye tracker: its frame grabber", "the Bluetooth fixes:"):
            self.assertIn(part, out)
        self.assertNotIn("the Hand Recorder: file capabilities", out)   # hand tracking sets them
        calls = self.sudo_calls()
        self.assertEqual(len(self.asked()), 1, self.entries())   # once, and no step asks again
        log = self.entries()
        self.assertLess(log.index(self.asked()[0]), next(i for i, l in enumerate(log) if l.startswith("STEP ")))
        self.assertEqual(calls[-1], "-k", "sudo forgets the password at the end")
        # the installers' own sudo runs (frame_sudo: plain sudo, with the terminal)
        self.assertEqual(calls.count("bash -c true"), 4, calls)  # hands, recorder, tracker, Bluetooth
        for rel in PRIVILEGED | {"gaze/rec/install.sh", "voice/run.sh", "gaze/run.sh"}:
            self.assertIn(rel, self.steps())

    def test_no_part_that_needs_sudo_means_no_password(self):
        self.release()
        # hands yes, but this release's ft-camd has its capabilities already; no tracker, no Bluetooth
        code, out = self.run_install(["y", "y", "y", "y", "n", "n", "n"], FAKE_CAPS="1")
        self.assertEqual(code, 0, out)
        self.assertEqual(len(PROMPT.findall(out)), 7, out)   # no Eye Recorder: no eye tracker
        self.assertNotIn("These need your password", out)
        self.assertEqual(self.sudo_calls(), [])
        self.assertNotIn("gaze/tracker/install.sh", self.steps())
        self.assertIn("Install ours later with: gaze/tracker/install.sh", out)

    def test_a_clone_counts_hand_tracking_and_offers_no_recorders(self):
        code, out = self.run_install(["y", "y", "y", "n", "n", "n"], FAKE_CAPS="1")
        self.assertEqual(code, 0, out)
        self.assertEqual(len(PROMPT.findall(out)), 6, out)   # voice, hands, gaze, tracker, Bluetooth, restart
        self.assertIn("hand tracking: file capabilities", out)   # a clone's build clears them
        self.assertEqual(len(self.asked()), 1, self.entries())

    def test_a_refused_password_skips_those_parts(self):
        self.release()
        code, out = self.run_install(["y", "y", "y", "y", "y", "y", "y", "n", "y"], FAKE_SUDO="deny")
        self.assertEqual(code, 0, out)
        self.assert_questions_first(out)
        steps = self.steps()
        for rel in PRIVILEGED:
            self.assertNotIn(rel, steps)
        self.assertIn("skipped: sudo didn't take the password. Install it later with: hands/run.sh install", out)
        self.assertIn("our eye tracker, which isn't installed", out)   # the Eye Recorder waits for it
        self.assertIn("voice/run.sh", steps)
        self.assertEqual(len(self.asked()), 1, self.entries())   # no step asks again

    def test_a_refused_password_can_stop_the_install(self):
        self.release()
        code, out = self.run_install(["y", "y", "y", "y", "y", "y", "y", "n", "n"], FAKE_SUDO="deny")
        self.assertEqual(code, 1, out)
        self.assertEqual(self.steps(), [])
        self.assertIn("Run the same command again", out)

    def test_no_password_on_the_user(self):
        self.release()
        code, out = self.run_install(["y", "y", "y", "y", "y", "y", "y", "n", "y"], FAKE_PASSWD="NP")
        self.assertEqual(code, 0, out)
        self.assertIn("Your user has no password", out)
        self.assertEqual(self.asked(), [])
        for rel in PRIVILEGED:
            self.assertNotIn(rel, self.steps())

    def test_the_keep_alive_refreshes_and_ends_with_the_install(self):
        code, out = self.run_install(["n", "y", "n", "n", "n"], FRAMETOP_SUDO_REFRESH="1", FAKE_STEP_SLEEP="2.5")
        self.assertEqual(code, 0, out)
        calls = self.sudo_calls()
        self.assertGreaterEqual(calls.count("-n -v"), 1, calls)
        self.assertEqual(calls[-1], "-k")
        time.sleep(2.5)
        self.assertEqual(self.sudo_calls(), calls, "the keep-alive outlived the install")

    def test_a_failed_step_still_ends_the_keep_alive(self):
        code, out = self.run_install(["n", "y", "y", "n", "n", "n"], FAKE_FAIL="gaze/run.sh",
                                     FRAMETOP_SUDO_REFRESH="1")
        self.assertEqual(code, 1, out)
        self.assertIn("Run the same command again", out)
        calls = self.sudo_calls()
        self.assertEqual(calls[-1], "-k", calls)
        time.sleep(2.5)
        self.assertEqual(self.sudo_calls(), calls, "the keep-alive outlived the install")

    def test_yes_never_asks_for_the_password(self):
        self.release()
        code, out = self.run_install(args=["--yes"])   # nothing cached
        self.assertEqual(code, 0, out)
        self.assertEqual(PROMPT.findall(out), [])
        self.assertEqual(self.asked(), [])
        for rel in ("hands/run.sh", "gaze/tracker/install.sh", "hands/rec/install.sh"):
            self.assertNotIn(rel, self.steps())
        self.assertIn("--yes doesn't ask", out)

    def test_yes_keeps_a_password_sudo_has(self):
        self.release()
        self.cached()
        code, out = self.run_install(args=["--yes"])
        self.assertEqual(code, 0, out)
        calls = self.sudo_calls()
        self.assertEqual(self.asked(), [])
        self.assertIn("-n -v", calls)
        self.assertEqual(calls[-1], "-k")
        self.assertIn("hands/run.sh", self.steps())

    def test_askpass_answers_sudo_itself(self):
        """FrameDrop's install: SUDO_ASKPASS, --yes, no keep-alive of ours."""
        self.release()
        code, out = self.run_install(args=["--yes", "--bluetooth"], SUDO_ASKPASS="/bin/false")
        self.assertEqual(code, 0, out)
        calls = self.sudo_calls()
        self.assertEqual(sorted(set(calls)), ["-A bash -c true"], calls)
        self.assertIn("setup/bluetooth/install.sh", self.steps())

    def test_the_old_tracker_setting_is_asked_up_front(self):
        self.conf.write_text("POINTER=1\nGAZE_TRACKER=steam\n")
        # voice, hands, gaze, tracker, use ours, Bluetooth, restart
        code, out = self.run_install(["n", "n", "y", "y", "y", "n", "n"])
        self.assertEqual(code, 0, out)
        self.assertEqual(len(PROMPT.findall(out)), 7, out)
        self.assert_questions_first(out)
        self.assertIn("GAZE_TRACKER=auto", self.conf.read_text())

    def test_restart_is_asked_first_and_done_last(self):
        code, out = self.run_install(["n", "n", "n", "y", "y"])   # ..., Bluetooth, restart
        self.assertEqual(code, 0, out)
        self.assertIn("Restarting SteamVR, as you picked at the start.", out)
        self.assertGreater(out.index("Restarting SteamVR"), out.index("STEP "))
        log = self.entries()
        restart = next(i for i, l in enumerate(log) if l.startswith("SYSTEMCTL"))
        self.assertIn("SUDO -k", log[:restart], "sudo forgets the password before the restart")

    def test_hand_tracking_that_needs_no_sudo_stays_when_sudo_refuses(self):
        self.release()
        # hands, no recorder, gaze, tracker, no Eye Recorder, Bluetooth, no restart; go on without
        code, out = self.run_install(["n", "y", "n", "y", "y", "n", "y", "n", "y"], FAKE_SUDO="deny", FAKE_CAPS="1")
        self.assertEqual(code, 0, out)
        steps = self.steps()
        self.assertIn("hands/run.sh", steps)
        self.assertNotIn("gaze/tracker/install.sh", steps)
        self.assertNotIn("setup/bluetooth/install.sh", steps)

    def test_nopasswd_needs_no_password_on_the_user(self):
        self.release()
        code, out = self.run_install(["y", "y", "y", "y", "y", "y", "y", "n"], FAKE_SUDO="nopasswd", FAKE_PASSWD="NP")
        self.assertEqual(code, 0, out)
        self.assertNotIn("Your user has no password", out)
        self.assertEqual(self.asked(), [])
        for rel in PRIVILEGED:
            self.assertIn(rel, self.steps())

    def test_a_signal_still_ends_the_keep_alive(self):
        def stop_at_hands(p):
            deadline = time.monotonic() + 30
            while "STEP hands/run.sh install" not in self.log.read_text():
                self.assertLess(time.monotonic(), deadline, "hands/run.sh never ran")
                time.sleep(0.05)
            p.send_signal(signal.SIGTERM)
        code, out = self.run_install(["n", "y", "n", "n", "n"], during=stop_at_hands,
                                     FRAMETOP_SUDO_REFRESH="1", FAKE_STEP_SLEEP="2.5")
        self.assertEqual(code, 143, out)
        self.assertIn("Run the same command again", out)
        calls = self.sudo_calls()
        self.assertEqual(calls[-1], "-k", calls)
        time.sleep(2.5)
        self.assertEqual(self.sudo_calls(), calls, "the keep-alive outlived the install")


if __name__ == "__main__":
    unittest.main()
