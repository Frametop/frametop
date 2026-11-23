#!/usr/bin/env python3
"""Tests for hands/handsrate.py, the 60 fps hand tracking switch: the camera mode it writes into
XRService's settings (other keys kept, a file that doesn't read left alone, nothing left next to
it), the two ft-hands settings in frametop.conf, what the state says when they don't match,
applying it with a made-up host, what its Host says when systemctl, pgrep or kill fail (with a
made-up user: nothing here runs them, or waits), and what scripts/update-check.py says of the
cameras' rate. hands/tools/slam-rate.py writes through the same code.

Every call here names its files, in a temporary folder of its own, so the real settings stay
out of reach whatever HOME was when handsrate was first imported. slam-rate.py takes no paths:
it runs as a program with HOME in that folder, and never with --kill.

  python3 hands/tests/test_handsrate.py
"""
import contextlib
import io
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import handsrate as hr  # noqa: E402

SLAM_RATE = os.path.join(os.path.dirname(HERE), "tools", "slam-rate.py")
OTHERS = {"captureSessionSettings": {"trackingCameras": {"exposure": 7}, "eyeCameras": {"fps": 90}}, "log": 1}
FAST = {"streamingMode": 0, "slamFrameFPSMode": 5}  # slam-rate.py's 120
STATUS = ("[{t} s]  og01a1b_4-0036_video9 {a:.1f} fps (dark 0 at 0.0 stale 0 torn 0 remapped 0 idled 0, sync 0.08 ms "
          "copy 0.11 ms)  og01a1b_4-0060_video13 {b:.1f} fps (dark 0 at 0.0 stale 0 torn 0 remapped 0 idled 0, sync "
          "0.08 ms copy 0.11 ms)  cam_color PAUSED 30.0 fps (paced 0 half-size copy 0 stale 0 torn 0 paused 0, sync 0.1 ms)")


class FakeHost:
    """XRService as a test sets it up: the process ids each look finds, the last one from then on."""

    def __init__(self, *looks, hands="", stopping=""):
        self.looks, self.hands, self.stopping, self.calls = [set(look) for look in looks], hands, stopping, []

    def restart_hands(self):
        self.calls.append("restart_hands")
        return self.hands

    def xrservice(self):
        return self.looks.pop(0) if len(self.looks) > 1 else (self.looks[0] if self.looks else set())

    def stop(self, pids):
        self.calls.append(("stop", sorted(pids)))
        return self.stopping

    def wait(self, seconds):
        self.calls.append("wait")


class FakeUser:
    """handsmode.Host for handsrate.Host: what each command answers, and what was run."""

    def __init__(self, returncode=0, stdout="", stderr=""):
        self.answer, self.ran = subprocess.CompletedProcess([], returncode, stdout, stderr), []

    def run(self, cmd, timeout=20):
        self.ran.append(cmd)
        return self.answer

    def systemctl(self, *args):
        return self.run(["systemctl", "--user", *args])


