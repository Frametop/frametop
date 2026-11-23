#!/usr/bin/env python3
"""Tests for hands/handsrate.py, the 60 fps hand tracking switch: the camera mode it writes into
XRService's settings (other keys kept, a file that doesn't read left alone), the two ft-hands
settings in frametop.conf, what the state says when they don't match, and applying it with a
made-up host (nothing here runs systemctl, pgrep or kill, or waits). hands/tools/slam-rate.py
writes through the same code. HOME is a temporary folder.

  python3 hands/tests/test_handsrate.py
"""
import atexit
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HOME = tempfile.mkdtemp()
atexit.register(shutil.rmtree, HOME, True)
os.environ["HOME"] = HOME
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import handsrate as hr  # noqa: E402

SLAM_RATE = os.path.join(os.path.dirname(HERE), "tools", "slam-rate.py")
OTHERS = {"captureSessionSettings": {"trackingCameras": {"exposure": 7}, "eyeCameras": {"fps": 90}}, "log": 1}


class FakeHost:
    """XRService as the tests set it up: its process ids in the order pgrep gives them."""

    def __init__(self, pids=()):
        self.pids, self.calls = list(pids), []

    def restart_hands(self):
        self.calls.append("restart_hands")

    def xrservice(self):
        return self.pids.pop(0) if len(self.pids) > 1 else (self.pids[0] if self.pids else None)

    def stop(self, pid):
        self.calls.append(("stop", pid))

    def wait(self, seconds):
        self.calls.append("wait")


class Files(unittest.TestCase):
    def setUp(self):
        for path in (hr.XR_PATH, hr.CONF_PATH):
            if os.path.exists(path):
                os.remove(path)

    def write_xr(self, data):
        os.makedirs(os.path.dirname(hr.XR_PATH), exist_ok=True)
        with open(hr.XR_PATH, "w") as f:
            f.write(data if isinstance(data, str) else json.dumps(data))

    def xr(self):
        with open(hr.XR_PATH) as f:
            return json.load(f)

    def write_conf(self, text):
        with open(hr.CONF_PATH, "w") as f:
            f.write(text)

    def conf(self):
        return hr.ft_conf.read(hr.CONF_PATH)


class Cameras(Files):
    def test_paths_are_under_the_temporary_home(self):
        self.assertTrue(hr.XR_PATH.startswith(HOME) and hr.CONF_PATH.startswith(HOME))

    def test_on_from_nothing_and_off_again(self):
        self.assertEqual(hr.with_cameras({}, hr.HIGH), {"captureSessionSettings": {"trackingCameras": hr.HIGH}})
        self.assertEqual(hr.with_cameras({"captureSessionSettings": {"trackingCameras": dict(hr.HIGH)}}, {}), {})

    def test_other_keys_are_kept_both_ways(self):
        on = hr.with_cameras(json.loads(json.dumps(OTHERS)), hr.HIGH)
        self.assertEqual(on["captureSessionSettings"]["trackingCameras"], dict(hr.HIGH, exposure=7))
        self.assertEqual((on["captureSessionSettings"]["eyeCameras"], on["log"]), ({"fps": 90}, 1))
        self.assertEqual(hr.with_cameras(on, {}), OTHERS)

    def test_write_makes_the_file_and_keeps_the_rest(self):
        hr.write_cameras(hr.HIGH)
        self.assertEqual(hr.cameras_of(self.xr()), hr.HIGH)
        self.write_xr(OTHERS)
        hr.write_cameras(hr.HIGH)
        self.assertEqual(self.xr()["captureSessionSettings"]["eyeCameras"], {"fps": 90})
        hr.write_cameras({})
        self.assertEqual(self.xr(), OTHERS)

    def test_a_file_that_does_not_read_is_left_alone(self):
        for bad in ("{not json", "[1, 2]", '{"captureSessionSettings": 3}',
                    '{"captureSessionSettings": {"trackingCameras": "fast"}}'):
            self.write_xr(bad)
            with self.assertRaises(hr.ft_store.StoreError):
                hr.write_cameras(hr.HIGH)
            with open(hr.XR_PATH) as f:
                self.assertEqual(f.read(), bad)
            self.assertFalse(hr.state({})["on"])
            self.assertIn("don't read", hr.state({})["note"])

    def test_slam_rate_writes_through_the_same_code(self):
        def run(*args):
            return subprocess.run([sys.executable, SLAM_RATE, *args], capture_output=True, text=True, timeout=30)
        self.write_xr(OTHERS)
        self.assertEqual(run("120").returncode, 0)
        self.assertEqual(hr.cameras_of(self.xr()), {"streamingMode": 0, "slamFrameFPSMode": 5})
        self.assertEqual(run("60").returncode, 0)
        self.assertEqual(hr.cameras_of(self.xr()), hr.HIGH)
        self.assertEqual(run().stdout.strip(), json.dumps(hr.HIGH))
        self.assertEqual(run("default").returncode, 0)
        self.assertEqual(self.xr(), OTHERS)
        self.assertEqual(run().stdout.strip(), '"default"')
        self.assertNotEqual(run("90").returncode, 0)


