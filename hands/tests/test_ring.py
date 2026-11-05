#!/usr/bin/python3
"""hands/tools/ring.py on a made-up ft-camd ring (camd/fhring.h, written by
recorder/fhformat.py's layout): a frame, the cameras' flags and dark_mean. Needs numpy."""
import os
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "tools"))
sys.path.insert(0, os.path.dirname(os.path.dirname(HERE)))
from recorder import fhformat  # noqa: E402

try:
    import numpy  # noqa: F401
    import ring  # noqa: E402
except ImportError:
    ring = None

W, H, SLOTS = 8, 4, 2


def make_ring(path):
    """Two cameras: a mono one with frame 3 published and a dark mean, and its dark twin."""
    slot_bytes = fhformat.RING_SLOT.size + W * H
    slot0 = fhformat.ring_cam_offset(fhformat.RING_MAX_CAMS)
    size = slot0 + 2 * SLOTS * slot_bytes
    data = bytearray(size)
    fhformat.RING_HDR.pack_into(data, 0, fhformat.RING_MAGIC, fhformat.RING_VERSION, 0, 2, 0, size, 4242, 0)
    for i, (name, flags, dark) in enumerate([(b"slam_left", 0, 12.5), (b"slam_left_dk", fhformat.CAM_DARK, 0.0)]):
        fhformat.RING_CAM.pack_into(data, fhformat.ring_cam_offset(i), b"og01a1b", name, 9 + i, fhformat.FMT_GREY8,
                                    W, H, W, SLOTS, slot0 + i * SLOTS * slot_bytes, slot_bytes,
                                    3 if i == 0 else 0, 3, 0, flags, dark)
    off = slot0 + (3 % SLOTS) * slot_bytes
    fhformat.RING_SLOT.pack_into(data, off, 2 * 3 + 2, 3, 111, 222, 333, 7, 40.0)
    data[off + fhformat.RING_SLOT.size: off + slot_bytes] = bytes(range(W * H))
    struct.pack_into("<Q", data, fhformat.RING_HEARTBEAT_OFF, 1)
    with open(path, "wb") as f:
        f.write(bytes(data))


@unittest.skipUnless(ring, "no numpy")
class RingTest(unittest.TestCase):
    def setUp(self):
        fd, self.path = tempfile.mkstemp(prefix="fhring-test-")
        os.close(fd)
        make_ring(self.path)
        self.ring = ring.Ring(self.path)

    def tearDown(self):
        self.ring.map.close()
        with open(self.path, "wb"):   # (emptied; the temp folder is the system's)
            pass

    def test_cameras_flags_and_dark_mean(self):
        mono, dark = self.ring.cams
        self.assertEqual((mono.name, mono.flags, mono.dark, mono.color), ("slam_left", 0, False, False))
        self.assertEqual((dark.name, dark.dark, dark.color), ("slam_left_dk", True, False))
        self.assertEqual(self.ring.dark_mean(mono), 12.5)
        self.assertEqual(self.ring.dark_mean(dark), 0.0)

    def test_frame(self):
        mono = self.ring.cams[0]
        self.assertEqual(self.ring.latest(mono), 3)
        f = self.ring.read(mono)
        self.assertEqual((f.frame, f.capture_ns, f.dqbuf_ns, f.v4l2_seq, f.mean), (3, 111, 222, 7, 40.0))
        self.assertEqual(f.image.shape, (H, W))
        self.assertEqual(int(f.image[1, 0]), W)
        self.assertIsNone(self.ring.read(self.ring.cams[1]))   # nothing published


if __name__ == "__main__":
    unittest.main()
