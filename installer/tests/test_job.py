"""The Frametop Installer's install service, offline: the command it starts (host commands
recorded, nothing run), what it reads back from the log, what the window opens on after a
restart or a crash, and run-install.sh's result and SteamVR restart (systemctl is a stub)."""
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "installer"))
from frametop_installer import host, job  # noqa: E402

HOST = REPO / "installer" / "host"


class Temp(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="ft-installer-")
        self.cache = pathlib.Path(self.tmp.name, "cache")
        self.saved = (host.CACHE, host.BIN, job.STATE, job.LOG, job.RESULT, host.run)
        host.CACHE, host.BIN = self.cache, self.cache / "bin"
        job.STATE, job.LOG, job.RESULT = self.cache / "state.json", self.cache / "install.log", self.cache / "result"
        self.calls = []

        def fake_run(argv, input=None, timeout=60):
            self.calls.append(list(argv))
            return 0, b"", b""
        host.run = fake_run

    def tearDown(self):
        host.CACHE, host.BIN, job.STATE, job.LOG, job.RESULT, host.run = self.saved
        self.tmp.cleanup()


class Start(Temp):
    def systemd_run(self):
        return next(c for c in self.calls if c[0] == "systemd-run")

    def test_the_install_command(self):
        err = job.start("0.3.0-exp.24", "experimental", ["--yes", "--together"], True, True)
        self.assertEqual(err, "")
        cmd = self.systemd_run()
        self.assertIn(f"--unit={job.UNIT}", cmd)
        self.assertIn(f"--setenv=SUDO_ASKPASS={host.BIN / 'askpass'}", cmd)
        tail = cmd[cmd.index("/usr/bin/bash"):]
        self.assertEqual(tail, ["/usr/bin/bash", str(host.BIN / "run-install.sh"), str(host.CACHE), "1", "--",
                                "/usr/bin/bash", str(host.BIN / "get.sh"), "--release", "--version", "0.3.0-exp.24",
                                "--yes", "--together"])
        state = job.load_state()
        self.assertEqual((state["version"], state["flags"], state["restart"], state["sudo"], state["seen"]),
                         ("0.3.0-exp.24", ["--yes", "--together"], True, True, False))
        for name in host.HOST_FILES + ("get.sh",):
            self.assertTrue(os.access(host.BIN / name, os.X_OK), name)
        self.assertEqual((host.BIN / "get.sh").read_bytes(), (REPO / "get.sh").read_bytes())

    def test_no_password_no_askpass_and_a_test_run_never_restarts(self):
        job.start("0.3.0", "stable", ["--yes"], True, False, dry_run=True)
        cmd = self.systemd_run()
        self.assertFalse([c for c in cmd if "SUDO_ASKPASS" in c])
        tail = cmd[cmd.index("/usr/bin/bash"):]
        self.assertEqual(tail[3], "0")
        self.assertEqual(tail[5:], ["/usr/bin/bash", str(host.BIN / "fake-install.sh")])

    def test_a_new_install_forgets_the_last_result_and_log(self):
        self.cache.mkdir(parents=True)
        job.RESULT.write_text("1\n")
        job.LOG.write_text("old\n")
        job.start("0.3.0", "stable", ["--yes"], False, False)
        self.assertIsNone(job.result())
        self.assertEqual(job.LOG.read_text(), "")


class Where(unittest.TestCase):
    def test_what_the_window_opens_on(self):
        s = {"version": "0.3.0", "started": 1000, "seen": False}
        w = job.where_things_are
        self.assertEqual(w(None, "inactive", None), "new")
        self.assertEqual(w(s, "activating", None), "running")
        self.assertEqual(w(s, "inactive", None, now=1002), "running")       # just started
        self.assertEqual(w(s, "inactive", None, now=2000), "unfinished")    # a reboot: no service, no result
        self.assertEqual(w(s, "failed", None, now=2000), "unfinished")      # killed before it wrote one
        self.assertEqual(w(s, "active", 0), "done")
        self.assertEqual(w(s, "failed", 1), "failed")
        self.assertEqual(w(dict(s, seen=True), "active", 0), "new")
        self.assertEqual(w(dict(s, seen=True), "failed", 1), "new")


