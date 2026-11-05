#!/usr/bin/python3
"""recorder/privacy.py: what identifies a Frame in the factory calibration, and the strip that
takes it out. Standard library only; made-up values, no real unit's."""
import copy
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", ".."))
from recorder import privacy  # noqa: E402


class KeyTest(unittest.TestCase):
    def test_identifying_keys(self):
        for k in ("serial", "Serial", "serialNumber", "device_serial_number", "uuid", "deviceUUID",
                  "guid", "mac", "macAddress", "id", "ids", "cameraId", "unit_id", "SN", "sn",
                  "identifier", "headsetSerialNo"):
            self.assertTrue(privacy.key_identifies(k), k)

    def test_keys_that_dont(self):
        # (words that contain the tokens aren't them: "width", "machine", "snap", "grid")
        for k in ("name", "width", "machine", "snap", "grid", "intrinsics", "distortion", "fx",
                  "slam_left", "rotation", "idle", "middle", "valid", 3):
            self.assertFalse(privacy.key_identifies(k), k)


class ValueTest(unittest.TestCase):
    def test_serial_like_strings(self):
        for v in ("4H2K19A7731", "unit 4H2K19A7731", "ABC12345678", "a1b2c3d4"):
            self.assertTrue(privacy.value_identifies(v), v)

    def test_values_that_dont(self):
        # sensor names are short; long words with few digits, numbers and non-strings aren't
        for v in ("og01a1b", "slam_left", "camera_front_left_2", "12345678", "abcdefgh1",
                  "", 12345678901, 1.5, None, ["4H2K19A7731"], {"x": 1}):
            self.assertFalse(privacy.value_identifies(v), v)


class StripTest(unittest.TestCase):
    CAL = {
        "serial": "4H2K19A7731",
        "version": 3,
        "cameras": [
            {"name": "slam_left", "serial": "X", "sensor": "og01a1b", "fx": 280.5,
             "label": "unit 4H2K19A7731", "extrinsics": {"rotation": [1, 0, 0], "uuid": "u"}},
            {"name": "slam_right", "fx": 281.0},
        ],
        "tags": ["left", "ABC12345678", "right"],
        "imu": {"macAddress": "aa:bb", "rate": 1000},
    }

    def test_strip(self):
        before = copy.deepcopy(self.CAL)
        clean, removed = privacy.strip_calibration(self.CAL)
        self.assertEqual(self.CAL, before)   # the input is left alone
        self.assertEqual(clean, {
            "version": 3,
            "cameras": [
                {"name": "slam_left", "sensor": "og01a1b", "fx": 280.5, "extrinsics": {"rotation": [1, 0, 0]}},
                {"name": "slam_right", "fx": 281.0},
            ],
            "tags": ["left", None, "right"],   # a list keeps its other items' positions
            "imu": {"rate": 1000},
        })
        self.assertEqual(removed, ["serial", "cameras[0].serial", "cameras[0].label",
                                   "cameras[0].extrinsics.uuid", "tags[1]", "imu.macAddress"])

    def test_stripping_twice_removes_nothing_more(self):
        # validate.py's check: what the recorder saved has nothing left to strip
        clean, _ = privacy.strip_calibration(self.CAL)
        again, removed = privacy.strip_calibration(clean)
        self.assertEqual(again, clean)
        self.assertEqual(removed, [])

    def test_not_a_dict(self):
        self.assertEqual(privacy.strip_calibration([1, "x"]), ([1, "x"], []))
        self.assertEqual(privacy.strip_calibration("4H2K19A7731"), ("4H2K19A7731", []))


if __name__ == "__main__":
    unittest.main()
