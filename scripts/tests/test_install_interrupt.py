"""install.sh, offline: an install that stops part-way says to run the same command again.

It doesn't run install.sh. It takes the block between install.sh's "interrupted-install trap"
markers and runs it in a stand-in script under the same set -euo pipefail, which then fails,
finishes, or is sent a signal (Ctrl-C, a closed terminal, FrameDrop stopping it)."""
import os
import pathlib
import signal
import subprocess
import tempfile
import time
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
INSTALL = REPO / "install.sh"
HINT = "Run the same command again to finish the install."


def trap_block():
    text = INSTALL.read_text()
    begin = text.index("# interrupted-install trap: begin")
    end = text.index("# interrupted-install trap: end")
    return text[begin:end]


class InterruptedInstall(unittest.TestCase):
    def script(self, body):
        d = tempfile.mkdtemp(prefix="ft-install-interrupt-")
        path = pathlib.Path(d) / "stand-in.sh"
        path.write_text("set -euo pipefail\n" + trap_block() + body)
        return path

    @staticmethod
    def kill_group(pgid):
        try:
            os.killpg(pgid, signal.SIGKILL)  # the stand-in step's sleep
        except ProcessLookupError:
            pass

    def run_to_end(self, body):
        return subprocess.run(["bash", str(self.script(body))], capture_output=True, text=True, timeout=30)

    def test_a_failed_step_says_run_again(self):
        r = self.run_to_end("echo step 1\nfalse\necho step 2\n")
        self.assertEqual(r.returncode, 1)
        self.assertNotIn("step 2", r.stdout)
        self.assertIn(HINT, r.stderr)
        self.assertIn("(exit 1)", r.stderr)

    def test_a_finished_install_says_nothing(self):
        r = self.run_to_end("echo all steps\n")
        self.assertEqual(r.returncode, 0)
        self.assertNotIn(HINT, r.stderr)

    def test_a_signal_says_run_again(self):
        for sig, rc in ((signal.SIGTERM, 143), (signal.SIGHUP, 129), (signal.SIGINT, 130)):
            with self.subTest(sig=sig.name):
                # A step that takes a while, as the container and image steps do.
                p = subprocess.Popen(["bash", str(self.script("echo ready\nsleep 10 >/dev/null 2>&1 & wait $!\necho after\n"))],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                     start_new_session=True)
                self.addCleanup(self.kill_group, p.pid)
                self.assertEqual(p.stdout.readline().strip(), "ready")
                time.sleep(0.2)
                p.send_signal(sig)
                out, err = p.communicate(timeout=30)
                self.assertEqual(p.returncode, rc)
                self.assertNotIn("after", out)
                self.assertIn(HINT, err)

    def test_the_end_message_says_it_too(self):
        done = INSTALL.read_text().split('step "Done"', 1)[1]
        self.assertIn("If an install or update is ever interrupted, run the same command again", done)


if __name__ == "__main__":
    unittest.main()
