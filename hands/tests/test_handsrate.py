#!/usr/bin/env python3
"""Tests for hands/handsrate.py, the 60 fps hand tracking switch: the camera mode it writes into
XRService's settings (other keys kept, a file that doesn't read left alone, nothing left next to
it), the two ft-hands settings in frametop.conf, what the state says when they don't match,
applying it with a made-up host, what its Host says when systemctl, pgrep or kill fail (with a
made-up user: nothing here runs them, or waits), and what it reads of the cameras in ft-camd's
status lines for scripts/update-check.py. hands/tools/slam-rate.py writes through the same code.

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
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import handsrate as hr  # noqa: E402

SLAM_RATE = os.path.join(os.path.dirname(HERE), "tools", "slam-rate.py")
OTHERS = {"captureSessionSettings": {"trackingCameras": {"exposure": 7}, "eyeCameras": {"fps": 90}}, "log": 1}
FAST = {"streamingMode": 0, "slamFrameFPSMode": 5}  # slam-rate.py's 120
STATUS = ("[{t} s]  og01a1b_4-0036_video9 {a:.1f} fps (dark {d} at 3.6 stale 0 torn 0 remapped 0 idled 0, sync 0.08 ms "
          "copy 0.11 ms)  og01a1b_4-0060_video13 {b:.1f} fps (dark {d} at 3.7 stale 0 torn 0 remapped 0 idled 0, sync "
          "0.08 ms copy 0.11 ms)  cam_color PAUSED 30.0 fps (paced 0 half-size copy 0 stale 0 torn 0 paused 0, sync 0.1 ms)")


class FakeHost:
    """XRService and tracking units as a test sets them up: the process ids each look finds,
    the last one from then on, plus which tracking units are running and when cam-ring is live."""

    def __init__(self, *looks, hands="", stopping="", running=hr.TRACKING_UNITS, querying="",
                 stop_units="", start_units="", ring=(True,)):
        self.looks = [set(look) for look in looks]
        self.hands, self.stopping = hands, stopping
        self.running, self.querying = tuple(running), querying
        self.stop_units = stop_units
        self.start_units = start_units if isinstance(start_units, dict) else {"*": start_units}
        self.ring = list(ring)
        self.calls = []

    def restart_hands(self):
        self.calls.append("restart_hands")
        return self.hands

    def running_tracking(self):
        self.calls.append("running_tracking")
        return (() if self.querying else self.running), self.querying

    def stop_tracking(self, units):
        self.calls.append(("stop_tracking", list(units)))
        return self.stop_units

    def start_tracking(self, units):
        units = list(units)
        self.calls.append(("start_tracking", units))
        key = tuple(units)
        return self.start_units.get(key, self.start_units.get("*", ""))

    def ring_alive(self):
        self.calls.append("ring_alive")
        return self.ring.pop(0) if len(self.ring) > 1 else (self.ring[0] if self.ring else False)

    def xrservice(self):
        return self.looks.pop(0) if len(self.looks) > 1 else (self.looks[0] if self.looks else set())

    def stop(self, pids):
        self.calls.append(("stop", sorted(pids)))
        return self.stopping

    def wait(self, seconds):
        self.calls.append("wait")


def make_ring_header(magic=hr.RING_MAGIC, version=hr.RING_VERSION, writer_pid=1234, heartbeat_ns=10_000_000_000):
    return struct.pack("<8sI20xqQ", magic, version, writer_pid, heartbeat_ns)


class FakeUser:
    """handsmode.Host for handsrate.Host: what each command answers, and what was run."""

    def __init__(self, returncode=0, stdout="", stderr="", each=None):
        self.answer, self.ran = subprocess.CompletedProcess([], returncode, stdout, stderr), []
        self.each = each or {}   # {a command's last word: (returncode, stderr)}, for kill by pid

    def run(self, cmd, timeout=20):
        self.ran.append(cmd)
        if cmd[-1] in self.each:
            return subprocess.CompletedProcess(cmd, self.each[cmd[-1]][0], "", self.each[cmd[-1]][1])
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
            with self.assertRaisesRegex(hr.RateError, r"frametop.conf couldn't be written \(disk full\).*as they were"):
                self.switch(True, FakeHost({41}))
        self.assertEqual(hr.read_cameras(self.xr_path), FAST)

    def test_the_second_setting_failing_puts_the_first_back_too(self):
        self.write_conf("HANDS_MAX_HZ=45\n")
        real = hr.ft_conf.set_value

        def second_fails(key, value, path):
            if key == "HANDS_FULL_RATE":
                raise OSError("disk full")
            return real(key, value, path)
        with mock.patch.object(hr.ft_conf, "set_value", side_effect=second_fails):
            with self.assertRaisesRegex(hr.RateError, "as they were"):
                self.switch(True)
        self.assertEqual(self.conf(), {"HANDS_MAX_HZ": "45"})
        self.assertEqual(self.state(), {"on": False, "note": ""})

    def test_what_cannot_be_put_back_is_said(self):
        # The disk fills after the cameras' mode is written: the settings and the way back both fail.
        real, writes = hr.ft_store.write_json, []

        def first_only(*args, **kw):
            writes.append(args)
            if len(writes) > 1:
                raise OSError("No space left on device")
            return real(*args, **kw)
        with mock.patch.object(hr.ft_conf, "set_value", side_effect=OSError("No space left on device")), \
                mock.patch.object(hr.ft_store, "write_json", side_effect=first_only):
            with self.assertRaisesRegex(hr.RateError, r"frametop.conf couldn't be written.*couldn't be put back.*"
                                                      "the switch reads on now"):
                self.switch(True)
        self.assertTrue(self.state()["on"])

    def test_now_stops_tracking_restarts_xrservice_and_starts_camd_before_hands(self):
        host = FakeHost({41}, {41}, {52}, ring=(False, True))
        said = self.switch(True, host)
        self.assertEqual(host.calls, [
            "running_tracking",
            ("stop_tracking", list(hr.TRACKING_UNITS)),
            ("stop", [41]),
            "wait",
            "wait",
            ("start_tracking", [hr.CAMD_UNIT]),
            "ring_alive",
            "wait",
            "ring_alive",
            ("start_tracking", [hr.HANDS_UNIT]),
        ])
        self.assertIn("Head tracking restarted", said)

    def test_now_still_starts_hands_if_ring_wait_times_out(self):
        host = FakeHost({41}, {52}, ring=(False,))
        with mock.patch.object(hr, "CAMD_WAIT", 0.4):
            said = self.switch(True, host)
        self.assertIn("Head tracking restarted", said)
        self.assertEqual(host.calls[-4:], ["ring_alive", "wait", "ring_alive", ("start_tracking", [hr.HANDS_UNIT])])

    def test_now_with_no_tracking_running_skips_tracking_units(self):
        host = FakeHost({41}, {52}, running=())
        said = self.switch(True, host)
        self.assertEqual(host.calls, ["running_tracking", ("stop", [41]), "wait"])
        self.assertIn("Head tracking restarted", said)

    def test_now_with_only_one_tracking_unit_skips_ring_wait(self):
        host = FakeHost({41}, {52}, running=(hr.CAMD_UNIT,))
        said = self.switch(True, host)
        self.assertEqual(host.calls, [
            "running_tracking",
            ("stop_tracking", [hr.CAMD_UNIT]),
            ("stop", [41]),
            "wait",
            ("start_tracking", [hr.CAMD_UNIT]),
        ])
        self.assertIn("Head tracking restarted", said)

    def test_a_new_xrservice_counts_while_the_old_one_is_still_going(self):
        self.assertEqual(hr.restart_tracking(FakeHost({41, 42}, {41, 42}, {42, 52}))[0], True)

    def test_now_without_xrservice_restarts_hands_if_running_and_waits_for_steamvr(self):
        host = FakeHost()
        self.assertIn("when SteamVR next starts", self.switch(False, host))
        self.assertEqual(host.calls, ["running_tracking", "restart_hands"])
        idle = FakeHost(running=())
        self.assertIn("when SteamVR next starts", self.switch(False, idle))
        self.assertEqual(idle.calls, ["running_tracking"])

    def test_xrservice_that_stays_down_says_so(self):
        host = FakeHost({41}, set())
        restarted, said = hr.restart_tracking(host, wait=1.0, step=0.5)
        self.assertIs(restarted, False)
        self.assertIsNone(hr.restart_tracking(FakeHost())[0])   # it wasn't running: nothing to wait for
        self.assertIn("hasn't restarted", said)
        self.assertEqual(host.calls, [("stop", [41]), "wait", "wait"])

    def test_now_when_xrservice_stays_down_restores_tracking_units_without_ring_wait(self):
        host = FakeHost({41}, set())
        with mock.patch.object(hr, "RESTART_WAIT", 0.4):
            said = self.switch(True, host)
        self.assertIn("hasn't restarted", said)
        self.assertEqual(host.calls[-1], ("start_tracking", list(hr.TRACKING_UNITS)))
        self.assertNotIn("ring_alive", host.calls)

    def test_the_wait_outlasts_steamvrs_timer(self):
        host = FakeHost({41}, set())
        self.assertIs(hr.restart_tracking(host)[0], False)
        # 30 s in steps of 0.2 s: SteamVR starts a new XRService after 15 s (16.5 measured)
        self.assertEqual(host.calls.count("wait"), 150)

    def test_query_or_stop_tracking_failure_leaves_xrservice_alone(self):
        host = FakeHost({41}, querying="Failed to connect to bus")
        with self.assertRaisesRegex(hr.RateError, "settings are written, but hand tracking couldn't be queried"):
            self.switch(True, host)
        self.assertEqual(host.calls, ["running_tracking"])
        self.assertTrue(self.state()["on"])

        host = FakeHost({41}, stop_units="Access denied")
        with self.assertRaisesRegex(hr.RateError, "settings are written, but hand tracking couldn't be stopped"):
            self.switch(False, host)
        self.assertEqual(host.calls, ["running_tracking", ("stop_tracking", list(hr.TRACKING_UNITS))])
        self.assertFalse(self.state()["on"])

    def test_hands_that_cannot_be_restarted_without_xrservice_says_so(self):
        host = FakeHost(hands="Failed to connect to bus")
        with self.assertRaisesRegex(hr.RateError, "settings are written, but ft-hands couldn't be restarted"):
            self.switch(True, host)
        self.assertEqual(host.calls, ["running_tracking", "restart_hands"])

    def test_xrservice_that_cannot_be_stopped_restores_tracking_and_says_so(self):
        host = FakeHost({41}, stopping="Operation not permitted")
        with self.assertRaisesRegex(hr.RateError, r"settings are written, but XRService couldn't be stopped "
                                                  r"\(Operation not permitted\)"):
            self.switch(True, host)
        self.assertNotIn("wait", host.calls)
        self.assertEqual(host.calls, [
            "running_tracking",
            ("stop_tracking", list(hr.TRACKING_UNITS)),
            ("stop", [41]),
            ("start_tracking", list(hr.TRACKING_UNITS)),
        ])

    def test_start_tracking_failure_after_restart_says_so(self):
        host = FakeHost({41}, {52}, start_units={(hr.CAMD_UNIT,): "bus gone"})
        with self.assertRaisesRegex(hr.RateError, r"settings are written, but hand tracking couldn't be started \(bus gone\)"):
            self.switch(True, host)
        self.assertNotIn("ring_alive", host.calls)

        host = FakeHost({41}, {52}, start_units={(hr.HANDS_UNIT,): "unit masked"})
        with self.assertRaisesRegex(hr.RateError, r"settings are written, but hand tracking couldn't be started \(unit masked\)"):
            self.switch(False, host)
        self.assertIn("ring_alive", host.calls)


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

    def test_stop_asks_each_of_them_and_says_what_went_wrong(self):
        user = FakeUser()
        self.assertEqual(hr.Host(user).stop({52, 41}), "")
        # In C, whatever the user's language: "No such process" is read from what kill says.
        self.assertEqual(user.ran, [["env", "LC_ALL=C", "kill", "41"], ["env", "LC_ALL=C", "kill", "52"]])
        self.assertEqual(hr.Host(FakeUser(1, stderr="kill: (41): Operation not permitted\n")).stop({41}),
                         "kill: (41): Operation not permitted")
        self.assertEqual(hr.Host(FakeUser(1)).stop({41}), "kill 41: exit 1")

    def test_one_that_went_by_itself_is_not_a_failure(self):
        # The old XRService can go while the new one starts: the other is still asked to stop.
        user = FakeUser(each={"41": (1, "kill: (41): No such process\n")})
        self.assertEqual(hr.Host(user).stop({41, 52}), "")
        self.assertEqual([cmd[-1] for cmd in user.ran], ["41", "52"])

    def test_kill_answers_in_c_on_this_machine(self):
        # The real kill, on a process id past the kernel's highest (4194304): nothing is stopped.
        r = hr.handsmode.Host().run(["env", "LC_ALL=C", "kill", "4999999"])
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("No such process", r.stderr)

    def test_restart_hands_says_what_went_wrong(self):
        user = FakeUser()
        self.assertEqual(hr.Host(user).restart_hands(), "")
        self.assertEqual(user.ran, [["systemctl", "--user", "try-restart", "--no-block", "frametop-hands.service"]])
        self.assertEqual(hr.Host(FakeUser(1, stderr="Failed to connect to bus\n")).restart_hands(),
                         "Failed to connect to bus")
        self.assertEqual(hr.Host(FakeUser(5, stderr="Unit frametop-hands.service not found.")).restart_hands(), "")

    def test_running_tracking_parses_active_units_in_order(self):
        out = ("Id=frametop-camd.service\nActiveState=active\n\n"
               "Id=frametop-hands.service\nActiveState=activating\n")
        user = FakeUser(stdout=out)
        self.assertEqual(hr.Host(user).running_tracking(), (hr.TRACKING_UNITS, ""))
        self.assertEqual(user.ran, [["systemctl", "--user", "show", "-p", "Id,ActiveState", *hr.TRACKING_UNITS]])

        only_camd = ("Id=frametop-camd.service\nActiveState=reloading\n\n"
                     "Id=frametop-hands.service\nActiveState=inactive\n")
        self.assertEqual(hr.Host(FakeUser(stdout=only_camd)).running_tracking(), ((hr.CAMD_UNIT,), ""))
        self.assertEqual(hr.Host(FakeUser(1, stderr="Failed to connect to bus\n")).running_tracking(),
                         ((), "Failed to connect to bus"))

    def test_stop_and_start_tracking_order_and_errors(self):
        user = FakeUser()
        host = hr.Host(user)
        self.assertEqual(host.stop_tracking(()), "")
        self.assertEqual(host.start_tracking(()), "")
        self.assertEqual(user.ran, [])
        self.assertEqual(host.stop_tracking(hr.TRACKING_UNITS), "")
        self.assertEqual(host.start_tracking(hr.TRACKING_UNITS), "")
        self.assertEqual(user.ran, [
            ["systemctl", "--user", "stop", hr.HANDS_UNIT, hr.CAMD_UNIT],
            ["systemctl", "--user", "start", "--no-block", hr.CAMD_UNIT, hr.HANDS_UNIT],
        ])
        self.assertEqual(hr.Host(FakeUser(5, stderr="not found")).stop_tracking(hr.TRACKING_UNITS), "")
        self.assertEqual(hr.Host(FakeUser(5, stderr="not found")).start_tracking(hr.TRACKING_UNITS), "")
        self.assertEqual(hr.Host(FakeUser(1, stderr="bus gone\n")).stop_tracking(hr.TRACKING_UNITS), "bus gone")
        self.assertEqual(hr.Host(FakeUser(1, stderr="bus gone\n")).start_tracking(hr.TRACKING_UNITS), "bus gone")

    def test_ring_is_live_and_host_ring_alive(self):
        now = 10_500_000_000
        self.assertTrue(hr.ring_is_live(make_ring_header(heartbeat_ns=now - 100_000_000), now))
        self.assertFalse(hr.ring_is_live(make_ring_header(heartbeat_ns=0), now))
        self.assertFalse(hr.ring_is_live(make_ring_header(heartbeat_ns=now - hr.RING_STALE_NS), now))
        self.assertFalse(hr.ring_is_live(make_ring_header(heartbeat_ns=now + 1), now))
        self.assertFalse(hr.ring_is_live(make_ring_header(writer_pid=0, heartbeat_ns=now), now))
        self.assertFalse(hr.ring_is_live(make_ring_header(magic=b"WRONG001", heartbeat_ns=now), now))
        self.assertFalse(hr.ring_is_live(make_ring_header(version=2, heartbeat_ns=now), now))
        self.assertFalse(hr.ring_is_live(b"short", now))

        with tempfile.TemporaryDirectory() as tmp:
            ring = os.path.join(tmp, "cam-ring")
            host = hr.Host(FakeUser())
            self.assertFalse(host.ring_alive(ring))
            with open(ring, "wb") as f:
                f.write(make_ring_header(heartbeat_ns=time.clock_gettime_ns(time.CLOCK_MONOTONIC)))
            self.assertTrue(host.ring_alive(ring))
            with open(ring, "wb") as f:
                f.write(make_ring_header(heartbeat_ns=0))
            self.assertFalse(host.ring_alive(ring))


class Rates(unittest.TestCase):
    """What ft-camd's status lines say of the cameras, for scripts/update-check.py. The numbers
    are ft-camd's own, from the journal of 2026-11-22 and 23."""

    def rates(self, *lines):
        return hr.camera_rates("\n".join(STATUS.format(t=t, a=a, b=b, d=d) for t, a, b, d in lines))

    def test_the_mode_worn(self):
        got = self.rates((60, 12.9, 12.9, 0), (120, 59.0, 59.9, 7), (180, 59.9, 59.9, 7))
        self.assertEqual((got["best"], got["minutes"]), (59.9, 0))   # two minutes: neither has one on both sides
        self.assertLess(got["dark"], 0.01)

    def test_steamvrs_own_mode_is_dark_frames(self):
        # 30 bright a second and as many dark: the running count grows 1800 a minute.
        got = self.rates((9242, 2.1, 2.1, 3952), (9302, 30.0, 30.0, 5752), (9362, 29.6, 29.6, 7535),
                         (9422, 30.0, 30.0, 9335))
        self.assertEqual((got["best"], got["minutes"]), (30.0, 1))
        self.assertGreater(got["dark"], 0.4)

    def test_short_wears_are_no_full_minutes(self):
        # 80 s worn twice at 60 a second, each across one of ft-camd's lines: four minutes at 40,
        # none of them full, where a mode that runs slower gives full minutes at its rate.
        got = self.rates((60, 0.0, 0.0, 0), (120, 40.0, 40.0, 0), (180, 40.0, 40.0, 0), (240, 0.0, 0.0, 0),
                         (300, 40.0, 40.0, 1), (360, 40.0, 40.0, 1), (420, 0.0, 0.0, 1))
        self.assertEqual((got["best"], got["minutes"]), (40.0, 0))
        got = self.rates((60, 0.0, 0.0, 0), (120, 21.0, 21.0, 0), *((180 + 60 * i, 45.0, 45.0, 0) for i in range(4)),
                         (420, 30.0, 30.0, 0), (480, 0.0, 0.0, 0))
        self.assertEqual((got["best"], got["minutes"]), (45.0, 4))

    def test_the_headset_off_measures_nothing(self):
        # XRService wakes the cameras for some seconds now and then: 5.1 a second over a minute.
        got = self.rates((240, 5.1, 5.1, 0), (300, 0.0, 0.0, 0), (360, 5.1, 5.1, 0), (420, 0.0, 0.0, 0))
        self.assertEqual(got, {"best": 5.1, "minutes": 0, "dark": 0.0})
        self.assertIsNone(self.rates((240, 5.1, 5.1, 0), (300, 0.0, 0.0, 0))["dark"])   # too few frames to say

    def test_the_first_line_only_starts_the_count(self):
        self.assertEqual(self.rates((120, 59.9, 59.9, 0)), {"best": 0.0, "minutes": 0, "dark": None})
        self.assertEqual(hr.camera_rates(""), {"best": 0.0, "minutes": 0, "dark": None})
        self.assertEqual(hr.camera_rates("XRService is not running; start SteamVR on the headset first\n")["best"], 0.0)

    def test_ft_camd_starting_again_starts_the_count_again(self):
        # Its clock and its dark count both go back to nothing: that line measures against no other.
        got = self.rates((9422, 30.0, 30.0, 9335), (60, 40.0, 40.0, 0), (120, 59.9, 59.9, 3))
        self.assertEqual((got["best"], got["minutes"]), (59.9, 0))
        # Minutes either side of the start aren't one run of the cameras.
        got = self.rates((300, 30.0, 30.0, 0), (360, 30.0, 30.0, 0), (60, 30.0, 30.0, 0), (120, 30.0, 30.0, 0),
                         (180, 30.0, 30.0, 0))
        self.assertEqual(got["minutes"], 0)
        self.assertLess(got["dark"], 0.01)

    def test_lines_newest_first_measure_nothing(self):
        # journalctl -g with -n gives them so: no line follows the one before it.
        got = self.rates((180, 59.9, 59.9, 7), (120, 59.0, 59.9, 7), (60, 12.9, 12.9, 0))
        self.assertEqual((got["best"], got["minutes"]), (0.0, 0))

    def test_a_parked_line_counts_as_its_rate(self):
        line = STATUS.format(t=120, a=0.0, b=0.0, d=0).replace("video9 0.0", "video9 PARKED 0.0")
        got = hr.camera_rates(STATUS.format(t=60, a=59.9, b=59.9, d=0) + "\n" + line)
        self.assertEqual((got["best"], got["minutes"]), (0.0, 0))


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
        with mock.patch.object(hr, "Host", return_value=FakeHost({41}, querying="no bus")):
            self.write_xr("{}")
            code, out, err = self.main("on", "--now")
        self.assertEqual((code, out), (1, ""))
        self.assertIn("settings are written", err)


if __name__ == "__main__":
    unittest.main()