class Files(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.home = tmp.name
        self.xr_dir = os.path.join(self.home, ".config/openvr/config/cv/xrservice")
        self.xr_path = os.path.join(self.xr_dir, "XRServiceSettings.json")
        self.conf_path = os.path.join(self.home, ".config/frametop.conf")

    def write_xr(self, data):
        os.makedirs(self.xr_dir, exist_ok=True)
        with open(self.xr_path, "w") as f:
            f.write(data if isinstance(data, str) else json.dumps(data))

    def xr(self):
        with open(self.xr_path) as f:
            return json.load(f)

    def write_conf(self, text):
        os.makedirs(os.path.dirname(self.conf_path), exist_ok=True)
        with open(self.conf_path, "w") as f:
            f.write(text)

    def conf(self):
        return hr.ft_conf.read(self.conf_path)

    def state(self, conf=None):
        return hr.state(conf, self.xr_path, self.conf_path)

    def switch(self, on, host=None):
        return hr.switch(on, host, self.conf_path, self.xr_path)


class Cameras(Files):
    def test_on_from_nothing_and_off_again(self):
        self.assertEqual(hr.with_cameras({}, hr.HIGH), {"captureSessionSettings": {"trackingCameras": hr.HIGH}})
        self.assertEqual(hr.with_cameras({"captureSessionSettings": {"trackingCameras": dict(hr.HIGH)}}, {}), {})

    def test_other_keys_are_kept_both_ways(self):
        on = hr.with_cameras(json.loads(json.dumps(OTHERS)), hr.HIGH)
        self.assertEqual(on["captureSessionSettings"]["trackingCameras"], dict(hr.HIGH, exposure=7))
        self.assertEqual((on["captureSessionSettings"]["eyeCameras"], on["log"]), ({"fps": 90}, 1))
        self.assertEqual(hr.with_cameras(on, {}), OTHERS)

    def test_write_makes_the_file_and_keeps_the_rest(self):
        hr.write_cameras(hr.HIGH, self.xr_path)
        self.assertEqual(hr.read_cameras(self.xr_path), hr.HIGH)
        self.write_xr(OTHERS)
        hr.write_cameras(hr.HIGH, self.xr_path)
        self.assertEqual(self.xr()["captureSessionSettings"]["eyeCameras"], {"fps": 90})
        hr.write_cameras({}, self.xr_path)
        self.assertEqual(self.xr(), OTHERS)
        self.assertEqual(os.listdir(self.xr_dir), ["XRServiceSettings.json"])   # no lock, no temp file

    def test_the_mode_it_already_has_is_not_written_again(self):
        self.write_xr('{"captureSessionSettings": {"trackingCameras": {"slamFrameFPSMode": 3, "streamingMode": 0}}}')
        with mock.patch.object(hr.ft_store, "write_json") as write:
            hr.write_cameras(hr.HIGH, self.xr_path)
        write.assert_not_called()

    def test_a_file_that_does_not_read_is_left_alone(self):
        for bad in ("{not json", "[1, 2]", '{"captureSessionSettings": 3}',
                    '{"captureSessionSettings": {"trackingCameras": "fast"}}'):
            self.write_xr(bad)
            with self.assertRaises(hr.RateError):
                hr.write_cameras(hr.HIGH, self.xr_path)
            st = self.state({})
            self.assertFalse(st["on"])
            self.assertIn("can't be used", st["note"])
            with open(self.xr_path) as f:
                self.assertEqual(f.read(), bad)
            self.assertEqual(os.listdir(self.xr_dir), ["XRServiceSettings.json"])   # no copy of it either

    def test_an_empty_file_is_no_settings(self):
        self.write_xr("")
        self.assertEqual(hr.read_cameras(self.xr_path), {})

    def test_a_write_that_fails_says_so(self):
        with mock.patch.object(hr.ft_store, "write_json", side_effect=OSError("read-only")):
            with self.assertRaisesRegex(hr.RateError, "couldn't be written"):
                hr.write_cameras(hr.HIGH, self.xr_path)

    def test_slam_rate_writes_through_the_same_code(self):
        def run(*args):
            env = dict(os.environ, HOME=self.home, PYTHONDONTWRITEBYTECODE="1")
            return subprocess.run([sys.executable, SLAM_RATE, *args], capture_output=True, text=True, timeout=30,
                                  env=env)
        self.write_xr(OTHERS)
        self.assertEqual(run("120").returncode, 0)
        self.assertEqual(hr.read_cameras(self.xr_path), FAST)
        self.assertEqual(run("60").returncode, 0)
        self.assertEqual(hr.read_cameras(self.xr_path), hr.HIGH)
        self.assertEqual(run().stdout.strip(), json.dumps(hr.HIGH))
        self.assertEqual(run("default").returncode, 0)
        self.assertEqual(self.xr(), OTHERS)
        self.assertEqual(run().stdout.strip(), '"default"')
        self.assertNotEqual(run("90").returncode, 0)
        self.write_xr("{not json")
        bad = run("60")
        self.assertEqual((bad.returncode, bad.stdout), (1, ""))
        self.assertIn("doesn't read", bad.stderr)


class State(Files):
    def test_off_by_default(self):
        self.assertEqual(self.state({}), {"on": False, "note": ""})
        self.assertEqual(self.state(), {"on": False, "note": ""})

    def test_on_is_the_camera_mode(self):
        hr.write_cameras(hr.HIGH, self.xr_path)
        self.assertEqual(self.state(dict(hr.CONF_ON)), {"on": True, "note": ""})
        st = self.state({"HANDS_MAX_HZ": "60"})
        self.assertTrue(st["on"])
        self.assertIn("HANDS_FULL_RATE", st["note"])
        self.assertIn("turn it off, then on again", st["note"])

    def test_the_tracker_alone_is_off_and_says_what_the_switch_can_do(self):
        st = self.state(dict(hr.CONF_ON))
        self.assertFalse(st["on"])
        self.assertIn("cameras aren't", st["note"])
        self.assertIn("turn it on to finish", st["note"])
        self.assertNotIn("turn it off", st["note"])   # it is off: that can't be done

    def test_another_camera_mode_is_off_and_says_on_replaces_it(self):
        hr.write_cameras(FAST, self.xr_path)
        st = self.state({"HANDS_MAX_HZ": "120", "HANDS_FULL_RATE": "1"})
        self.assertFalse(st["on"])
        self.assertIn("another mode", st["note"])
        self.assertIn("turning it on replaces", st["note"])
        self.switch(True)
        self.assertEqual(self.state(), {"on": True, "note": ""})


class Switch(Files):
    def test_on_and_off_write_all_three(self):
        self.write_conf("# mine\nHANDS_MAX_HZ=120   # tried\nHANDS_MODE=frametop\n")
        said = self.switch(True)
        self.assertIn("controllers don't track", said)
        self.assertIn("next start", said)
        self.assertEqual(self.state(), {"on": True, "note": ""})
        self.assertEqual((self.conf()["HANDS_MAX_HZ"], self.conf()["HANDS_FULL_RATE"]), ("60", "1"))
        with open(self.conf_path) as f:
            self.assertIn("# mine\n", f.read())
        self.switch(False)
        self.assertEqual(self.state(), {"on": False, "note": ""})
        self.assertEqual((self.conf()["HANDS_MAX_HZ"], self.conf()["HANDS_FULL_RATE"]), ("30", "0"))
        self.assertEqual(self.conf()["HANDS_MODE"], "frametop")

    def test_off_with_nothing_set_writes_no_settings(self):
        hr.write_cameras(hr.HIGH, self.xr_path)
        self.switch(False)
        self.assertFalse(os.path.exists(self.conf_path))
        self.assertEqual(self.xr(), {})

    def test_a_bad_camera_file_stops_it_before_the_settings(self):
        self.write_xr("{not json")
        with self.assertRaises(hr.RateError):
            self.switch(True)
        self.assertFalse(os.path.exists(self.conf_path))

    def test_settings_that_cannot_be_written_put_the_cameras_back(self):
        hr.write_cameras(FAST, self.xr_path)
        with mock.patch.object(hr.ft_conf, "set_value", side_effect=OSError("disk full")):
            with self.assertRaisesRegex(hr.RateError, "as it was"):
                self.switch(True, FakeHost({41}))
        self.assertEqual(hr.read_cameras(self.xr_path), FAST)

    def test_now_restarts_the_hands_and_xrservice(self):
        host = FakeHost({41}, {41}, {52})
        said = self.switch(True, host)
        self.assertEqual(host.calls[:2], ["restart_hands", ("stop", [41])])
        self.assertIn("Head tracking restarted", said)

    def test_a_new_xrservice_counts_while_the_old_one_is_still_going(self):
        self.assertEqual(hr.restart_tracking(FakeHost({41, 42}, {41, 42}, {42, 52}))[0], True)

    def test_now_without_xrservice_waits_for_steamvr(self):
        host = FakeHost()
        self.assertIn("when SteamVR next starts", self.switch(False, host))
        self.assertEqual(host.calls, ["restart_hands"])

    def test_xrservice_that_stays_down_says_so(self):
        host = FakeHost({41}, set())
        restarted, said = hr.restart_tracking(host, wait=1.0, step=0.5)
        self.assertFalse(restarted)
        self.assertIn("hasn't restarted", said)
        self.assertEqual(host.calls, [("stop", [41]), "wait", "wait"])

    def test_hands_that_cannot_be_restarted_stop_it_there(self):
        host = FakeHost({41}, hands="Failed to connect to bus")
        with self.assertRaisesRegex(hr.RateError, "settings are written, but ft-hands couldn't be restarted"):
            self.switch(True, host)
        self.assertEqual(host.calls, ["restart_hands"])   # XRService is left running
        self.assertTrue(self.state()["on"])

    def test_xrservice_that_cannot_be_stopped_says_so(self):
        host = FakeHost({41}, stopping="Operation not permitted")
        with self.assertRaisesRegex(hr.RateError, r"couldn't be stopped \(Operation not permitted\)"):
            self.switch(True, host)
        self.assertNotIn("wait", host.calls)


class TheHost(unittest.TestCase):
    def test_every_xrservice_is_found(self):
        user = FakeUser(stdout="41\n52\n")
        self.assertEqual(hr.Host(user).xrservice(), {41, 52})
        self.assertEqual(user.ran, [["pgrep", "-f", hr.XRSERVICE]])
        self.assertEqual(hr.Host(FakeUser(returncode=1)).xrservice(), set())

    def test_xrservice_is_found_by_its_command_line(self):
        for line, found in (("/usr/bin/XRService --documentsRoot /home/steamos/.config/openvr", True),
                            ("XRService", True), ("/opt/steamvr/bin/XRService -x", True),
                            ("python3 hands/tools/slam-rate.py XRService", False),
                            ("/usr/bin/XRServiceHelper", False), ("pgrep -f ^(/[^ ]*/)?XRService( |$)", False)):
            with self.subTest(line):
                self.assertEqual(bool(re.search(hr.XRSERVICE, line)), found)

    def test_stop_asks_all_of_them_and_says_what_went_wrong(self):
        user = FakeUser()
        self.assertEqual(hr.Host(user).stop({52, 41}), "")
        self.assertEqual(user.ran, [["kill", "41", "52"]])
        self.assertEqual(hr.Host(FakeUser(1, stderr="kill: (41): Operation not permitted\n")).stop({41}),
                         "kill: (41): Operation not permitted")
        self.assertEqual(hr.Host(FakeUser(1)).stop({41}), "kill exit 1")

    def test_restart_hands_says_what_went_wrong(self):
        user = FakeUser()
        self.assertEqual(hr.Host(user).restart_hands(), "")
        self.assertEqual(user.ran, [["systemctl", "--user", "try-restart", "--no-block", "frametop-hands.service"]])
        self.assertEqual(hr.Host(FakeUser(1, stderr="Failed to connect to bus\n")).restart_hands(),
                         "Failed to connect to bus")
        self.assertEqual(hr.Host(FakeUser(5, stderr="Unit frametop-hands.service not found.")).restart_hands(), "")


class Rate(unittest.TestCase):
    """What scripts/update-check.py says of the cameras with the switch on."""

    def test_the_rate_is_the_highest_after_the_first_line(self):
        lines = [STATUS.format(t=60, a=12.9, b=12.9), STATUS.format(t=120, a=59.0, b=59.9),
                 STATUS.format(t=180, a=0.0, b=0.0)]
        self.assertEqual(hr.bright_rate("\n".join(lines)), 59.9)
        self.assertIsNone(hr.bright_rate(lines[1]))                 # the first line alone: a part minute
        self.assertIsNone(hr.bright_rate("\n".join([lines[1], lines[2], lines[2]])))   # parked since
        self.assertIsNone(hr.bright_rate("XRService is not running; start SteamVR on the headset first\n"))
        self.assertIsNone(hr.bright_rate(""))

    def test_off_says_nothing(self):
        self.assertIsNone(hr.verdict(False, True, False, 30.0))

    def test_sixty_is_ok(self):
        state, detail = hr.verdict(True, True, False, 59.9)
        self.assertEqual(state, "ok")
        self.assertIn("59.9", detail)

    def test_thirty_means_the_mode_no_longer_holds(self):
        # What a SteamOS update that renumbers the modes looks like: the dark frames are back.
        state, detail = hr.verdict(True, True, False, 30.0)
        self.assertEqual(state, "FAIL")
        self.assertIn("handsrate.py off --now", detail)
        self.assertIn("slam-rate.py", detail)

    def test_a_faster_mode_warns(self):
        self.assertEqual(hr.verdict(True, True, False, 114.4)[0], "warn")

    def test_no_rate_yet_is_a_skip_with_what_to_do(self):
        state, detail = hr.verdict(True, True, False, None)
        self.assertEqual(state, "skip")
        self.assertIn("check again", detail)

    def test_written_after_steamvr_started_says_restart(self):
        state, detail = hr.verdict(True, True, True, 30.0)
        self.assertEqual(state, "warn")
        self.assertIn("restart SteamVR", detail)

    def test_on_without_hand_tracking_says_turn_it_off(self):
        state, detail = hr.verdict(True, False, False, None)
        self.assertEqual(state, "warn")
        self.assertIn("handsrate.py off", detail)


class Main(Files):
    def main(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = hr.main(list(argv), self.conf_path, self.xr_path)
        return code, out.getvalue(), err.getvalue()

    def test_status_and_usage(self):
        self.assertEqual(self.main("status", "--json"), (0, '{"on": false, "note": ""}\n', ""))
        self.assertEqual(self.main()[:2], (0, "60 fps hand tracking: off\n"))
        self.assertEqual(self.main("on", "--soon")[0], 2)
        self.assertEqual(self.main("sideways")[0], 2)

    def test_on_without_now_touches_no_process(self):
        with mock.patch.object(hr, "Host", side_effect=AssertionError("no host without --now")):
            code, out, _ = self.main("on")
        self.assertEqual(code, 0)
        self.assertIn("next start", out)
        self.assertTrue(self.state()["on"])

    def test_what_went_wrong_is_exit_1_on_stderr(self):
        self.write_xr("{not json")
        code, out, err = self.main("off")
        self.assertEqual((code, out), (1, ""))
        self.assertIn("doesn't read", err)
        with mock.patch.object(hr, "Host", return_value=FakeHost({41}, hands="no bus")):
            self.write_xr("{}")
            code, out, err = self.main("on", "--now")
        self.assertEqual((code, out), (1, ""))
        self.assertIn("settings are written", err)


if __name__ == "__main__":
    unittest.main()
