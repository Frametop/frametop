"""travel_mode: Input Settings' switch for SteamVR's travel mode, without Qt or SteamVR.

The host runner runs travel_mode's real shell script here, with a stub vrcmd in a temp folder
(VRCMD points at it) and a stub pgrep on PATH, so the real vrcmd never runs. The stub keeps
the setting in a file and logs each call with the environment it got.
Run: python3 input-settings/test/test_travel_mode.py"""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import travel_mode  # noqa: E402

# Answers --settings-bool from $STATE (no line until it's set, as SteamVR does for an unset key)
# and stores --set-settings-bool there, unless $IGNORE_SET is set.
VRCMD = """#!/bin/sh
echo "$* XDG=$XDG_CONFIG_HOME LD=$LD_LIBRARY_PATH" >>"$LOG"
case "$1" in
  --settings-bool) [ -f "$STATE" ] && echo "$2=$(cat "$STATE")"; exit 0 ;;
  --set-settings-bool) [ -n "$IGNORE_SET" ] || { [ "$3" = 1 ] && echo true || echo false; } >"$STATE"; exit 0 ;;
esac
exit 1
"""
PGREP = '#!/bin/sh\n[ -n "$VRSERVER" ]\n'


class TravelMode(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(prefix="ft-travel-"))
        self.bin = self.dir / "steamvr"
        self.bin.mkdir()
        self.vrcmd = self.bin / "vrcmd"
        self.vrcmd.write_text(VRCMD)
        self.vrcmd.chmod(0o755)
        tools = self.dir / "tools"
        tools.mkdir()
        (tools / "pgrep").write_text(PGREP)
        (tools / "pgrep").chmod(0o755)
        self.log, self.state = self.dir / "log", self.dir / "state"
        self.env = dict(os.environ, PATH=f"{tools}:{os.environ['PATH']}", HOME=str(self.dir),
                        LOG=str(self.log), STATE=str(self.state), VRSERVER="1")
        self.env.pop("IGNORE_SET", None)
        self.ran = []
        old = travel_mode.VRCMD
        travel_mode.VRCMD = str(self.vrcmd)
        self.addCleanup(setattr, travel_mode, "VRCMD", old)

    def host(self, *cmd, timeout=20):
        self.ran.append(cmd)
        self.assertNotIn("/opt/steamvr/bin/linuxarm64/vrcmd", cmd)  # never the real one
        return subprocess.run(list(cmd), capture_output=True, text=True, timeout=timeout, env=self.env)

    def calls(self):
        return self.log.read_text().splitlines() if self.log.exists() else []

    def test_unset_reads_as_off(self):
        self.assertEqual(travel_mode.get(self.host), (False, ""))
        self.assertEqual(self.calls()[0].split()[:2], ["--settings-bool", travel_mode.KEY])

    def test_on_then_off(self):
        ok, text, got = travel_mode.put(self.host, True)
        self.assertTrue(ok, text)
        self.assertIs(got, True)
        self.assertIn("Travel mode on", text)
        self.assertEqual(travel_mode.get(self.host), (True, ""))
        # 1, not "true" (vrcmd reads "true" as 0), with SteamVR's config folder and libraries.
        self.assertEqual(self.calls()[0],
                         f"--set-settings-bool {travel_mode.KEY} 1 XDG={self.dir}/.config LD={self.bin}")
        ok, text, got = travel_mode.put(self.host, False)
        self.assertTrue(ok, text)
        self.assertIs(got, False)
        self.assertEqual(text, "Travel mode off")
        self.assertEqual(travel_mode.get(self.host), (False, ""))
        self.assertIn(f"--set-settings-bool {travel_mode.KEY} 0", self.calls()[3])

    def test_steamvr_not_running(self):
        self.env.pop("VRSERVER")
        on, note = travel_mode.get(self.host)
        self.assertIsNone(on)
        self.assertIn("Start SteamVR", note)
        ok, text, got = travel_mode.put(self.host, True)
        self.assertFalse(ok)
        self.assertIsNone(got)
        self.assertIn("Start SteamVR", text)
        self.assertEqual(self.calls(), [])  # vrcmd wasn't run
        self.assertFalse(self.state.exists())

    def test_vrcmd_missing(self):
        travel_mode.VRCMD = str(self.bin / "gone" / "vrcmd")
        on, note = travel_mode.get(self.host)
        self.assertIsNone(on)
        self.assertIn("isn't at", note)
        ok, text, _ = travel_mode.put(self.host, True)
        self.assertFalse(ok)
        self.assertIn("isn't at", text)

    def test_steamvr_keeps_it_off(self):
        self.env["IGNORE_SET"] = "1"
        ok, text, got = travel_mode.put(self.host, True)
        self.assertFalse(ok)
        self.assertIs(got, False)
        self.assertEqual(text, "SteamVR kept travel mode off")

    def test_a_failed_set_says_what_vrcmd_said(self):
        def failing(*cmd, timeout=20):
            if "--set-settings-bool" in cmd:
                return subprocess.CompletedProcess(cmd, 1, "", "VR_Init failed: hmd not found\n")
            return self.host(*cmd, timeout=timeout)
        ok, text, got = travel_mode.put(failing, True)
        self.assertFalse(ok)
        self.assertIsNone(got)
        self.assertEqual(text, "vrcmd couldn't set travel mode: VR_Init failed: hmd not found")

    def test_a_missing_library_isnt_called_missing_vrcmd(self):
        # 127 from the loader (or any missing command) is vrcmd failing, not vrcmd absent.
        self.vrcmd.write_text("#!/bin/sh\nexit 127\n")
        on, note = travel_mode.get(self.host)
        self.assertIsNone(on)
        self.assertNotIn("isn't at", note)
        self.assertNotIn("Start SteamVR", note)

    def test_unreadable_answer(self):
        def broken(*cmd, timeout=20):
            return subprocess.CompletedProcess(cmd, 1, "", "vrcmd: no")
        self.assertEqual(travel_mode.get(broken), (None, f"vrcmd couldn't read {travel_mode.KEY}"))

    def test_the_switch_is_wired(self):
        here = Path(__file__).resolve().parents[1]
        qml = (here / "InputPages.qml").read_text()
        py = (here / "ft_input_settings.py").read_text()
        for name in ("travelMode", "travelKnown", "travelBusy", "travelNote", "refreshTravel", "setTravelMode"):
            self.assertIn(f"backend.{name}", qml)
            self.assertIn(f"def {name}(self", py)


if __name__ == "__main__":
    unittest.main()
