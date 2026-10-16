#!/usr/bin/env python3
"""The eye recorder's frame writer (DESIGN.md: "Files"): every frame of both eye cameras, from
the frames ft-eyegrab shares (gaze/tracker/eyes_share.py), into a take's folder as zstd chunks.

  cam0/000000.zst, 000001.zst, ...   the right eye (camera 0): up to CHUNK_FRAMES frames each
  cam1/...                           the left eye (camera 1)
  index.tsv                          a header line, then one line per frame:
                                     camera, chunk, frame in the chunk, the tracker's slot, time
                                     (CLOCK_MONOTONIC_RAW seconds, the share's)

A chunk is the frames back to back, 512x400 bytes each, compressed with zstd level 1 (56 % of the
size at about 250 MB/s on one core, measured 2026-10-14; both cameras at 90 fps are 36.9 MB/s
raw). A chunk ends when it's full or the recording pauses, so a pause or a deleted range never
splits one. The index lines of a chunk are written once the chunk is on disk, so the index never
names a chunk that isn't there; they're in chunk order, not time order across the cameras.

One thread copies frames out of the share as they come (it holds only 8 per camera, 89 ms);
another compresses and writes, so a slow chunk never makes the copy fall behind. If the writer
falls MAX_QUEUED chunks behind, the next chunk is dropped and counted rather than held in memory.

Plain Python, standard library: zstd is Python 3.14's compression.zstd (the dev container's), or
zstandard, or the zstd command (SteamOS's host Python 3.12 has neither module).

From the command line, for testing (FT_EYES_CAMS points it at a replay, lab/ft-eyes-replay):
  python3 gaze/rec/eyerec.py DIR [SECONDS]
  python3 gaze/rec/eyerec.py --read DIR      the frames' count and times, decompressed
"""
import os
import queue
import shutil
import subprocess
import sys
import threading
import time
from collections import deque

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "tracker"))
import eyes_share  # noqa: E402  (gaze/tracker/eyes_share.py)

W, H = eyes_share.W, eyes_share.H
FRAME = W * H
CAMERAS = (0, 1)               # 0 the right eye, 1 the left (gaze/tracker/README: camera 0 is the steep one)
CHUNK_FRAMES = 90              # a second at 90 fps
LEVEL = 1
MAX_QUEUED = 6                 # chunks waiting for the writer (about 18 MB each) before one is dropped
MIN_FREE = 3e9                 # bytes: stop before the disk fills
RATE_WINDOW = 2.0              # s of frame times for each camera's rate
SHARE_WAIT = 10.0              # s to wait for the share at the start, or after it was replaced
POLL = 0.002
INDEX_HEADER = "# camera\tchunk\tframe\tslot\tt_raw\n"


def _compressor():
    """(compress(bytes) -> bytes, decompress(bytes) -> bytes, name)."""
    try:
        from compression import zstd   # Python 3.14
        return (lambda b: zstd.compress(b, level=LEVEL)), zstd.decompress, "compression.zstd"
    except ImportError:
        pass
    try:
        import zstandard
        c, d = zstandard.ZstdCompressor(level=LEVEL), zstandard.ZstdDecompressor()
        return c.compress, (lambda b: d.decompress(b, max_output_size=CHUNK_FRAMES * FRAME)), "zstandard"
    except ImportError:
        pass
    exe = shutil.which("zstd") or ("/usr/bin/zstd" if os.path.exists("/usr/bin/zstd") else None)
    if not exe:
        return None, None, None

    def run(args, data):
        return subprocess.run([exe, *args], input=data, capture_output=True, check=True).stdout
    return (lambda b: run(["-q", "-%d" % LEVEL, "-c"], b)), (lambda b: run(["-q", "-d", "-c"], b)), exe


compress, decompress, ZSTD = _compressor()


