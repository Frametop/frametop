"""The shared eye-camera frames ft-eyegrab keeps in /dev/shm/frametop-eyes-cams, read without
numpy: lab/ft-eyes-record and the eye recorder's frame writer (gaze/rec/eyerec.py) use it.

Layout (ft-eyegrab.c, share_head_t and share_entry_t): a 64-byte header (magic "FEC1", version,
width, height, slots per camera, entry size, then each camera's frame count as u64 at 24 + 8 *
camera, and the tracker's pid at 40), then SLOTS entries per camera. Frame n of camera c is in
entry c * slots + n % slots: a 64-byte block (u64 seq, odd while it's written; f64 time,
CLOCK_MONOTONIC_RAW seconds; u64 n; u32 camera; u32 the tracker's slot), then the image.
"""
import mmap
import os
import struct
import time

CAMS = os.environ.get("FT_EYES_CAMS", "/dev/shm/frametop-eyes-cams")  # overrides for replays
WANT = "/dev/shm/frametop-eyes-want"
W, H = 512, 400
MAGIC = 0x31434546


class Share:
    """The shared frames, mapped read-only."""

    def __init__(self, path=None):
        self.path = path or CAMS
        fd = os.open(self.path, os.O_RDONLY)
        try:
            self.ino = os.fstat(fd).st_ino
            self.mm = mmap.mmap(fd, 0, prot=mmap.PROT_READ)
        finally:
            os.close(fd)
        magic, version, w, h, self.slots, self.esize = struct.unpack_from("<6I", self.mm, 0)
        if magic != MAGIC or version != 1 or (w, h) != (W, H):
            raise RuntimeError(f"{self.path}: unexpected header")

    def replaced(self):
        """ft-eyegrab restarted: it makes a new file, and this one is stale."""
        try:
            return os.stat(self.path).st_ino != self.ino
        except OSError:
            return True

    def count(self, cam):
        return struct.unpack_from("<Q", self.mm, 24 + 8 * cam)[0]

    def frame(self, cam, n):
        """(time, slot, bytes) of frame n of a camera, or None if it was overwritten."""
        off = 64 + (cam * self.slots + n % self.slots) * self.esize
        for _ in range(3):
            seq = struct.unpack_from("<Q", self.mm, off)[0]
            if seq & 1:
                continue
            data = self.mm[off + 64:off + 64 + W * H]
            t, got, _cam, slot = struct.unpack_from("<dQII", self.mm, off + 8)
            if struct.unpack_from("<Q", self.mm, off)[0] == seq and got == n:
                return t, slot, data
        return None

    def close(self):
        self.mm.close()


def touch_want(path=None):
    """Tell ft-eyegrab someone wants frames (it idles otherwise). Not for replays."""
    if path is None and "FT_EYES_CAMS" in os.environ:
        return
    try:
        fd = os.open(path or WANT, os.O_WRONLY | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
        os.utime(fd)
        os.close(fd)
    except OSError:
        pass


def open_share(timeout, path=None, want=None):
    """The share, waiting up to timeout seconds for it (touching the want file meanwhile), or None."""
    deadline = time.monotonic() + timeout
    while True:
        touch_want(want)
        try:
            return Share(path)
        except (OSError, ValueError, RuntimeError):
            if time.monotonic() >= deadline:
                return None
            time.sleep(0.5)
