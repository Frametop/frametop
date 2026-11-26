"""ft-npud's requests: the self-test through the serving path, then one client's requests at a time (transport.py)
until SIGTERM or SIGINT.

Signals stop the daemon at once only while it waits in server.wait() (Stop raised from the handler); anywhere
else, in a DSP job or the startup and teardown around them, the handler only sets a flag, which check_stop()
reads between jobs. A DSP job is never cut short, so a signal can't poison the session."""
import signal, time
import numpy as np

import gather as G
import run_int8_link as RL     # on sys.path through gather
import transport as T


class Stop(Exception):
    pass


class Service:
    """graphs: key -> Served; crops_at: the DSP-side crop slots; outs_at: key -> the client-side output offset;
    header: (features, graph id, detector scales); frame: a dsp.Frames; log: a line to stderr"""

    def __init__(self, server, memory, graphs, crops_at, outs_at, selftest_crops, header, frame, log):
        self.server, self.graphs, self.crops_at, self.outs_at = server, graphs, crops_at, outs_at
        self.selftest_crops, (self.features, self.gid, self.det_scales) = selftest_crops, header
        self.frame, self.phases, self.log = frame, frame.phases, log
        self.shm = memory.map
        # views over the daemon's own mapping (never closed before exit): the crops go to the DSP in one copy and
        # the outputs are gathered straight into their slots
        self.shm_mv, self.shm_np = memoryview(self.shm), np.frombuffer(self.shm, np.uint8)
        self.paired = tuple(k for k, g in graphs.items() if g.plan.two_view)   # graphs that take pair bits
        self.stats = {'frames': 0, 'errors': 0}
        self.idle = self.stopping = False

    # signals

    def install_signals(self):
        signal.signal(signal.SIGTERM, self._on_signal)
        signal.signal(signal.SIGINT, self._on_signal)

    def _on_signal(self, *_):
        self.stopping = True
        if self.idle:
            raise Stop()

    def check_stop(self):
        if self.stopping:
            raise Stop()

    # the DSP side

    def selftest(self, lk, mem):
        """every graph: 1 crop checked tensor by tensor, then 4, 3, 2 and 1 crops through the serving path (a two-view
        graph's in every pair variant)"""
        crops_at, frame = self.crops_at, self.frame
        for key, g in self.graphs.items():
            mem[crops_at:crops_at + T.CROP] = self.selftest_crops[0].tobytes()
            data, where, ddr, _ = g.observe
            frame(lk, data)
            for nm, (off, size) in ddr.items():
                if bytes(mem[off:off + size]) != RL.expected(g.plan, g.ref, nm):
                    raise SystemExit('self-test: %s differs from int8_ref' % nm)
            # 1: its own program (observe is records(1, True), a different layout)
            for n, pairs in sorted(g.data, reverse=True):
                mem[crops_at:crops_at + n * T.CROP] = self.selftest_crops[:n].tobytes()
                frame(lk, g.data[n, pairs], crops=n)
                got = np.zeros(n * g.slot, np.uint8)
                G.publish_parts(mem, g.gather[n], got, g.shapes)
                if not np.array_equal(got, g.want[n, pairs]):
                    raise SystemExit('self-test: a %d-crop request (pairs %d) differs from int8_ref' % (n, pairs))

    def _header(self, last, wrap, daemon_ms):
        frames = self.stats['frames']
        T.HDR.pack_into(self.shm, 0, T.MAGIC, T.VERSION, last, frames, self.features, frames, self.stats['errors'],
                        self.gid, wrap, daemon_ms, *self.det_scales, 0, 0, 0, 0)

    def serve(self, lk, mem):
        """answers requests until Stop"""
        server, stats, phases, crops_at = self.server, self.stats, self.phases, self.crops_at
        self._header(0, 0, 0)
        frames_at_join = 0
        server.listen()
        while True:
            self.idle = True     # first: a signal from here on raises at once, so none is missed before wait()
            try:
                self.check_stop()
                ev, b = server.wait()
            finally:
                self.idle = False
            if ev == 'joined':
                self.log('client connected')
                frames_at_join = stats['frames']
                continue
            if ev == 'left':
                self._client_left(lk, stats['frames'] - frames_at_join)
                continue
            req = T.parse(b, self.graphs, self.paired)
            if req is None:
                stats['errors'] += 1
                if not server.reply(T.BAD):
                    self._client_left(lk, stats['frames'] - frames_at_join)
                continue
            key, n, pairs = req
            t0 = time.perf_counter()
            if lk.stamp is not None:
                lk.stamp.recv = lk.stamp.now()
            g, at = self.graphs[key], self.outs_at[key]
            mem[crops_at:crops_at + n * T.CROP] = self.shm_mv[T.CROPS:T.CROPS + n * T.CROP]
            answer = {}

            def ready(wrap):
                G.publish_parts(mem, g.gather[n], self.shm_np[at:at + n * g.slot], g.shapes, phases)
                stats['frames'] += 1
                self._header(b[0], wrap, (time.perf_counter() - t0) * 1e3)
                answer['sent'] = server.reply(T.OK)
                if lk.stamp is not None:
                    lk.stamp.done()
            self.frame(lk, g.data[n, pairs], key, ready, crops=n)
            if 'sent' not in answer:
                raise self.frame.RecoveryRequired('a request finished without its early answer')
            if not answer['sent']:    # dropped: it stopped reading, or left mid-request
                self._client_left(lk, stats['frames'] - frames_at_join)

    def _client_left(self, lk, nframes):
        self.log('client left after %d frames' % nframes)
        phases = self.phases
        if phases:
            nf = max(nframes, 1)
            self.log('phases ms a request: ' + ', '.join('%s %.3f' % (k, v / nf) for k, v in phases.items()))
            phases.clear()
            if lk.stamp is not None:
                for kind, (cnt, ms) in sorted(lk.stamp.recs.items()):
                    self.log('records ms, request kind %d (%d requests): %s (sum %.3f)' % (
                        kind, cnt, ' '.join('%.3f' % (v / cnt) for v in ms), sum(ms) / cnt))
                lk.stamp.recs.clear()
