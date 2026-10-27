"""CPU-only public ABI integration tests. No SteamVR runtime or hardware needed."""
import json
import os
from pathlib import Path
import socket
import stat
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build" / "input-tests"


def expected_inputs(hand):
    buttons = ["system", "bumper", "trigger", "grip", "thumbstick"]
    buttons += (["view", "dpad_up", "dpad_right", "dpad_down", "dpad_left"]
                if hand == "left" else ["menu", "a", "b", "x", "y"])
    values = {f"/input/{button}/{kind}": False for button in buttons for kind in ("click", "touch")}
    values["/input/thumbrest/touch"] = False
    values.update({f"/input/{path}": 0.0 for path in ("trigger/value", "grip/value", "thumbstick/x", "thumbstick/y")})
    return values


class InputProxyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        script = ROOT / "scripts/build-input.sh"
        if not script.exists():
            raise AssertionError("Missing input proxy build implementation")
        subprocess.run(["bash", str(script), str(BUILD)], check=True)
        import shlex
        flags = [os.environ.get("CXX", "g++"), "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread", "-isystem", str(ROOT / "vendor")]
        flags += shlex.split(os.environ.get("CXXFLAGS", ""))
        subprocess.run(flags + ["-shared", "-fPIC", str(ROOT / "tests/input_fake_cv.cpp"), "-o", str(BUILD / "driver_cv.so")], check=True)
        subprocess.run(flags + [str(ROOT / "tests/input_fake_runtime.cpp"), "-ldl", "-o", str(BUILD / "vrserver")], check=True)

    def setUp(self):
        scratch = os.environ.get("TMPDIR", str(ROOT / "build"))
        self.temp = tempfile.TemporaryDirectory(prefix="fi-", dir=scratch)
        self.sock = str(Path(self.temp.name) / "s")
        self.env = dict(os.environ, FRAME_TESTBENCH_SOCKET=self.sock,
                        FRAME_TESTBENCH_REAL_DRIVER=str(BUILD / "driver_cv.so"),
                        FRAME_TESTBENCH_PROXY=str(BUILD / "libframe_input.so"),
                        LD_PRELOAD=str(BUILD / "libframe_loader.so"))
        self.p = subprocess.Popen([str(BUILD / "vrserver")], env=self.env,
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  text=True)
        assert self.p.stdout is not None
        self.assertEqual(self.p.stdout.readline().strip(), "ready")

    def tearDown(self):
        if self.p.poll() is None:
            self.p.communicate("quit\n", timeout=5)
        self.assertEqual(self.p.returncode, 0)
        self.temp.cleanup()

    def runtime(self, command):
        assert self.p.stdin is not None and self.p.stdout is not None
        self.p.stdin.write(command + "\n")
        self.p.stdin.flush()
        return json.loads(self.p.stdout.readline())

    def rpc(self, command):
        with socket.socket(socket.AF_UNIX) as client:
            client.settimeout(2)
            client.connect(self.sock)
            client.sendall(command.encode() + b"\n")
            stream = client.makefile("rb")
            return json.loads(stream.readline())

    def test_public_abi_forwarding_and_identity(self):
        s = self.rpc("status")
        self.assertTrue(s["ok"])
        self.assertEqual(s["hmd_index"], 7)
        self.assertEqual(s["hmd_container"], 107)
        self.assertTrue(s["proximity_ready"])
        self.assertFalse(s["pose_override"])
        self.assertIsNone(s["worn_override"])
        self.assertEqual(stat.S_IMODE(os.stat(self.sock).st_mode), 0o600)
        r = self.runtime("forward")
        self.assertTrue(r["all_forwarded"])
        self.assertTrue(r["component_identity"])
        self.assertTrue(r["controller_identity"])
        self.assertTrue(r["factory_identity"])

    def test_synthetic_controller_registration_and_disconnected_defaults(self):
        s = self.rpc("status")
        self.assertIn("controllers", s)
        r = self.runtime("controllers")["controllers"]
        for k, hand in enumerate(("left", "right")):
            self.assertEqual(s["controllers"][hand], {"device_index": 20 + k, "pose_override": False, "pose": None, "synthetic": True, "inputs_ready": True, "inputs": expected_inputs(hand), "input_error": None})
            self.assertTrue(r[k]["registered"])
            self.assertGreater(r[k]["count"], 0)
            self.assertTrue(r[k]["raw"])
            self.assertFalse(r[k]["connected"])
            self.assertFalse(r[k]["valid"])
            self.assertFalse(r[k]["sample_connected"])
            self.assertFalse(r[k]["sample_valid"])
            self.assertEqual(r[k]["sample_qw"], 1)
            self.assertEqual(r[k]["role"], k + 1)
            self.assertEqual(r[k]["serial"], "frame_testbench_" + hand)
            self.assertTrue(r[k]["render_model"])
        self.assertTrue(self.runtime("controller-methods")["ok"])
        self.assertTrue(self.runtime("forward")["controller_identity"])

    def test_frame_input_registration_exact_paths_types_and_units(self):
        status = self.rpc("status")
        runtime = self.runtime("controllers")["controllers"]
        components = self.runtime("inputs")["components"]
        self.assertEqual(len(components), 50)
        self.assertEqual(len({c["handle"] for c in components}), 50)
        for k, hand in enumerate(("left", "right")):
            with self.subTest(hand=hand):
                self.assertEqual(runtime[k]["controller_type"], "frame_controller")
                self.assertEqual(runtime[k]["input_profile"], "{frame_controller}/input/frame_controller_profile.json")
                self.assertEqual(runtime[k]["manufacturer"], "Valve")
                self.assertEqual(runtime[k]["model"], "Steam Frame Controller")
                wanted = expected_inputs(hand)
                actual = {c["path"]: c for c in components if c["container"] == 120 + k}
                self.assertEqual(actual.keys(), wanted.keys())
                cstatus = status["controllers"][hand]
                self.assertTrue(cstatus["inputs_ready"])
                self.assertEqual(cstatus["inputs"], wanted)
                for path, value in wanted.items():
                    c = actual[path]
                    self.assertEqual(c["boolean"], isinstance(value, bool), path)
                    self.assertEqual(type(cstatus["inputs"][path]), bool if isinstance(value, bool) else int)
                    self.assertTrue(c["absolute"])
                    self.assertEqual(c["two_sided"], path in ("/input/thumbstick/x", "/input/thumbstick/y"))
                    self.assertEqual(c["value"], 0)
                    self.assertGreaterEqual(c["updates"], 1)

    def test_frame_input_registration_failure_never_claims_readiness(self):
        for env in ({"FAKE_CREATE_FAILURE": "/input/trigger/value"}, {"FAKE_INVALID_HANDLE": "1"}):
            with self.subTest(env=env):
                self.restart_runtime(**env)
                status = self.rpc("status")
                for hand in ("left", "right"):
                    c = status["controllers"][hand]
                    self.assertFalse(c.get("inputs_ready", True))
                    self.assertIsNotNone(c["input_error"])
                    self.assertNotIn("/input/trigger/value", c["inputs"])
                    self.assertTrue(self.rpc(f"controller-pose {hand} 0 0 0 1 0 0 0")["ok"])
                    self.assertFalse(self.rpc(f"controller-inputs {hand} /input/system/click 1")["ok"])

    def connect_controllers(self):
        for hand in ("left", "right"):
            self.assertTrue(self.rpc(f"controller-pose {hand} 1 2 3 1 0 0 0")["ok"])

    def runtime_inputs(self, hand):
        container = 120 if hand == "left" else 121
        return {c["path"]: c["value"] for c in self.runtime("inputs")["components"]
                if c["container"] == container and c["live"]}

    def test_frame_inputs_publish_every_component_and_preserve_physical_input(self):
        self.connect_controllers()
        for hand in ("left", "right"):
            values = {path: (True if isinstance(value, bool) else
                            -0.75 if path.endswith(("/x", "/y")) else 0.625)
                      for path, value in expected_inputs(hand).items()}
            command = "controller-inputs " + hand + " " + " ".join(
                f"{path} {int(value) if isinstance(value, bool) else value}" for path, value in values.items())
            result = self.rpc(command)
            self.assertTrue(result["ok"], result)
            self.assertEqual(result["controllers"][hand]["inputs"], values)
            self.assertEqual(self.runtime_inputs(hand), values)
        before = self.rpc("status")["controllers"]
        self.runtime("physical")
        self.assertTrue(self.runtime("forward")["all_forwarded"])
        self.assertFalse(self.runtime("metrics")["other_worn"])
        self.assertEqual(self.runtime("metrics")["other_x"], 99)
        time.sleep(0.03)
        self.assertEqual(self.rpc("status")["controllers"], before)
        # A publication stores the float delivered to the SDK, not the unrounded request.
        result = self.rpc("controller-inputs left /input/trigger/value 0.1")
        self.assertTrue(result["ok"])
        import struct
        self.assertEqual(result["controllers"]["left"]["inputs"]["/input/trigger/value"],
                         struct.unpack("f", struct.pack("f", 0.1))[0])

    def test_frame_inputs_validate_complete_batch_before_any_write(self):
        self.connect_controllers()
        self.assertTrue(self.rpc("controller-inputs left /input/trigger/value 0.5 /input/system/click 1")["ok"])
        before = self.rpc("status")
        runtime_before = self.runtime("inputs")
        commands = ["controller-inputs", "controller-inputs all /input/system/click 1",
                    "controller-inputs LEFT /input/system/click 1", "controller-inputs left",
                    "controller-inputs left /input/system/click", "controller-inputs left /input/menu/click 1",
                    "controller-inputs right /input/view/touch 1", "controller-input-release",
                    "controller-input-release both", "controller-input-release left extra"]
        for path in ("/input/thumbrest/click", "/input/trigger", "/input/skeleton/left", "/output/haptic", "/input/nope/click"):
            commands.append(f"controller-inputs left /input/grip/value 1 {path} 1")
        for value in ("true", "false", "2", "-1", "0.0", "1.0", "+1", "01", "nan", "1junk"):
            commands.append(f"controller-inputs left /input/grip/value 1 /input/system/click {value}")
        for path in ("/input/trigger/value", "/input/grip/value", "/input/thumbstick/x", "/input/thumbstick/y"):
            for value in ("nan", "NaN", "inf", "-inf", "1e999", "1.00000001", "-1.00000001", "0.5junk"):
                commands.append(f"controller-inputs left /input/system/click 0 {path} {value}")
        commands += ["controller-inputs left /input/trigger/value -0.01",
                     "controller-inputs left /input/grip/value -1",
                     "controller-inputs left /input/system/click 0 /input/system/click 1",
                     "controller-inputs left /input/trigger/value 1 /input/trigger/value 0"]
        for command in commands:
            with self.subTest(command=command):
                bad = self.rpc(command)
                self.assertFalse(bad.pop("ok"))
                self.assertTrue(bad.pop("error"))
                self.assertEqual(bad, {k: v for k, v in before.items() if k != "ok"})
                self.assertEqual(self.runtime("inputs"), runtime_before)

    def test_frame_inputs_require_connected_pose_and_accept_scalar_boundaries(self):
        before = self.runtime("inputs")
        for hand in ("left", "right"):
            bad = self.rpc(f"controller-inputs {hand} /input/system/click 1")
            self.assertFalse(bad["ok"])
            self.assertEqual(bad["error"], "controller pose is not connected")
        self.assertEqual(self.runtime("inputs"), before)
        self.connect_controllers()
        for value in (0, 1):
            self.assertTrue(self.rpc(f"controller-inputs left /input/trigger/value {value} /input/grip/value {value}")["ok"])
        for value in (-1, 0, 1):
            self.assertTrue(self.rpc(f"controller-inputs right /input/thumbstick/x {value} /input/thumbstick/y {value}")["ok"])

    def test_frame_inputs_report_partial_runtime_failure_and_recover(self):
        self.connect_controllers()
        self.runtime("fail-input /input/trigger/value")
        before = self.rpc("status")
        bad = self.rpc("controller-inputs left /input/system/click 1 /input/trigger/value 0.5 /input/grip/value 1")
        self.assertFalse(bad["ok"])
        self.assertIn("earlier writes may have applied", bad["error"])
        c = bad["controllers"]["left"]
        self.assertGreater(bad["sequence"], before["sequence"])
        self.assertTrue(c["inputs_ready"])
        self.assertEqual(c["input_error"], {"path": "/input/trigger/value", "code": 4})
        self.assertTrue(c["inputs"]["/input/system/click"])
        self.assertEqual(c["inputs"]["/input/trigger/value"], 0)
        self.assertEqual(c["inputs"]["/input/grip/value"], 0)
        self.assertEqual(self.runtime_inputs("left"), c["inputs"])
        self.assertEqual(self.rpc("status")["controllers"]["left"], c)
        good = self.rpc("controller-inputs left /input/trigger/value 0.5 /input/grip/value 1")
        self.assertTrue(good["ok"])
        self.assertIsNone(good["controllers"]["left"]["input_error"])
        self.assertEqual(self.runtime_inputs("left"), good["controllers"]["left"]["inputs"])

    def press_all_inputs(self):
        self.connect_controllers()
        for hand in ("left", "right"):
            command = "controller-inputs " + hand + " " + " ".join(f"{path} 1" for path in expected_inputs(hand))
            self.assertTrue(self.rpc(command)["ok"])

    def test_frame_input_release_is_selective_without_releasing_pose(self):
        self.press_all_inputs()
        self.rpc("pose 4 5 6 1 0 0 0")
        self.rpc("worn 1")
        before = self.rpc("status")["controllers"]
        for command in ("pose-release", "worn-release"):
            self.assertEqual(self.rpc(command)["controllers"], before)
        s = self.rpc("controller-input-release left")
        self.assertTrue(s["ok"])
        self.assertTrue(s["controllers"]["left"]["pose_override"])
        self.assertEqual(s["controllers"]["left"]["pose"], before["left"]["pose"])
        self.assertEqual(s["controllers"]["left"]["inputs"], expected_inputs("left"))
        self.assertEqual(self.runtime_inputs("left"), expected_inputs("left"))
        self.assertEqual(s["controllers"]["right"], before["right"])
        for command in ("controller-input-release right", "controller-input-release all", "controller-input-release all"):
            s = self.rpc(command)
            self.assertTrue(s["ok"])
            for hand in ("left", "right"):
                self.assertTrue(s["controllers"][hand]["pose_override"])
                self.assertEqual(self.runtime_inputs(hand), expected_inputs(hand))

    def test_frame_inputs_neutralized_before_selective_and_global_disconnect(self):
        self.press_all_inputs()
        s = self.rpc("controller-release left")
        self.assertTrue(s["ok"])
        self.assertFalse(s["controllers"]["left"]["pose_override"])
        self.assertEqual(self.runtime_inputs("left"), expected_inputs("left"))
        self.assertTrue(all(self.runtime_inputs("right").values()))
        self.assertEqual(self.runtime("inputs")["nonneutral_disconnects"], 0)
        self.assertTrue(self.rpc("release")["ok"])
        self.assertEqual(self.runtime_inputs("right"), expected_inputs("right"))
        self.assertFalse(self.runtime("controllers")["controllers"][1]["connected"])
        self.assertEqual(self.runtime("inputs")["nonneutral_disconnects"], 0)
        self.assertFalse(self.rpc("controller-inputs right /input/a/click 1")["ok"])

    def test_frame_inputs_release_failure_reports_state_and_allows_retry(self):
        for command in ("controller-input-release left", "controller-release left", "release"):
            with self.subTest(command=command):
                self.press_all_inputs()
                self.runtime("fail-input /input/trigger/value")
                s = self.rpc(command)
                self.assertFalse(s["ok"])
                c = s["controllers"]["left"]
                self.assertTrue(c["pose_override"])
                self.assertIsNotNone(c["input_error"])
                self.assertEqual(c["inputs"]["/input/trigger/value"], 1)
                self.assertEqual(c["inputs"]["/input/system/click"], False)
                self.assertEqual(self.runtime_inputs("left"), c["inputs"])
                self.assertEqual(self.runtime("inputs")["nonneutral_disconnects"], 0)
                self.assertTrue(self.rpc(command)["ok"])
                self.assertEqual(self.runtime_inputs("left"), expected_inputs("left"))
                self.assertEqual(self.runtime("inputs")["nonneutral_disconnects"], 0)

    def test_frame_inputs_lifecycle_neutralizes_and_replaces_handles(self):
        self.press_all_inputs()
        old = self.runtime("inputs")["components"]
        self.runtime("controller-deactivate-left")
        s = self.rpc("status")["controllers"]["left"]
        self.assertFalse(s["inputs_ready"])
        self.assertEqual(s["inputs"], {})
        after = self.runtime("inputs")
        self.assertEqual(after["nonneutral_disconnects"], 0)
        self.assertTrue(all(c["value"] == 0 for c in after["components"] if c["container"] == 120))
        self.assertTrue(all(self.runtime_inputs("right").values()))
        self.runtime("controller-activate")
        new = self.runtime("inputs")["components"]
        old_handles = {c["handle"] for c in old}
        self.assertTrue(old_handles.isdisjoint(c["handle"] for c in new if c["live"]))
        self.press_all_inputs()
        self.runtime("cleanup")
        self.assertTrue(all(c["value"] == 0 for c in self.runtime("inputs")["components"]))
        self.assertEqual(self.runtime("inputs")["nonneutral_disconnects"], 0)
        before = self.runtime("inputs")
        time.sleep(0.03)
        self.assertEqual(self.runtime("inputs"), before)
        self.assertTrue(self.runtime("controller-late-activate")["rejected"])

    def test_controller_translation_rotation_and_ticks_without_hmd(self):
        for hand, xyz, q in [("left", [1, 2, 3], [0, 2, 0, 0]), ("right", [-4, 5, -6], [0, 0, 0, 3])]:
            s = self.rpc("controller-pose " + hand + " " + " ".join(map(str, xyz + q)))
            self.assertTrue(s["ok"], s)
            c = s["controllers"][hand]
            self.assertTrue(c["pose_override"])
            self.assertEqual(c["pose"]["position"], xyz)
            self.assertEqual(c["pose"]["quaternion"], [int(v != 0) for v in q])
            self.assertTrue(c["pose"]["valid"])
            self.assertTrue(c["pose"]["connected"])
        self.runtime("physical")
        before = self.runtime("controllers")["controllers"]
        # HMD state must not gate controller publication, including an inactive HMD.
        self.runtime("deactivate")
        time.sleep(0.08)
        after = self.runtime("controllers")["controllers"]
        for k, xyz, q in [(0, [1, 2, 3], [0, 1, 0, 0]), (1, [-4, 5, -6], [0, 0, 0, 1])]:
            self.assertGreater(after[k]["count"], before[k]["count"] + 2)
            self.assertEqual(after[k]["position"], xyz)
            self.assertEqual(after[k]["quaternion"], q)
            self.assertEqual(after[k]["sample_x"], xyz[0])
            for field in ("raw", "running", "valid", "connected", "sample_connected", "sample_valid"):
                self.assertTrue(after[k][field], field)
        self.assertEqual(self.runtime("metrics")["other_x"], 99)
        self.assertFalse(self.runtime("metrics")["other_worn"])
        s = self.rpc("status")
        self.assertFalse(s["pose_override"])
        self.assertIsNone(s["worn_override"])
        self.assertIsNone(s["hmd_index"])
        self.assertTrue(self.rpc("controller-pose left 7 8 9 1e308 -1e308 0 0")["ok"])
        q = self.rpc("status")["controllers"]["left"]["pose"]["quaternion"]
        self.assertAlmostEqual(sum(v*v for v in q), 1)
        self.assertAlmostEqual(q[0], -q[1])
        self.assertEqual(self.rpc("status")["controllers"]["right"]["pose"]["position"], [-4, 5, -6])

    def test_controller_release_is_selective_and_disconnects(self):
        for cmd in ("pose 10 20 30 1 0 0 0", "worn 1",
                    "controller-pose left 1 2 3 1 0 0 0", "controller-pose right 4 5 6 1 0 0 0"):
            self.assertTrue(self.rpc(cmd)["ok"])
        s = self.rpc("controller-release left")
        self.assertTrue(s["ok"], s)
        self.assertTrue(s["pose_override"])
        self.assertTrue(s["worn_override"])
        self.assertEqual(s["controllers"]["left"], {"device_index": 20, "pose_override": False, "pose": None, "synthetic": True, "inputs_ready": True, "inputs": expected_inputs("left"), "input_error": None})
        self.assertTrue(s["controllers"]["right"]["pose_override"])
        before = self.runtime("controllers")["controllers"]
        self.assertFalse(before[0]["connected"])
        self.assertFalse(before[0]["valid"])
        self.assertTrue(before[0]["raw"])
        self.assertEqual(before[0]["quaternion"], [1, 0, 0, 0])
        time.sleep(0.05)
        after = self.runtime("controllers")["controllers"]
        self.assertEqual(after[0]["count"], before[0]["count"])
        self.assertGreater(after[1]["count"], before[1]["count"])
        for cmd in ("pose-release", "worn-release"):
            self.assertTrue(self.rpc(cmd)["controllers"]["right"]["pose_override"])
        self.assertTrue(self.rpc("controller-release all")["ok"])
        self.assertFalse(self.runtime("controllers")["controllers"][1]["connected"])
        # Idempotent release of an already-disconnected device is accepted.
        self.assertTrue(self.rpc("controller-release right")["ok"])

    def test_global_release_clears_controller_and_hmd_overrides(self):
        for cmd in ("pose 10 20 30 1 0 0 0", "worn 1",
                    "controller-pose left 1 2 3 1 0 0 0", "controller-pose right 4 5 6 1 0 0 0"):
            self.assertTrue(self.rpc(cmd)["ok"])
        s = self.rpc("release")
        for hand in ("left", "right"):
            self.assertFalse(s["controllers"][hand]["pose_override"])
            self.assertIsNone(s["controllers"][hand]["pose"])
        self.assertFalse(s["pose_override"])
        self.assertIsNone(s["worn_override"])
        self.assertEqual(self.runtime("metrics")["hmd_pose"][0], 42)
        self.assertFalse(self.runtime("metrics")["worn"])
        for c in self.runtime("controllers")["controllers"]:
            self.assertFalse(c["connected"])
            self.assertFalse(c["sample_connected"])

    def test_controller_cleanup_disconnects_and_stops_all_ticks(self):
        self.rpc("controller-pose left 1 2 3 1 0 0 0")
        self.rpc("controller-pose right 4 5 6 1 0 0 0")
        self.runtime("cleanup")
        before = self.runtime("controllers")["controllers"]
        for c in before:
            self.assertFalse(c["connected"])
            self.assertFalse(c["sample_connected"])
        time.sleep(0.05)
        self.assertEqual(before, self.runtime("controllers")["controllers"])
        self.assertFalse(os.path.exists(self.sock))

    def test_controller_activation_after_cleanup_is_rejected(self):
        self.runtime("cleanup")
        self.assertTrue(self.runtime("controller-late-activate")["rejected"])

    def test_controller_deactivate_clears_readiness_without_stopping_other_hand(self):
        self.rpc("controller-pose left 1 2 3 1 0 0 0")
        self.rpc("controller-pose right 4 5 6 1 0 0 0")
        self.runtime("controller-deactivate-left")
        s = self.rpc("status")
        self.assertEqual(s["controllers"]["left"], {"device_index": None, "pose_override": False, "pose": None, "synthetic": True, "inputs_ready": False, "inputs": {}, "input_error": None})
        self.assertEqual(s["hmd_index"], 7)
        before = self.runtime("controllers")["controllers"]
        self.assertFalse(before[0]["sample_connected"])
        time.sleep(0.05)
        after = self.runtime("controllers")["controllers"]
        self.assertEqual(before[0]["count"], after[0]["count"])
        self.assertGreater(after[1]["count"], before[1]["count"])
        bad = self.rpc("controller-pose left 9 8 7 1 0 0 0")
        self.assertFalse(bad["ok"])
        self.assertEqual(bad["error"], "controller is not active")
        self.assertEqual(bad["sequence"], s["sequence"])
        self.runtime("controller-activate")
        self.assertTrue(self.rpc("controller-pose left 9 8 7 1 0 0 0")["ok"])

    def restart_runtime(self, **env):
        self.p.communicate("quit\n", timeout=5)
        self.assertEqual(self.p.returncode, 0)
        self.p = subprocess.Popen([str(BUILD / "vrserver")], env=dict(self.env, **env),
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        assert self.p.stdout is not None
        self.assertEqual(self.p.stdout.readline().strip(), "ready")

    def test_controller_readiness_waits_for_activation_and_registration_acceptance(self):
        for env in ("FAKE_DEFER_CONTROLLERS", "FAKE_REJECT_CONTROLLERS"):
            with self.subTest(env=env):
                self.restart_runtime(**{env: "1"})
                before = self.rpc("status")
                for hand in ("left", "right"):
                    self.assertIsNone(before["controllers"][hand]["device_index"])
                    bad = self.rpc("controller-pose " + hand + " 1 2 3 1 0 0 0")
                    self.assertFalse(bad["ok"])
                    self.assertEqual(bad["error"], "controller is not active")
                    self.assertEqual(bad["sequence"], before["sequence"])
                self.assertTrue(self.rpc("pose 1 2 3 1 0 0 0")["ok"])
                self.assertTrue(self.rpc("controller-release all")["ok"])
                if env == "FAKE_DEFER_CONTROLLERS":
                    self.runtime("controller-activate")
                    self.assertTrue(self.rpc("controller-pose right 1 2 3 1 0 0 0")["ok"])

    def test_controller_malformed_commands_do_not_mutate_any_channel(self):
        for cmd in ("pose 10 20 30 1 0 0 0", "worn 1",
                    "controller-pose left 1 2 3 1 0 0 0", "controller-pose right 4 5 6 1 0 0 0"):
            self.assertTrue(self.rpc(cmd)["ok"])
        before = self.rpc("status")
        for cmd in ("controller-pose", "controller-pose all 1 2 3 1 0 0 0",
                    "controller-pose LEFT 1 2 3 1 0 0 0", "controller-pose left 1 2 3",
                    "controller-pose left nan 2 3 1 0 0 0", "controller-pose right 1 2 3 inf 0 0 0",
                    "controller-pose left 1 2 3 0 0 0 0", "controller-pose right 1 2 3 1 0 0 0 extra",
                    "controller-release", "controller-release both", "controller-release all extra"):
            with self.subTest(cmd=cmd):
                bad = self.rpc(cmd)
                self.assertFalse(bad.pop("ok"))
                self.assertTrue(bad.pop("error"))
                self.assertEqual(bad, {k: v for k, v in before.items() if k != "ok"})

    def test_concurrent_controller_commands_and_physical_updates_release_coherently(self):
        from concurrent.futures import ThreadPoolExecutor
        def commands(hand):
            for _ in range(25):
                self.assertTrue(self.rpc("controller-pose " + hand + " 4 5 6 1 1 0 0")["ok"])
                self.assertTrue(self.rpc("controller-release " + hand)["ok"])
        def physical():
            for _ in range(40):
                self.runtime("physical")
        with ThreadPoolExecutor(max_workers=3) as pool:
            futures = [pool.submit(commands, hand) for hand in ("left", "right")] + [pool.submit(physical)]
            for f in futures:
                f.result(timeout=5)
        self.rpc("release")
        before = self.runtime("controllers")["controllers"]
        time.sleep(0.05)
        self.assertEqual(before, self.runtime("controllers")["controllers"])
        for c in before:
            self.assertFalse(c["connected"])
        self.assertEqual(self.runtime("metrics")["other_x"], 99)
        self.assertTrue(self.runtime("forward")["controller_identity"])

    def test_raw_pose_persists_and_only_hmd_is_replaced(self):
        s = self.rpc("pose 1 2 3 2 0 0 0")
        self.assertTrue(s["ok"])
        self.assertEqual(s["requested_pose"], [1, 2, 3, 1, 0, 0, 0])
        self.assertTrue(s["pose_override"])
        self.runtime("physical")
        time.sleep(0.06)
        r = self.runtime("metrics")
        self.assertEqual(r["hmd_pose"], [1, 2, 3, 1, 0, 0, 0])
        self.assertTrue(r["raw_transform"])
        self.assertTrue(r["valid"])
        self.assertEqual(r["other_x"], 99)
        self.assertGreater(r["pose_count"], 3)
        self.assertIsNone(self.rpc("status")["worn_override"])
        # A short-lived controller connection must not release its override.
        time.sleep(0.1)
        self.assertTrue(self.rpc("status")["pose_override"])

    def test_independent_worn_suppression_ticks_and_release_latest_physical(self):
        before = self.rpc("status")
        s = self.rpc("worn 1")
        self.assertGreater(s["sequence"], before["sequence"])
        self.assertFalse(s["pose_override"])
        self.runtime("physical")
        time.sleep(0.05)
        r = self.runtime("metrics")
        self.assertTrue(r["worn"])
        self.assertFalse(r["other_worn"])
        self.assertGreater(r["worn_count"], 3)
        self.assertEqual(r["hmd_pose"][0], 42)
        self.rpc("pose 1 2 3 1 0 0 0")
        s = self.rpc("release")
        self.assertFalse(s["pose_override"])
        self.assertIsNone(s["worn_override"])
        self.assertEqual(s["physical_pose"]["position"][0], 42)
        self.assertFalse(s["physical_worn"])
        r = self.runtime("metrics")
        self.assertFalse(r["worn"])
        self.assertEqual(r["hmd_pose"][0], 42)
        self.assertFalse(r["raw_transform"])
        self.assertFalse(r["valid"])
        self.rpc("worn 0")
        self.assertFalse(self.runtime("metrics")["worn"])

    def test_malformed_commands_do_not_mutate_state(self):
        s = self.rpc("pose 1 2 3 1 0 0 0")
        for cmd in ["", "blah", "status extra", "release extra", "worn 2", "worn -1", "worn 1 extra",
                    "pose 1 2", "pose nan 0 0 1 0 0 0", "pose 0 0 0 0 0 0 0",
                    "pose 0 0 0 inf 0 0 0", "pose 0 0 0 1 0 0 0 extra"]:
            with self.subTest(cmd=cmd):
                bad = self.rpc(cmd)
                self.assertFalse(bad["ok"])
                self.assertEqual(bad["sequence"], s["sequence"])
                self.assertEqual(bad["requested_pose"], s["requested_pose"])

    def test_idle_client_does_not_block_publisher_or_other_clients(self):
        self.rpc("worn 1")
        with socket.socket(socket.AF_UNIX) as idle:
            idle.connect(self.sock)
            idle.sendall(b"po")
            time.sleep(0.04)
            self.assertTrue(self.rpc("status")["worn_override"])
            self.assertGreater(self.runtime("metrics")["worn_count"], 2)

    def test_selective_release_keeps_other_override(self):
        self.rpc("pose 1 2 3 1 0 0 0")
        self.rpc("worn 1")
        s = self.rpc("pose-release")
        self.assertTrue(s["ok"])
        self.assertFalse(s["pose_override"])
        self.assertTrue(s["worn_override"])
        self.assertEqual(self.runtime("metrics")["hmd_pose"][0], 42)
        self.rpc("pose 4 5 6 1 0 0 0")
        s = self.rpc("worn-release")
        self.assertTrue(s["ok"])
        self.assertTrue(s["pose_override"])
        self.assertIsNone(s["worn_override"])
        self.assertFalse(self.runtime("metrics")["worn"])

    def test_physical_pose_reports_its_driver_transforms(self):
        p = self.rpc("status")["physical_pose"]
        self.assertEqual(p["world_from_driver"]["position"], [5, 0, 0])
        self.assertEqual(p["driver_from_head"]["position"], [0, 6, 0])
        self.assertFalse(p["valid"])

    def test_deactivate_stops_ticks_and_clears_readiness(self):
        self.rpc("pose 1 2 3 1 0 0 0")
        self.rpc("worn 1")
        self.assertTrue(self.runtime("deactivate")["deactivated"])
        s = self.rpc("status")
        self.assertIsNone(s["hmd_index"])
        self.assertFalse(s["proximity_ready"])
        count = self.runtime("metrics")["pose_count"]
        time.sleep(0.05)
        self.assertEqual(count, self.runtime("metrics")["pose_count"])
        self.assertFalse(self.rpc("worn 0")["ok"])
        self.assertFalse(self.rpc("pose 0 0 0 1 0 0 0")["ok"])

    def test_loader_leaves_other_executables_and_other_paths_untouched(self):
        import shutil
        unscoped = Path(self.temp.name) / "not-vrserver"
        shutil.copy2(BUILD / "vrserver", unscoped)
        env = dict(self.env, FRAME_TESTBENCH_SOCKET=str(Path(self.temp.name) / "unused"))
        r = subprocess.run([str(unscoped)], env=env, input="forward\nquit\n", text=True, capture_output=True, timeout=3)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue(json.loads(r.stdout.splitlines()[1])["all_forwarded"])
        self.assertFalse(os.path.exists(env["FRAME_TESTBENCH_SOCKET"]))
        r = subprocess.run([str(BUILD / "vrserver"), "scope"], env=self.env, text=True, capture_output=True, timeout=3)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.strip(), "scope-ok")

    def test_restart_reclaims_only_stale_owned_socket(self):
        path = str(Path(self.temp.name) / "stale")
        with socket.socket(socket.AF_UNIX) as stale:
            stale.bind(path)
        env = dict(self.env, FRAME_TESTBENCH_SOCKET=path)
        r = subprocess.run([str(BUILD / "vrserver")], env=env, input="quit\n", text=True, capture_output=True, timeout=3)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertFalse(os.path.exists(path))

    def test_artifacts_export_only_loader_and_factory_and_no_cpp_runtime_dependency(self):
        expected = {"libframe_input.so": {"HmdDriverFactory"},
                    "libframe_loader.so": {"dlopen", "frame_real_dlopen"}}
        for name, symbols in expected.items():
            exports = subprocess.check_output(["nm", "-D", "--defined-only", str(BUILD / name)], text=True)
            self.assertEqual({line.split()[-1] for line in exports.splitlines()}, symbols)
            elf = subprocess.check_output(["objdump", "-p", str(BUILD / name)], text=True)
            needed = [line for line in elf.splitlines() if "NEEDED" in line]
            self.assertFalse(any("libstdc++" in line for line in needed))

    def test_concurrent_commands_and_physical_updates_finish_coherently(self):
        from concurrent.futures import ThreadPoolExecutor
        self.rpc("pose 1 2 3 1 0 0 0")
        def physical_updates():
            for _ in range(40):
                self.runtime("physical")
        def commands():
            for _ in range(20):
                self.assertTrue(self.rpc("worn 1")["ok"])
                self.assertTrue(self.rpc("pose 4 5 6 1 1 0 0")["ok"])
        with ThreadPoolExecutor(max_workers=2) as pool:
            a = pool.submit(physical_updates)
            b = pool.submit(commands)
            a.result(timeout=5)
            b.result(timeout=5)
        self.rpc("release")
        time.sleep(0.05)
        result = self.runtime("metrics")
        self.assertEqual(result["hmd_pose"][0], 42)
        self.assertFalse(result["worn"])

    def test_oversized_and_fragmented_requests(self):
        self.assertFalse(self.rpc("x" * 2000)["ok"])
        with socket.socket(socket.AF_UNIX) as client:
            client.settimeout(2)
            client.connect(self.sock)
            client.sendall(b"sta")
            time.sleep(0.01)
            client.sendall(b"tus\n")
            with client.makefile("rb") as stream:
                self.assertTrue(json.loads(stream.readline())["ok"])

    def test_pose_normalizes_huge_and_nontrivial_quaternions(self):
        s = self.rpc("pose 0 0 0 1e308 1e308 0 0")
        self.assertTrue(s["ok"])
        q = s["pose"]["quaternion"]
        self.assertAlmostEqual(sum(v*v for v in q), 1)
        self.assertAlmostEqual(q[0], q[1])
        self.assertEqual(q[2:], [0, 0])

    def test_socket_collision_never_replaces_live_socket_or_file(self):
        from pathlib import Path
        for path in [self.sock, str(Path(self.temp.name) / "file")]:
            if path != self.sock:
                Path(path).write_text("preserve-me")
            inode = os.lstat(path).st_ino
            env = dict(self.env, FRAME_TESTBENCH_SOCKET=path)
            result = subprocess.run([str(BUILD / "vrserver")], env=env, input="quit\n", text=True, capture_output=True, timeout=3)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(os.lstat(path).st_ino, inode)
        self.assertTrue(self.rpc("status")["ok"])
        self.assertEqual(Path(self.temp.name, "file").read_text(), "preserve-me")

    def test_cleanup_stops_worker_and_removes_socket(self):
        self.rpc("pose 1 2 3 1 0 0 0")
        self.assertTrue(self.runtime("cleanup")["cleaned"])
        self.assertFalse(os.path.exists(self.sock))
        first = self.runtime("metrics")["pose_count"]
        time.sleep(0.04)
        self.assertEqual(first, self.runtime("metrics")["pose_count"])


if __name__ == "__main__":
    unittest.main()
