"""The Frametop Installer's host helper, offline: its socket answers askpass only for a process in
the install's cgroup (here, the test's own, given with --cgroup), holds one that comes before the
password, refuses on request, and leaves nothing behind when the window goes. setpw answers a
stand-in passwd's prompts on a pseudo-terminal."""
import os
import pathlib
import subprocess
import sys
import tempfile
import time
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
HOST = REPO / "installer" / "host"
HELPER, ASKPASS = HOST / "helper.py", HOST / "askpass"


def own_cgroup():
    for line in pathlib.Path("/proc/self/cgroup").read_text().splitlines():
        if line.startswith("0::"):
            return line[3:].strip()
    return None


CGROUP = own_cgroup()


@unittest.skipUnless(CGROUP, "no cgroup v2 here")
class Serve(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="fthp-", dir="/tmp")   # short: a socket path has a limit
        self.sock = os.path.join(self.tmp, "s", "askpass")
        self.procs = []

    def tearDown(self):
        for p in self.procs:
            if p.poll() is None:
                p.kill()
                p.wait()
            for f in (p.stdin, p.stdout, p.stderr):
                if f and not f.closed:
                    f.close()
        subprocess.run(["rm", "-rf", self.tmp])

    def serve(self, cgroup=CGROUP):
        p = subprocess.Popen([sys.executable, str(HELPER), "serve", "--socket", self.sock, "--cgroup", cgroup],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.procs.append(p)
        self.assertEqual(p.stdout.readline(), b"ready\n")
        return p

    def askpass(self):
        return subprocess.Popen([sys.executable, str(ASKPASS)], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                env=dict(os.environ, FRAMETOP_ASKPASS_SOCKET=self.sock))

    def test_it_answers_the_install(self):
        p = self.serve()
        p.stdin.write(b"pw " + "pässword".encode().hex().encode() + b"\n")
        p.stdin.flush()
        time.sleep(0.2)
        a = self.askpass()
        out, _ = a.communicate(timeout=10)
        self.assertEqual((a.returncode, out), (0, "pässword\n".encode()))
        self.assertEqual(oct(os.stat(os.path.dirname(self.sock)).st_mode & 0o777), "0o700")

    def test_it_refuses_anyone_else(self):
        p = self.serve(cgroup="/user.slice/user-1.slice/user@1.service/app.slice/frametop-installer.service")
        p.stdin.write(b"pw 41\n")
        p.stdin.flush()
        a = self.askpass()
        out, err = a.communicate(timeout=10)
        self.assertEqual((a.returncode, out), (1, b""))

    def test_one_that_comes_first_waits_for_the_password(self):
        p = self.serve()
        a = self.askpass()
        self.assertEqual(p.stdout.readline(), b"ask\n")
        p.stdin.write(b"pw " + b"later".hex().encode() + b"\n")
        p.stdin.flush()
        out, _ = a.communicate(timeout=10)
        self.assertEqual(out, b"later\n")

    def test_refuse_skips_that_part(self):
        p = self.serve()
        a = self.askpass()
        self.assertEqual(p.stdout.readline(), b"ask\n")
        p.stdin.write(b"refuse\n")
        p.stdin.flush()
        out, _ = a.communicate(timeout=10)
        self.assertEqual((a.returncode, out), (1, b""))

    def test_the_window_going_leaves_nothing(self):
        p = self.serve()
        p.stdin.close()
        self.assertEqual(p.wait(timeout=10), 0)
        self.assertFalse(os.path.exists(self.sock))
        a = self.askpass()
        _, err = a.communicate(timeout=10)
        self.assertEqual(a.returncode, 1)
        self.assertIn(b"isn't answering", err)

    def test_a_folder_that_isnt_ours_is_refused(self):
        os.symlink("/tmp", os.path.join(self.tmp, "s"))
        p = subprocess.Popen([sys.executable, str(HELPER), "serve", "--socket", self.sock, "--cgroup", CGROUP],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.procs.append(p)
        _, err = p.communicate(timeout=10)
        self.assertEqual(p.returncode, 1)
        self.assertIn(b"isn't a folder of yours", err)


# A stand-in passwd: its prompts, then the two answers compared; what it took goes to TOOK.
PASSWD = r"""#!/usr/bin/env python3
import os, sys
mode = os.environ.get("MODE", "plain")
print("Changing password for steamos.", flush=True)
if mode == "current":
    sys.stdout.write("Current password: "); sys.stdout.flush(); sys.stdin.readline()
tries = 0
while True:
    sys.stdout.write("New password: "); sys.stdout.flush(); a = sys.stdin.readline().rstrip("\n")
    sys.stdout.write("Retype new password: "); sys.stdout.flush(); b = sys.stdin.readline().rstrip("\n")
    if mode == "weak":
        print("BAD PASSWORD: The password is shorter than 8 characters", flush=True)
        tries += 1
        if tries >= 3:
            print("passwd: Have exhausted maximum number of retries for service", flush=True); sys.exit(1)
        continue
    if a != b:
        print("Sorry, passwords do not match.", flush=True); sys.exit(1)
    open(os.environ["TOOK"], "w").write(a)
    print("passwd: password updated successfully", flush=True); sys.exit(0)
"""


class SetPassword(unittest.TestCase):
    def setpw(self, pw, mode="plain"):
        with tempfile.TemporaryDirectory() as d:
            bin_ = pathlib.Path(d, "bin")
            bin_.mkdir()
            (bin_ / "passwd").write_text(PASSWD)
            (bin_ / "passwd").chmod(0o755)
            took = pathlib.Path(d, "took")
            r = subprocess.run([sys.executable, str(HELPER), "setpw"], input=pw.encode().hex().encode() + b"\n",
                               capture_output=True, timeout=60,
                               env=dict(os.environ, PATH=f"{bin_}:{os.environ['PATH']}", MODE=mode, TOOK=str(took)))
            return r.returncode, r.stdout.decode(), took.read_text() if took.exists() else None

    def test_a_user_with_no_password(self):
        self.assertEqual(self.setpw("hunter22"), (0, "", "hunter22"))

    def test_an_empty_current_password(self):
        self.assertEqual(self.setpw("hunter22", mode="current")[::2], (0, "hunter22"))

    def test_passwd_refuses_it(self):
        code, said, took = self.setpw("short", mode="weak")
        self.assertEqual((code, took), (1, None))
        self.assertIn("BAD PASSWORD", said)

    def test_nothing_given(self):
        self.assertEqual(self.setpw("")[0], 2)


if __name__ == "__main__":
    unittest.main()
