"""ft-npud's instrumentation: Pace (sleeps through most of a request's expected DSP time before framenpu's
completion polls) and Stamps (NPUD_PHASES / NPUD_STAMPS: where a request's time went, from the DSP's reference
clock)."""
import ctypes, os, struct, sys, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'spikes/int8'))
from run_int8_link import IG_MAGIC


class Pace:
    """framenpu polls a request's completion every 0.1 ms (~35 wakeups over a 4-crop request). This sleeps
    through most of the expected DSP time first, then leaves the same checks to framenpu's wait. The estimate
    is 80% of the fastest of the last 32 waits for that crop count, so a slower request (another DSP user)
    only polls for longer; only the completion wait after arm() is paced, never the acknowledge."""

    def __init__(self, t):
        self.wait0, self.n, self.hist = t.wait, 0, {}

    def arm(self, n):
        self.n = n

    def wait(self, predicate, timeout):
        n, self.n = self.n, 0
        if not n:
            return self.wait0(predicate, timeout)
        t0 = time.perf_counter()
        h = self.hist.setdefault(n, [])
        if h and not predicate():
            time.sleep(0.8 * min(h))
        ok = self.wait0(predicate, timeout)
        h.append(time.perf_counter() - t0)
        del h[:-32]
        return ok

class Stamps:
    """NPUD_PHASES only: wraps the transport's publish to read the reference clock just before the DSP is
    woken, and splits each request's wait with worker 0's start and end stamps: wake_ms (publish to the
    kernel's start), kernel_ms, notice_ms (the kernel's end to the host's completion check returning)."""

    def __init__(self, t, out):
        lib = ctypes.CDLL(str(Path(__file__).with_name('ticks.so')))   # ticks.c
        lib.ticks.restype = ctypes.c_uint64
        self.now, self.out, self.at, self.last, self.recs = lib.ticks, out, None, 0, {}
        self.recv, self.row = None, None   # NPUD_STAMPS: the request's arrival, and its DSP stamps until done()
        raw = os.environ.get('NPUD_STAMPS')
        # root writes it: never through a symlink
        self.raw = (os.fdopen(os.open(raw, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o644), 'w',
                              buffering=1) if raw else None)
        publish0 = t.publish

        def publish(**kw):
            self.at = self.now()
            return publish0(**kw)
        t.publish = publish

    def seen(self, res, kind):
        now, at, self.at = self.now(), self.at, None
        m = struct.unpack('<64I', res.output)
        if m[0] == IG_MAGIC and 0 < m[1] <= 48:   # int8_graph.c: [8 + i] = record i's reference ticks
            acc = self.recs.setdefault(kind, [0, [0.0] * m[1]])
            if len(acc[1]) == m[1]:
                acc[0] += 1
                for i in range(m[1]):
                    acc[1][i] += m[8 + i] / 19.2e3
        row = res.workers[0] if res.workers else ()
        if at is None or len(row) < 14:
            return
        start, end = row[10] | row[11] << 32, row[12] | row[13] << 32
        for k, v in (('wake_ms', start - at), ('kernel_ms', end - start), ('notice_ms', now - end)):
            self.out[k] = self.out.get(k, 0.0) + v / 19.2e3
        if self.raw is not None:
            self.row = (at, start, end, now)
        self.last = now

    def done(self):
        """NPUD_STAMPS=FILE: after the client's answer, one line a request, ms: pre (arrival to publish: crops
        in, claim, power), wake, kernel, notice, post (completion seen to the answer sent: checks, gather)."""
        if self.raw is None or self.row is None or self.recv is None:
            return
        at, start, end, seen = self.row
        t = self.now()
        self.raw.write('%.4f %.4f %.4f %.4f %.4f\n' % tuple(v / 19.2e3 for v in (
            at - self.recv, start - at, end - start, seen - end, t - seen)))
        self.recv = self.row = None
