"""ft-kbhost, offline: its exit status for each way the keyboard can end, which the unit's
RestartPreventExitStatus reads (3 and 4 stay closed until SteamVR starts again)."""
import importlib.machinery
import importlib.util
import io
import os
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

PATH = Path(__file__).resolve().parents[1] / "host" / "ft-kbhost"
loader = importlib.machinery.SourceFileLoader("ft_kbhost", str(PATH))
spec = importlib.util.spec_from_loader("ft_kbhost", loader)
kbhost = importlib.util.module_from_spec(spec)
loader.exec_module(kbhost)


class ExitStatus(unittest.TestCase):
    def status(self, code, why=None, version="0.19.1"):
        out = io.StringIO()
        with redirect_stdout(out), mock.patch.object(kbhost.subprocess, "run") as run:
            run.return_value.returncode = 3   # the standalone unit isn't running
            status = kbhost.exit_status(code, why, version)
        return status, out.getvalue(), run.call_args_list

    def test_the_users_quit_stays_closed(self):
        self.assertEqual(self.status(0)[0], kbhost.QUIT)

    def test_a_release_without_protocol_1_stays_closed_and_says_how_to_go_back(self):
        status, said, _ = self.status(0, "protocol", "1.0.0")
        self.assertEqual(status, kbhost.NO_PROTOCOL)
        self.assertIn("1.0.0", said)
        self.assertIn("FULLKB_VERSION=", said)

    def test_closed_from_this_end_is_a_failure_not_a_quit(self):
        for why in ("stuck", "oversize"):
            status, said, _ = self.status(0, why)
            self.assertEqual(status, 1, why)
            self.assertIn(kbhost.WHY[why], said)

    def test_failures_restart(self):
        for code in (1, 12, 13, -9):
            self.assertEqual(self.status(code)[0], 1, code)

    def test_an_older_keyboard_that_wont_hand_over_is_stopped_first(self):
        status, said, calls = self.status(kbhost.REFUSED_QUIT)
        self.assertEqual(status, 1)
        self.assertEqual(calls[-1].args[0], ["pkill", "-TERM", "-U", str(os.getuid()), "-x", "framekeyboard"])
        with redirect_stdout(io.StringIO()), mock.patch.object(kbhost.subprocess, "run") as run:
            run.return_value.returncode = 0   # its own unit runs it
            kbhost.exit_status(kbhost.REFUSED_QUIT, None, "0.19.1")
        self.assertEqual(run.call_args_list[-1].args[0], ["systemctl", "--user", "stop", kbhost.STANDALONE])


if __name__ == "__main__":
    unittest.main()
