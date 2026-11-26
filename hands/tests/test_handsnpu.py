#!/usr/bin/env python3
"""Tests for hands/handsnpu.py, the NPU or CPU choice for hand tracking: how HANDS_NPU reads (as
ft-hands' npu_dir_setting does), whether the NPU service is there (its heads file, on the host or
under the container's /run/host), what the state says, and what the switch writes and applies with
a made-up restart. Every call names its files, in a temporary folder: the real settings and the
real service stay out of reach.

  python3 hands/tests/test_handsnpu.py
"""
import contextlib
import io
import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import handsnpu as hn  # noqa: E402


class HandsNpu(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="ft-handsnpu-")
        self.conf = os.path.join(self.tmp, "frametop.conf")
        self.root = os.path.join(self.tmp, "host")   # a made-up /run/host
        self.roots = (os.path.join(self.tmp, "none"), self.root)

    def write(self, text):
        with open(self.conf, "w") as f:
            f.write(text)

    def read(self):
        with open(self.conf) as f:
            return f.read()

    def serve(self, where=hn.DEFAULT_DIR):
        os.makedirs(self.root + where)
        open(self.root + where + "/" + hn.HEADS, "w").close()

    def state(self):
        return hn.state(None, self.conf, self.roots)

    def run_main(self, *argv, restart=None):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = hn.main(list(argv), self.conf, self.roots, restart)
        return code, out.getvalue(), err.getvalue()

    def test_the_setting_reads_as_ft_hands_reads_it(self):
        for value in ("", "auto", "1"):
            self.assertEqual(hn.folder(value), "/run/ft-npud")
        self.assertEqual(hn.folder("/run/other"), "/run/other")
        # 0, and a word that is no folder: the CPU only ("off" is never a folder named off)
        for value in ("0", "off", "cpu", "auto ", "run/other"):
            self.assertEqual(hn.folder(value), "")
        # (npuserver_test.cpp checks the tracker's npu_dir_setting with these same values)
        with open(os.path.join(HERE, "..", "track", "npuserver.h")) as f:
            self.assertIn(f'kNpuDefaultDir[] = "{hn.DEFAULT_DIR}"', f.read())

    def test_not_set_is_the_npu_and_says_when_its_service_is_missing(self):
        self.write("# mine\n")
        st = self.state()
        self.assertTrue(st["npu"])
        self.assertFalse(st["available"])
        self.assertIn("uses the CPU", st["note"])
        self.serve()
        self.assertEqual(self.state(), {"npu": True, "available": True, "note": ""})

    def test_not_installed_says_how_to_install_it(self):
        self.write("")
        self.assertIn("isn't installed", self.state()["note"])
        self.assertIn("npu/install.sh", self.state()["note"])
        os.makedirs(os.path.dirname(self.root + hn.UNIT))
        open(self.root + hn.UNIT, "w").close()   # installed, not running yet (the DSP at boot)
        self.assertTrue(hn.service_installed(self.roots))
        self.assertIn("isn't running", self.state()["note"])

    def test_the_service_is_found_from_the_container(self):
        self.serve()
        self.assertTrue(hn.service_there(hn.DEFAULT_DIR, self.roots))
        self.assertFalse(hn.service_there(hn.DEFAULT_DIR, self.roots[:1]))
        self.assertFalse(hn.service_there("relative", self.roots))

    def test_a_folder_of_its_own(self):
        self.write("HANDS_NPU=/run/other\n")
        self.serve()   # the usual one, not where it looks
        st = self.state()
        self.assertTrue(st["npu"])
        self.assertFalse(st["available"])
        self.assertIn("/run/other", st["note"])
        self.serve("/run/other")
        self.assertEqual(self.state()["note"], "")

    def test_cpu_has_no_note(self):
        self.write("HANDS_NPU=0\n")
        self.assertEqual(self.state(), {"npu": False, "available": False, "note": ""})
        self.serve()
        self.assertEqual(self.state(), {"npu": False, "available": True, "note": ""})

    def test_switch_writes_only_a_change(self):
        self.write("# mine\n")
        self.assertIn("next start", hn.switch(True, None, self.conf))
        self.assertEqual(self.read(), "# mine\n")   # the NPU already: nothing written
        hn.switch(False, None, self.conf)
        self.assertIn("HANDS_NPU=0", self.read())
        hn.switch(True, None, self.conf)
        self.assertIn("HANDS_NPU=auto", self.read())
        self.write("HANDS_NPU=/run/other\n")
        hn.switch(True, None, self.conf)
        self.assertEqual(self.read(), "HANDS_NPU=/run/other\n")   # its own folder kept
        self.write("HANDS_NPU=off\n")   # a word that isn't a folder is the CPU: the NPU row writes over it
        hn.switch(True, None, self.conf)
        self.assertIn("HANDS_NPU=auto", self.read())

    def test_now_restarts_ft_hands_and_says_what_went_wrong(self):
        self.write("")
        calls = []
        self.assertEqual(hn.switch(False, lambda: calls.append(1) or "", self.conf), "Hand tracking uses the CPU.")
        self.assertEqual(calls, [1])
        with self.assertRaises(hn.NpuError) as e:
            hn.switch(True, lambda: "no bus", self.conf)
        self.assertIn("no bus", str(e.exception))
        self.assertIn("HANDS_NPU=auto", self.read())   # written all the same, as it says

    def test_a_conf_that_cant_be_written(self):
        self.write("")   # a file where its folder would be
        with self.assertRaises(hn.NpuError):
            hn.switch(False, None, os.path.join(self.conf, "frametop.conf"))

    def test_main(self):
        self.write("")
        code, out, _ = self.run_main("status", "--json")
        self.assertEqual((code, json.loads(out)["npu"]), (0, True))
        self.assertIn("its service not running", self.run_main("status")[1])
        calls = []
        code, out, _ = self.run_main("cpu", "--now", restart=lambda: calls.append(1) or "")
        self.assertEqual((code, calls), (0, [1]))
        self.assertEqual(self.run_main("status")[1].strip(), "hand tracking: the CPU")
        code, _, err = self.run_main("npu", "--now", restart=lambda: "no bus")
        self.assertEqual(code, 1)
        self.assertIn("handsnpu:", err)
        self.assertEqual(self.run_main("sideways")[0], 2)


if __name__ == "__main__":
    unittest.main()
