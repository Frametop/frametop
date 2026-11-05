"""Read frames from ft-camd's shared-memory ring (layout: camd/fhring.h, in recorder/fhformat.py)."""
import mmap
import os
import struct
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from recorder import fhformat  # noqa: E402  (the ring's layout, checked against fhring.h)

RING_FILE = fhformat.ring_path()
HDR, CAM, SLOT = fhformat.RING_HDR, fhformat.RING_CAM, fhformat.RING_SLOT


class Frame:
    __slots__ = ('cam', 'frame', 'capture_ns', 'dqbuf_ns', 'publish_ns', 'v4l2_seq', 'mean', 'image')

    def __init__(self, cam, fields, image):
        self.cam = cam
        (_, self.frame, self.capture_ns, self.dqbuf_ns, self.publish_ns, self.v4l2_seq, self.mean) = fields
        self.image = image


class RingCamera:
    def __init__(self, index, fields):
        (sensor, name, self.node, self.format, self.width, self.height, self.stride, self.nslots,
         self.slot_offset, self.slot_bytes, _latest, _pub, _drop, self.flags, _dark_mean) = fields
        self.index = index
        self.sensor = sensor.split(b'\0', 1)[0].decode()
        self.name = name.split(b'\0', 1)[0].decode()
        self.dark = bool(self.flags & fhformat.CAM_DARK)     # the near-black exposures (--with-dark)
        self.color = bool(self.flags & fhformat.CAM_COLOR)   # a colour camera's luma (--with-color)
        self.latest_off = fhformat.ring_cam_offset(index) + fhformat.RING_CAM_LATEST_OFF
        self.dark_mean_off = fhformat.ring_cam_offset(index) + fhformat.RING_CAM_DARK_MEAN_OFF

    def __repr__(self):
        return 'RingCamera(video%d %s %dx%d)' % (self.node, self.sensor, self.width, self.height)


class Ring:
    def __init__(self, path=RING_FILE):
        fd = os.open(path, os.O_RDONLY)
        try:
            self.map = mmap.mmap(fd, 0, mmap.MAP_SHARED, mmap.PROT_READ)
        finally:
            os.close(fd)
        magic, version, hdr_bytes, ncams, _, file_bytes, self.writer_pid, _ = HDR.unpack_from(self.map, 0)
        if magic != fhformat.RING_MAGIC or version != fhformat.RING_VERSION:
            raise RuntimeError('%s is not an ft-camd ring (magic %r version %d)' % (path, magic, version))
        self.cams = [RingCamera(i, CAM.unpack_from(self.map, fhformat.ring_cam_offset(i))) for i in range(ncams)]

    def heartbeat_ns(self):
        return struct.unpack_from('<Q', self.map, fhformat.RING_HEARTBEAT_OFF)[0]

    def alive(self, max_age=1.0):
        hb = self.heartbeat_ns()
        return hb != 0 and (time.clock_gettime_ns(time.CLOCK_MONOTONIC) - hb) / 1e9 < max_age

    def latest(self, cam):
        return struct.unpack_from('<Q', self.map, cam.latest_off)[0]

    def dark_mean(self, cam):
        """A mono camera's mean luma in its latest near-black frame (it follows the room's IR
        light); 0 before the first."""
        return struct.unpack_from('<f', self.map, cam.dark_mean_off)[0]

    def read(self, cam, n=None):
        """Copy frame n (default: the newest) of a camera, or None if it's gone or being written."""
        for _ in range(3):
            if n is None or n == 0:
                n = self.latest(cam)
            if n == 0:
                return None
            off = cam.slot_offset + (n % cam.nslots) * cam.slot_bytes
            fields = SLOT.unpack_from(self.map, off)
            if fields[0] != 2 * n + 2:
                return None
            start = off + SLOT.size
            image = np.frombuffer(self.map, np.uint8, cam.stride * cam.height, start).reshape(cam.height, cam.stride)
            image = image[:, :cam.width].copy()
            if struct.unpack_from('<Q', self.map, off)[0] == fields[0]:
                return Frame(cam, fields, image)
            n = None                                      # overwritten while copying: take the newest
        return None