class Writer:
    """Records the eye cameras into take_dir while recording is on.

    start() starts the threads (not recording yet); record(True) and record(False) start and
    pause; stop() ends the open chunks, waits for the writer and returns stats(). error is set
    (and recording stops) when the disk gets full or the share is gone for good."""

    def __init__(self, take_dir, share_path=None, want_path=None, log=None, min_free=MIN_FREE,
                 chunk_frames=CHUNK_FRAMES):
        if compress is None:
            raise RuntimeError("no zstd: neither compression.zstd, zstandard nor the zstd command")
        self.dir, self.share_path, self.want_path = take_dir, share_path, want_path
        self.log = log or (lambda line: None)
        self.min_free, self.chunk_frames = min_free, chunk_frames
        self.error = None
        self._on = False
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._queue = queue.Queue()
        self._open = {c: [] for c in CAMERAS}      # the chunk being filled: [(t, slot, bytes)]
        self._next = {c: 0 for c in CAMERAS}       # the next chunk's number
        self._frames = {c: 0 for c in CAMERAS}     # frames written (in chunks on disk)
        self._dropped = {c: 0 for c in CAMERAS}    # frames lost: overwritten in the share, or a dropped chunk
        self._times = {c: deque() for c in CAMERAS}
        self._last = {c: None for c in CAMERAS}
        self._bytes = 0
        self._busy = 0.0                           # s spent compressing and writing
        for c in CAMERAS:
            os.makedirs(os.path.join(take_dir, "cam%d" % c), exist_ok=True)
        self._index = open(os.path.join(take_dir, "index.tsv"), "a")
        if self._index.tell() == 0:
            self._index.write(INDEX_HEADER)
        # Numbering goes on after any chunks already there (a take resumed after a pause in another run).
        for c in CAMERAS:
            names = [n for n in os.listdir(os.path.join(take_dir, "cam%d" % c)) if n.endswith(".zst")]
            self._next[c] = max((int(n[:-4]) + 1 for n in names if n[:-4].isdigit()), default=0)
        self._reader = threading.Thread(target=self._read, name="eyerec-read", daemon=True)
        self._writer = threading.Thread(target=self._write, name="eyerec-write", daemon=True)

    def start(self):
        self._reader.start()
        self._writer.start()
        return self

    def record(self, on):
        with self._lock:
            if self._on and not on:
                for c in CAMERAS:
                    self._end_chunk(c)
            self._on = bool(on) and self.error is None

    @property
    def recording(self):
        return self._on

    def stop(self):
        self.record(False)
        self._stop.set()
        self._reader.join(5)
        self._queue.put(None)
        self._writer.join(60)
        self._index.close()
        return self.stats()

    def stats(self):
        """{"frames": [right, left], "dropped": [...], "fps": [...], "bytes": on disk, "busy": s,
        "queued": chunks waiting, "last_t": [the newest frame's time or None], "error"}"""
        with self._lock:
            fps = []
            for c in CAMERAS:
                ts = self._times[c]
                fps.append(round((len(ts) - 1) / (ts[-1] - ts[0]), 1) if len(ts) > 1 and ts[-1] > ts[0] else 0.0)
            return {"frames": [self._frames[c] for c in CAMERAS], "dropped": [self._dropped[c] for c in CAMERAS],
                    "fps": fps, "bytes": self._bytes, "busy": round(self._busy, 3), "queued": self._queue.qsize(),
                    "last_t": [self._last[c] for c in CAMERAS], "error": self.error}

    # -- the copy

    def _read(self):
        share = eyes_share.open_share(SHARE_WAIT, self.share_path, self.want_path)
        if share is None:
            self._fail("no eye camera frames (%s): is frametop-eyegrab.service running?" % (self.share_path or eyes_share.CAMS))
            return
        seen = {c: share.count(c) for c in CAMERAS}
        touched = checked = time.monotonic()
        while not self._stop.is_set():
            got = False
            for c in CAMERAS:
                n = share.count(c)
                if n <= seen[c]:   # nothing new (and never step back: that would copy frames twice)
                    continue
                if n - seen[c] > share.slots:   # fell behind: those are gone
                    with self._lock:
                        if self._on:
                            self._dropped[c] += n - seen[c] - share.slots
                    seen[c] = n - share.slots
                for i in range(seen[c], n):
                    f = share.frame(c, i)
                    got = True
                    with self._lock:
                        if f is None:
                            if self._on:
                                self._dropped[c] += 1
                            continue
                        self._seen(c, f[0])
                        if self._on:
                            self._open[c].append(f)
                            if len(self._open[c]) >= self.chunk_frames:
                                self._end_chunk(c)
                seen[c] = n
            now = time.monotonic()
            if now - touched >= 1.0:
                touched = now
                eyes_share.touch_want(self.want_path)
            if now - checked >= 2.0:
                checked = now
                if self._on and shutil.disk_usage(self.dir).free < self.min_free:
                    self._fail("under %.1f GB free on the disk" % (self.min_free / 1e9))
                if share.replaced():   # ft-eyegrab restarted (SteamVR did): follow it
                    self.log("eye frames: the share was replaced; following it")
                    share.close()
                    share = eyes_share.open_share(SHARE_WAIT, self.share_path, self.want_path)
                    if share is None:
                        self._fail("the eye camera frames stopped (frametop-eyegrab.service)")
                        return
                    seen = {c: share.count(c) for c in CAMERAS}
            if not got:
                time.sleep(POLL)
        share.close()

    def _seen(self, c, t):
        ts = self._times[c]
        ts.append(t)
        while ts and ts[0] < t - RATE_WINDOW:
            ts.popleft()
        self._last[c] = t

    def _end_chunk(self, c):
        """Hand the open chunk to the writer (with the lock held)."""
        frames = self._open[c]
        if not frames:
            return
        self._open[c] = []
        if self._queue.qsize() >= MAX_QUEUED:
            self._dropped[c] += len(frames)
            self.log("eye frames: the writer is behind; dropped %d frames of camera %d" % (len(frames), c))
            return
        self._queue.put((c, self._next[c], frames))
        self._next[c] += 1

    def _fail(self, why):
        with self._lock:
            if self.error is None:
                self.error = why
                self.log("eye frames: " + why)
            if self._on:
                for c in CAMERAS:
                    self._end_chunk(c)
            self._on = False

    # -- the writer

    def _write(self):
        while True:
            item = self._queue.get()
            if item is None:
                return
            c, chunk, frames = item
            t0 = time.perf_counter()
            path = os.path.join(self.dir, "cam%d" % c, "%06d.zst" % chunk)
            try:
                data = compress(b"".join(f[2] for f in frames))
                with open(path + ".tmp", "wb") as f:
                    f.write(data)
                os.replace(path + ".tmp", path)
            except (OSError, subprocess.CalledProcessError) as e:
                self._fail("can't write %s: %s" % (path, e))
                with self._lock:
                    self._dropped[c] += len(frames)
                continue
            self._index.write("".join("%d\t%d\t%d\t%d\t%.6f\n" % (c, chunk, i, f[1], f[0]) for i, f in enumerate(frames)))
            self._index.flush()
            with self._lock:
                self._frames[c] += len(frames)
                self._bytes += len(data)
                self._busy += time.perf_counter() - t0


