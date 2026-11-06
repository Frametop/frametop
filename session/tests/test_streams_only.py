#!/usr/bin/env python3
"""session/frametop-session.sh with FT_STREAMS_ONLY=1: ft-screens starts alone, no KWin or
Plasma, and the session lasts while ft-screens does. The marker file ft-layout reads
(frametop-streams-only) is there from before ft-layout start until the session ends, whether
ft-screens exits (SteamVR quit) or the unit stops the session (SIGTERM).

The script runs from a copy in a temporary tree, next to stand-ins for what it starts (ft-screens
through scripts/in-box, ft-layout, hand tracking, ft-pause), with pgrep and pkill stand-ins
ahead on PATH that see only the stand-in ft-screens, and HOME and XDG_RUNTIME_DIR temporary
folders: nothing of a real Frametop is touched.

  pytest -q session/tests/test_streams_only.py
"""
import os
import shutil
import signal
import subprocess
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.realpath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

STUBS = {
    # ft-screens: its socket, then "running" while the up file is there
    "scripts/in-box": """#!/bin/bash
echo "in-box $*" >> "$STATE/log"
python3 -c 'import socket, sys; socket.socket(socket.AF_UNIX).bind(sys.argv[1])' "$XDG_RUNTIME_DIR/ft-screens-0"
: > "$STATE/up"
while [ -e "$STATE/up" ]; do sleep 0.1; done
""",
    "layout/ft-layout": """#!/bin/bash
case $1 in
  screen-args) echo "--screen 1920x1080@1.6 --screen 1920x1080@1.6" ;;
  start) echo "ft-layout start marker=$([ -e "$XDG_RUNTIME_DIR/frametop-streams-only" ] && echo 1 || echo 0)" >> "$STATE/log" ;;
esac
""",
    "hands/handsmode.py": "import sys\nopen(__import__('os').environ['STATE'] + '/log', 'a').write('hands ' + ' '.join(sys.argv[1:]) + '\\n')\n",
    "input/ft-pause": "#!/bin/bash\n",
    # On PATH: they see the stand-in ft-screens only
    "bin/pgrep": """#!/bin/bash
[ "$1 $2" = "-x ft-screens" ] && [ -e "$STATE/up" ]
""",
    "bin/pkill": """#!/bin/bash
[ "$1 $2" = "-x ft-screens" ] && { echo pkill >> "$STATE/log"; rm -f "$STATE/up"; }
exit 0
""",
    # The desktop (--inner) must not start: if it does, it gets no further than this
    "bin/dbus-run-session": "#!/bin/bash\necho plasma >> \"$STATE/log\"\nexit 1\n",
}


class StreamsOnly(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        t = lambda *p: os.path.join(self.tmp, *p)
        for d in ("tree/session", "home/.config", "run", "state"):
            os.makedirs(t(d))
        shutil.copy(os.path.join(REPO, "session", "frametop-session.sh"), t("tree/session"))
        shutil.copytree(os.path.join(REPO, "conf"), t("tree/conf"))
        for path, text in STUBS.items():
            os.makedirs(os.path.dirname(t("tree", path)), exist_ok=True)
            with open(t("tree", path), "w") as f:
                f.write(text)
            os.chmod(t("tree", path), 0o755)
        self.state, self.run = t("state"), t("run")
        self.marker = t("run", "frametop-streams-only")
        self.env = dict(os.environ, HOME=t("home"), XDG_RUNTIME_DIR=self.run, STATE=self.state,
                        PATH=t("tree", "bin") + os.pathsep + os.environ["PATH"], FT_STREAMS_ONLY="1")
        for k in ("FT_BACKEND", "FT_SCREENS", "FT_PROFILE"):
            self.env.pop(k, None)
        self.script = t("tree", "session", "frametop-session.sh")
        self.proc = None

    def tearDown(self):
        if self.proc:
            if self.proc.poll() is None:
                self.proc.kill()
                self.proc.wait()
            self.proc.stdout.close()
        try:
            os.remove(os.path.join(self.state, "up"))  # ends a stand-in ft-screens left over
        except FileNotFoundError:
            pass
        time.sleep(0.3)
        shutil.rmtree(self.tmp, ignore_errors=True)

    def log(self):
        try:
            with open(os.path.join(self.state, "log")) as f:
                return f.read()
        except FileNotFoundError:
            return ""

    def start(self, **env):
        self.proc = subprocess.Popen(["bash", self.script], env=dict(self.env, **env), stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True)

    def wait_for(self, what, timeout=15):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if what():
                return True
            if self.proc.poll() is not None:
                break
            time.sleep(0.1)
        return what()

    def up(self):
        """Started, streams only: ft-screens and ft-layout ran, the marker is there."""
        self.assertTrue(self.wait_for(lambda: "in-box" in self.log() and "hands desktop up" in self.log()),
                        self.log() + (self.proc.stdout.read() if self.proc.poll() is not None else ""))
        self.assertTrue(self.wait_for(lambda: "ft-layout start" in self.log()), self.log())
        self.assertIn("ft-layout start marker=1", self.log())
        self.assertTrue(os.path.exists(self.marker))
        self.assertIn("--spares 0", self.log(), "no KWin, so no floating windows")
        time.sleep(0.5)
        self.assertIsNone(self.proc.poll(), "the session ended while ft-screens runs")

    def test_lasts_while_ft_screens_does(self):
        self.start()
        self.up()
        os.remove(os.path.join(self.state, "up"))  # ft-screens exits (SteamVR quit)
        self.assertEqual(self.proc.wait(timeout=10), 0, self.proc.stdout.read())
        self.assertFalse(os.path.exists(self.marker))
        self.assertNotIn("plasma", self.log())

    def test_unit_stop_ends_ft_screens(self):
        self.start()
        self.up()
        self.proc.send_signal(signal.SIGTERM)  # systemctl --user stop frametop-desktop
        self.proc.wait(timeout=10)
        self.assertIn("pkill", self.log())
        self.assertFalse(os.path.exists(os.path.join(self.state, "up")))
        self.assertFalse(os.path.exists(self.marker))
        self.assertNotIn("plasma", self.log())

    def test_needs_the_screens_backend(self):
        self.start(FT_BACKEND="gamescope")
        self.assertEqual(self.proc.wait(timeout=10), 1)
        self.assertIn("needs the screens backend", self.proc.stdout.read())
        self.assertNotIn("in-box", self.log())
        self.assertFalse(os.path.exists(self.marker))


if __name__ == "__main__":
    unittest.main()