class Progress(unittest.TestCase):
    def fake_log(self, **env):
        r = subprocess.run(["bash", str(HOST / "fake-install.sh")], capture_output=True,
                           env=dict(os.environ, FAKE_SLEEP="0", **env))
        return r.stdout

    def test_it_follows_the_steps(self):
        p = job.Progress()
        data = self.fake_log()
        seen = []
        for i in range(0, len(data), 97):   # in pieces, as the log grows
            p.feed(data[i:i + 97])
            seen.append(p.fraction)
        p.flush()
        self.assertEqual(seen, sorted(seen), "the bar went back")
        self.assertGreater(p.fraction, 0.95)
        self.assertEqual(p.step, "Finishing")
        self.assertFalse([l for l in p.lines if "Dload" in l or "% Total" in l or l.lstrip().startswith("100 ")])
        self.assertIn("Test run: nothing was installed.", p.lines)

    def test_the_download(self):
        p = job.Progress()
        p.feed(b"Downloading https://x/Frametop.zip (about 1.1 GB)\n")
        self.assertEqual(p.step, "Downloading Frametop (about 1.2 GB)")
        p.feed(b"\r 11  4.65M  11 529.8k   0      0 333.7k      0   00:14   00:01   00:13 456.5k\r")
        self.assertEqual(p.download, "Downloaded 529.8kB of 4.65MB (11%), 00:13 left")
        self.assertAlmostEqual(p.fraction, job.DOWNLOAD_SHARE * 0.11)
        p.feed(b"\x1b[1m== 0/13 loading Frametop 0.3.0 (a minute or two)\x1b[0m\n")
        self.assertEqual((p.fraction, p.download), (job.LOAD_AT, ""))
        self.assertEqual(p.step, "Loading Frametop 0.3.0 (a minute or two)")
        p.feed(b"== 7/13 multi-screen desktop\n")
        self.assertEqual(p.step, "Step 7 of 13: multi-screen desktop")


class RunInstall(unittest.TestCase):
    def run_it(self, restart, rc):
        with tempfile.TemporaryDirectory() as d:
            bin_ = pathlib.Path(d, "bin")
            bin_.mkdir()
            log = pathlib.Path(d, "systemctl.log")
            (bin_ / "systemctl").write_text(f'#!/bin/bash\necho "$*" >> {log}\n')
            (bin_ / "systemctl").chmod(0o755)
            r = subprocess.run(["bash", str(HOST / "run-install.sh"), d, restart, "--", "bash", "-c", f"exit {rc}"],
                               capture_output=True, text=True,
                               env=dict(os.environ, PATH=f"{bin_}:/usr/bin:/bin"), timeout=30)
            return (r.returncode, pathlib.Path(d, "result").read_text().strip(),
                    log.read_text().strip() if log.exists() else "")

    def test_result_then_restart(self):
        self.assertEqual(self.run_it("1", 0), (0, "0", "--user restart steamvr.service"))
        self.assertEqual(self.run_it("0", 0), (0, "0", ""))
        self.assertEqual(self.run_it("1", 3), (3, "3", ""))   # a failed install doesn't restart SteamVR


class Command(unittest.TestCase):
    def test_on_the_host(self):
        saved = host.IN_FLATPAK
        try:
            host.IN_FLATPAK = True
            cmd = host.command(["systemctl", "--user", "show"], watch_bus=True)
            self.assertEqual(cmd[:3], ["flatpak-spawn", "--host", "--watch-bus"])
            self.assertIn("-u", cmd)
            self.assertIn("LD_PRELOAD", cmd)
            self.assertEqual(cmd[-3:], ["systemctl", "--user", "show"])
            host.IN_FLATPAK = False
            self.assertEqual(host.command(["true"])[0], "env")
        finally:
            host.IN_FLATPAK = saved


if __name__ == "__main__":
    unittest.main()
