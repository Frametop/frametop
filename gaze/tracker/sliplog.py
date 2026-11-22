"""sliplog: ft-eyes' slip log, slip.jsonl in its state folder, for tuning the slip filter
(slipfilter.py) and seeing what moved it. A JSON line per row:

  {"t", "time", "eyes": {"0"|"1": {...}}}   every LOG_EVERY s while frames come: per eye its
                                             newest find (pupil, size "a", glint pair "mid"),
                                             its last glint estimate, the filter's state and
                                             variances (feature units), slip_deg and its gaze
  {"t", "time", "event": ..., ...}           a reading or a change: "click", "look", "glint",
                                             "reseat", "adjusting", "calibration"

Derived numbers only (no images). Past MAX_BYTES the file moves to slip.old.jsonl (the one
before is replaced), so the two stay under 2 x MAX_BYTES.
"""
import json
import os
import time

MAX_BYTES = 20_000_000
FLUSH_EVERY = 2.0      # s: rows reach the file at least this often (ft-eyes is stopped with os._exit)


class SlipLog:
    def __init__(self, path, max_bytes=MAX_BYTES):
        self.path, self.max_bytes = path, max_bytes
        self.old = path.with_name(path.stem + ".old" + path.suffix)
        self.f = None
        self.size = 0
        self.flushed = time.monotonic()

    def write(self, row):
        try:
            if self.f is None:
                self.f = open(self.path, "a", buffering=1 << 16)
                self.size = self.f.tell()
            line = json.dumps({"time": round(time.time(), 3), **row}) + "\n"
            self.f.write(line)
            self.size += len(line)
            if time.monotonic() - self.flushed >= FLUSH_EVERY:
                self.flush()
            if self.size > self.max_bytes:
                self.f.close()
                self.f = None
                os.replace(self.path, self.old)
        except OSError:
            self.f = None   # a full disk mustn't stop the tracker: try again next row

    def event(self, t, kind, **fields):
        self.write({"t": None if t is None else round(t, 4), "event": kind, **fields})

    def flush(self):
        self.flushed = time.monotonic()
        f = self.f   # (ft-eyes' stop flushes from another thread, maybe while write() moves it aside)
        if f is not None:
            try:
                f.flush()
            except (OSError, ValueError):
                pass
