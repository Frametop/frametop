"""npu/root.sh, offline and without root: its `check` reads the unit as systemd does and refuses any
unit without exactly one python3 -I -S -B -c ExecStartPre in [Service], the preflight refuses a tree a
user owns, install and uninstall refuse to run without root, and the paths it installs to are the
ones uninstall.sh, update-check.py and handsnpu.py look for, and install waits for the line the daemons
log once they serve."""
import os
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
ROOT_SH = REPO / "npu/root.sh"
UNIT = REPO / "npu/ft-npud.service"


class RootSh(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="ft-npud-root-"))
        # a tree as npu/build.sh lays it out, owned by this user
        (self.tmp / "tree/app/npud").mkdir(parents=True)
        (self.tmp / "tree/app/npud/trust.py").write_text("raise SystemExit('trust.py ran')\n")

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def root_sh(self, *args):
        r = subprocess.run(["bash", str(ROOT_SH), *map(str, args)], capture_output=True, text=True, timeout=60)
        return r.returncode, r.stdout + r.stderr

    def unit(self, text):
        f = self.tmp / "ft-npud.service"
        f.write_text(text)
        return f

    def test_a_users_tree_is_refused_before_trust_py_runs(self):
        code, out = self.root_sh("check", UNIT, self.tmp / "tree")
        self.assertEqual(code, 1, out)
        self.assertIn("trust.py not root-only", out)   # the shipped unit's one preflight, which ran
        self.assertNotIn("trust.py ran", out)

    def test_only_one_preflight_in_service_passes_the_reader(self):
        pre = UNIT.read_text().split("ExecStartPre=", 1)[1].split("\n", 1)[0]
        line = "ExecStartPre=" + pre + "\n"
        for name, text in (
                ("none", "[Service]\nExecStart=/bin/true\n"),
                ("two", "[Service]\n" + line + line),
                ("one in [Unit] too", "[Unit]\n" + line + "[Service]\n" + line),
                ("another program", "[Service]\nExecStartPre=/usr/bin/python3 -I -c 'pass'\n"),
                ("a continued line", "[Service]\nExecStartPre=/usr/bin/python3 -I -S -B \\\n -c 'pass' x\n")):
            with self.subTest(name):
                code, out = self.root_sh("check", self.unit(text), self.tmp / "tree")
                self.assertEqual(code, 1, out)
                self.assertIn("has no single python3 -I -S -B -c ExecStartPre", out)
        # commented out, a second one doesn't count
        code, out = self.root_sh("check", self.unit("[Service]\n#" + line + line), self.tmp / "tree")
        self.assertIn("trust.py not root-only", out)

    def test_bad_paths(self):
        code, out = self.root_sh("check", "ft-npud.service", self.tmp / "tree")
        self.assertIn("must be an absolute path", out)
        code, out = self.root_sh("check", UNIT, str(self.tmp / "tree") + "/a b")
        self.assertIn("bad tree path", out)
        self.assertEqual(self.root_sh("nonsense")[0], 2)

    # The image's tests run as root (in the container): there, install and uninstall would get past the check
    @unittest.skipIf(os.geteuid() == 0, "as root, install and uninstall would run")
    def test_install_and_uninstall_need_root(self):
        for args in (("install", self.tmp / "tree", "1000"), ("uninstall",)):
            code, out = self.root_sh(*args)
            self.assertEqual(code, 1, out)
            self.assertIn("run as root", out)

    def test_it_waits_for_the_line_each_daemon_logs_once_it_serves(self):
        pattern = re.search(r"grep -q '([^']+)'", ROOT_SH.read_text())[1]   # in serving()
        self.assertEqual(pattern, "; serving /")
        for daemon in ("ft_npud.py", "v30a_serve.py"):
            with self.subTest(daemon):
                self.assertIn("; serving %s' % (", (REPO / "npu/app/npud" / daemon).read_text())

    def test_its_paths_are_the_ones_the_rest_looks_for(self):
        text = ROOT_SH.read_text()
        tree = re.search(r"^TREE=(\S+)$", text, re.M)[1]
        unit = "/etc/systemd/system/" + re.search(r"^UNIT=(\S+)$", text, re.M)[1]
        mnt = re.search(r"^MNT=(\S+)$", text, re.M)[1]
        uninstall = (REPO / "uninstall.sh").read_text()
        self.assertIn(f"npu_unit={unit}\n", uninstall)
        self.assertIn(f"npu_tree={tree}\n", uninstall)
        self.assertIn(f"rmdir {mnt} ", uninstall)
        self.assertIn(f'NPU_UNIT = "{unit}"', (REPO / "scripts/update-check.py").read_text())
        self.assertIn(f'UNIT = "{unit}"', (REPO / "hands/handsnpu.py").read_text())
        self.assertIn(f"BindReadOnlyPaths={tree}:{mnt}\n", UNIT.read_text())


if __name__ == "__main__":
    unittest.main()
