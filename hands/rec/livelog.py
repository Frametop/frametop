#!/usr/bin/env python3
"""The live tracker's output during a debug session (DESIGN.md, "Debug sessions"): each new
publish of Frametop's hands and gestures files (include/fh_hands.h, fh_gestures.h), copied whole
under their sequence lock into the take's live.bin. Replaying the take's sets through a newer
ft-hands and comparing with live.bin shows what changed for that person.

live.bin: a 16-byte header, then records.
  header: magic "FHLIVE01", u32 version (1), u32 0
  record: u8 source (0 hands, 1 gestures), 3 pad, u32 length, u64 read_ns, u64 seq, then the
          file's bytes (length). read_ns: CLOCK_MONOTONIC when copied; the files' own
          capture_ns and publish_ns are inside the copy.

Standard library only.
"""
import os
import struct
import threading

from recorder.common import mono_ns

MAGIC = b"FHLIVE01"
VERSION = 1
FILE_HDR = struct.Struct("<8sII")      # 16 bytes
REC_HDR = struct.Struct("<B3xIQQ")     # 24 bytes
SOURCES = (("hands", b"FHHANDS1"), ("gestures", b"FHGEST01"))
SEQ_AT = 16                            # u64 seq after magic, version and size in both files
POLL_S = 0.004                         # the tracker publishes about 30 times a second
MAX_BYTES = 1 << 16                    # a file claiming more than this is not one of ours


def snapshot(fd, magic):
    """(seq, bytes) of a consistent copy, or None (no file yet, being written, or not ours)."""
    for _ in range(4):
        head = os.pread(fd, 24, 0)
        if len(head) < 24 or head[:8] != magic:
            return None
        _, _, size, seq = struct.unpack_from("<8sIIQ", head, 0)
        if seq % 2:
            continue
        if not 24 <= size <= MAX_BYTES:
            return None
        data = os.pread(fd, size, 0)
        if len(data) == size and os.pread(fd, 8, SEQ_AT) == head[SEQ_AT:24]:
            return seq, data
    return None


class LiveLog:
    """A thread copying run_dir()'s hands and gestures files into path as they change. stop()
    ends it and returns {"hands": n, "gestures": n} records written."""

    def __init__(self, run_dir, path, poll_s=POLL_S):
        self.run_dir, self.path, self.poll_s = run_dir, path, poll_s
        self.counts = {name: 0 for name, _ in SOURCES}
        self._stop = threading.Event()
        self._out = open(path, "wb")
        self._out.write(FILE_HDR.pack(MAGIC, VERSION, 0))
        self._thread = threading.Thread(target=self._run, name="handrec-livelog", daemon=True)

    def start(self):
        self._thread.start()
        return self

    def _run(self):
        fds = [None] * len(SOURCES)
        last = [None] * len(SOURCES)
        try:
            while not self._stop.is_set():
                for k, (name, magic) in enumerate(SOURCES):
                    if fds[k] is None:
                        try:
                            fds[k] = os.open(os.path.join(self.run_dir, name), os.O_RDONLY)
                        except OSError:
                            continue
                    try:
                        snap = snapshot(fds[k], magic)
                    except OSError:
                        snap = None
                    if snap is None or snap[0] == last[k]:
                        continue
                    last[k] = snap[0]
                    self._out.write(REC_HDR.pack(k, len(snap[1]), mono_ns(), snap[0]))
                    self._out.write(snap[1])
                    self.counts[name] += 1
                self._stop.wait(self.poll_s)
        finally:
            for fd in fds:
                if fd is not None:
                    os.close(fd)

    def stop(self):
        self._stop.set()
        if self._thread.is_alive():
            self._thread.join(5)
        if not self._out.closed:
            self._out.close()
        return dict(self.counts)


def read(path):
    """The records of a live.bin: [(source name, read_ns, seq, bytes)]. Stops at a cut-off end."""
    out = []
    with open(path, "rb") as f:
        magic, version, _ = FILE_HDR.unpack(f.read(FILE_HDR.size))
        if magic != MAGIC or version != VERSION:
            raise ValueError("%s: not a live.bin" % path)
        while True:
            head = f.read(REC_HDR.size)
            if len(head) < REC_HDR.size:
                return out
            src, n, read_ns, seq = REC_HDR.unpack(head)
            data = f.read(n)
            if len(data) < n or src >= len(SOURCES):
                return out
            out.append((SOURCES[src][0], read_ns, seq, data))