class State(Files):
    def test_off_by_default(self):
        self.assertEqual(hr.state({}), {"on": False, "note": ""})

    def test_on_is_the_camera_mode(self):
        hr.write_cameras(hr.HIGH)
        self.assertEqual(hr.state(dict(hr.CONF_ON)), {"on": True, "note": ""})
        st = hr.state({"HANDS_MAX_HZ": "60"})
        self.assertTrue(st["on"])
        self.assertIn("HANDS_FULL_RATE", st["note"])

    def test_the_tracker_alone_is_off_with_a_note(self):
        st = hr.state(dict(hr.CONF_ON))
        self.assertFalse(st["on"])
        self.assertIn("cameras aren't", st["note"])

    def test_another_camera_mode_is_off_with_a_note(self):
        hr.write_cameras({"streamingMode": 0, "slamFrameFPSMode": 5})
        st = hr.state({"HANDS_MAX_HZ": "120", "HANDS_FULL_RATE": "1"})
        self.assertFalse(st["on"])
        self.assertIn("another mode", st["note"])


class Switch(Files):
    def test_on_and_off_write_all_three(self):
        self.write_conf("# mine\nHANDS_MAX_HZ=120   # tried\nHANDS_MODE=frametop\n")
        said = hr.switch(True)
        self.assertIn("controllers don't track", said)
        self.assertIn("next start", said)
        self.assertEqual(hr.state(), {"on": True, "note": ""})
        self.assertEqual((self.conf()["HANDS_MAX_HZ"], self.conf()["HANDS_FULL_RATE"]), ("60", "1"))
        with open(hr.CONF_PATH) as f:
            self.assertIn("# mine\n", f.read())
        hr.switch(False)
        self.assertEqual(hr.state(), {"on": False, "note": ""})
        self.assertEqual((self.conf()["HANDS_MAX_HZ"], self.conf()["HANDS_FULL_RATE"]), ("30", "0"))
        self.assertEqual(self.conf()["HANDS_MODE"], "frametop")

    def test_off_with_nothing_set_writes_no_settings(self):
        hr.write_cameras(hr.HIGH)
        hr.switch(False)
        self.assertFalse(os.path.exists(hr.CONF_PATH))
        self.assertEqual(self.xr(), {})

    def test_a_bad_camera_file_stops_it_before_the_settings(self):
        self.write_xr("{not json")
        with self.assertRaises(hr.ft_store.StoreError):
            hr.switch(True)
        self.assertFalse(os.path.exists(hr.CONF_PATH))

    def test_now_restarts_the_hands_and_xrservice(self):
        host = FakeHost([41, 41, 52])
        said = hr.switch(True, host)
        self.assertEqual(host.calls[:2], ["restart_hands", ("stop", 41)])
        self.assertIn("Head tracking restarted", said)

    def test_now_without_xrservice_waits_for_steamvr(self):
        host = FakeHost()
        self.assertIn("when SteamVR next starts", hr.switch(False, host))
        self.assertEqual(host.calls, ["restart_hands"])

    def test_xrservice_that_stays_down_says_so(self):
        class Gone(FakeHost):
            def stop(self, pid):
                super().stop(pid)
                self.pids = []
        host = Gone([41])
        said = hr.restart_tracking(host, wait=1.0, step=0.5)
        self.assertIn("hasn't restarted", said)
        self.assertEqual(host.calls, [("stop", 41), "wait", "wait"])


class Main(Files):
    def test_status_and_usage(self):
        self.assertEqual(hr.main(["status", "--json"]), 0)
        self.assertEqual(hr.main(["on", "--soon"]), 2)
        self.assertEqual(hr.main(["sideways"]), 2)

    def test_on_without_now_touches_no_process(self):
        self.assertEqual(hr.main(["on"]), 0)
        self.assertTrue(hr.state()["on"])
        self.write_xr("{not json")
        self.assertEqual(hr.main(["off"]), 1)


if __name__ == "__main__":
    unittest.main()