def read_index(take_dir):
    """index.tsv as [(camera, chunk, frame, slot, t), ...] in its order."""
    out = []
    try:
        with open(os.path.join(take_dir, "index.tsv")) as f:
            for line in f:
                if line.startswith("#") or not line.strip():
                    continue
                c, chunk, i, slot, t = line.split("\t")
                out.append((int(c), int(chunk), int(i), int(slot), float(t)))
    except FileNotFoundError:
        pass
    return out


def read_chunk(take_dir, cam, chunk):
    """The frames of a chunk, as a list of 512x400 byte strings."""
    with open(os.path.join(take_dir, "cam%d" % cam, "%06d.zst" % chunk), "rb") as f:
        raw = decompress(f.read())
    if len(raw) % FRAME:
        raise ValueError("cam%d/%06d.zst: %d bytes isn't whole frames" % (cam, chunk, len(raw)))
    return [raw[i:i + FRAME] for i in range(0, len(raw), FRAME)]


def iter_frames(take_dir):
    """Every frame in index order: (camera, t, slot, bytes)."""
    cache = (None, None)
    for c, chunk, i, slot, t in read_index(take_dir):
        if cache[0] != (c, chunk):
            cache = ((c, chunk), read_chunk(take_dir, c, chunk))
        yield c, t, slot, cache[1][i]


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        sys.exit(__doc__)
    if argv[0] == "--read":
        n, ts = [0, 0], [[], []]
        for c, t, _slot, _data in iter_frames(argv[1]):
            n[c] += 1
            ts[c].append(t)
        for c in CAMERAS:
            span = max(ts[c]) - min(ts[c]) if len(ts[c]) > 1 else 0.0
            print("camera %d: %d frames over %.2f s" % (c, n[c], span))
        return
    secs = float(argv[1]) if len(argv) > 1 else 10.0
    w = Writer(argv[0], log=lambda line: print(line, file=sys.stderr)).start()
    w.record(True)
    end = time.monotonic() + secs
    try:
        while time.monotonic() < end and w.error is None:
            time.sleep(1)
            print(w.stats(), file=sys.stderr)
    except KeyboardInterrupt:
        pass
    print(w.stop())


if __name__ == "__main__":
    main(sys.argv[1:])
