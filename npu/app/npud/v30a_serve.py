"""ft-npud's v30a mode: v30a (the live CPU landmark model, DeeJanuz/frametop-hands-models@03d2018) in FP16 on the
HMX image (spikes/hmx: build_v30a_link.py, v30a_layout.py), for the same client as the INT8 student. ft_npud.py
runs this when --graph is a v30a graph file (pack below); everything else on its command line means the same.

  python v30a_serve.py pack MODEL_DIR OUT.npz     the graph file: frame-hands' hand.ncnn.param and .bin in an npz

The device runs v30a to its 672 x 7 x 7 map; this host does the stem columns (v30a_io.Stem) and GAP + the four heads
(v30a_io.Final), so the client reads finished outputs: heads file version 3 (heads_file.pack_float), FLOAT_OUTS f32
a crop at transport.OUTS. No detector: the INT8 detector runs on the INT8 image (--det is not served; the client
keeps its CPU palm search). Requests of 1..4 crops. The kernel replays at most 3 a job, so 4 run as 3 + 1,
unless the link image was built for 4 (its record says so: max_crops), and then 4 run as one job. The stem
gather and the final map's sums run in C (v30a_c.py).

Startup and teardown are v30a_device.py's protections with ft_npud.py's serving ones: the lease, the refusal record,
the arena saved, audited direct dispatch, the window zeroed, the image sealed, HMX power held, and the self-test
before the socket opens: 4 crops alone (no stray padding halves, no FP16 overflow lanes, crop 0's outputs within
SELFTEST of the host FP32 graph), then 4-, 3-, 2- and 1-crop requests through the serving path, which must
reproduce the single-crop outputs bit for bit. On SIGTERM/SIGINT or any error: power released (abandoned if the
session is unsafe), image retired, window and reservation restored, the arena compared with its start, lease
released. Never a retry or a recovery.
"""
import hashlib, os, signal, struct, sys, time
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
HMX = HERE.parent / 'spikes/hmx'
sys.path[:0] = [str(HERE), str(HMX)]
import heads_file                        # noqa: E402
import transport as T                    # noqa: E402
import v30a_io as IO                     # noqa: E402

LINK = HMX / 'link'                      # build_v30a_link.py's output, installed with the app (deploy/files.txt)
LINK_FILES = ('link.json', 'teacher_link_loader.fjob', 'teacher_link.bin')
CHUNK = 3                                # the kernel's replay limit (framenpu _mercury_common.MAX_CROPS)
CHUNKS = (3, 4)                          # what a link record may name instead (RS_MAX_CROPS of its kernel)
# the self-test's bounds for crop 0 against the host FP32 graph (FP16 on the device: 0.17 px max on real crops)
SELFTEST = {'out0_px': 1.0, 'prob': 0.01, 'world': 0.01}


def pack(model_dir, out):
    """the graph file: the two ncnn files as u8 arrays (v30a_io.is_graph knows it by 'v30a.param')"""
    d = Path(model_dir)
    arrays = {k: np.frombuffer((d / n).read_bytes(), np.uint8) for k, n in
              (('v30a.param', 'hand.ncnn.param'), ('v30a.bin', 'hand.ncnn.bin'))}
    np.savez(out, **arrays)


def load_model(path):
    import v30a_graph as G
    with np.load(path) as z:
        return G.load_bytes(z['v30a.param'].tobytes(), z['v30a.bin'].tobytes())


class Stop(Exception):
    pass


class Served:
    """the plan and its host I/O: crop k's stem columns and final map addresses, the stem and final transforms"""

    def __init__(self, model, image, chunk=CHUNK):
        import v30a_c
        import v30a_device as D
        import v30a_layout as L
        from framenpu import _mercury_common as C
        self.chunk = chunk
        C.MAX_CROPS = chunk                  # the host planner's mirror of the kernel's RS_MAX_CROPS
        # v30a_device.plan_for with the batch this image replays (it plans for 3)
        self.mp = mp = L.build_plan(model, batch=chunk, **D.OPTS)
        for k in range(chunk):
            for row in struct.iter_unpack('<32I', mp.plan.table):
                if not L.native_ok(C.replay_row(row, k, mp.plan.crop_stride, mp.win_hi), **D.CHECK):
                    raise SystemExit('v30a plan: a descriptor would be rejected at %d crops a job' % chunk)
        if list(mp.external) != [L.STEM_COLS] or list(mp.host_parts) or mp.plan.max_crops != chunk:
            raise SystemExit('v30a plan: inputs %s, host parts %s, %d crops a job' % (
                mp.external, mp.host_parts, mp.plan.max_crops))
        self.stem = v30a_c.Stem(IO.Stem(mp.grid(L.STEM_COLS), mp.shape[L.STEM_COLS][0]))
        self.final = v30a_c.Final(IO.Final(mp.grid(mp.final), mp.shape[mp.final][0], model.heads))
        self.stem_at = [mp.crop(k).where[L.STEM_COLS] for k in range(chunk)]
        self.final_at = [mp.crop(k).where[mp.final] for k in range(chunk)]
        # 32 bits over the model, the image and the plan: the heads file's and the header's graph id
        h = hashlib.sha256(b'v30a' + model.sha256.encode() + hashlib.sha256(image).digest() + mp.plan.table)
        self.gid = struct.unpack('<I', h.digest()[:4])[0]


def runner_class():
    import v30a_device as D
    from framenpu import _mercury_common as C
    from framenpu.mercury_runtime import _segment_metadata
    HL = D.HL

    class Runner(D.V30aRunner):
        """V30aRunner's resident requests with HMX power held (V30aRunner.held_power, which _SubmitHooks passes),
        the Pace wait, and the early answer: ready(meta) runs from inside the completion validator once the kernel
        statuses and the metadata check out, so the client has its outputs before the acknowledge."""
        pace = None

        def run(self, mp, n, ready=None):
            job = mp.plan.jobs[0]
            data = bytearray(C.payload(mp.plan, n))
            struct.pack_into('<5I', data, 20, *self.tag, 1)
            ndesc, late = job.end - job.first, []

            def meta(res):
                out = bytearray(res.output)
                # words 64..95: FP16 overflow flags (exponent all ones), one bit a lane, ORed over every store
                flags = sum(bin(w).count('1') for w in struct.unpack_from('<32I', out, 256))
                out[256:384] = bytes(128)
                m = _segment_metadata(bytes(out), n, ndesc, len(self.image), True, res.kernel_reference_ticks)
                return m | {'overflow_lanes': flags, 'wrapper_ms': res.kernel_reference_ticks / 19200}

            def validate(res):
                HL.linked_completion(res, len(self.image), output_bytes=C.META_BYTES)
                self._check_reservation(True)
                if ready is None:
                    return
                try:    # never raise here (a validator's exception poisons the session): again after the acknowledge
                    if not any(row[2] for row in res.workers):
                        ready(meta(res))
                except Exception as e:
                    late.append(e)
            if self.pace is not None:
                self.pace.arm(n)
            res = self.dev._submit(self.loader, bytes(data), opcode=C.RS_OPCODE,
                                   params=(1, job.first, job.end, 0, 0, 0), output_size=C.META_BYTES,
                                   use_direct=True, _completion_validator=validate,
                                   _prepare=lambda t: self._check_reservation(True))
            if late:
                raise late[0]
            return meta(res)

    return Runner


class Service:
    """the self-test and one client's requests at a time until SIGTERM or SIGINT (signals as service.Service: Stop
    at once only while waiting for the client, else a flag read between jobs)"""

    def __init__(self, server, memory, served, log):
        self.server, self.s, self.log = server, served, log
        self.shm = memory.map
        self.shm_np = np.frombuffer(self.shm, np.uint8)
        self.stats = {'frames': 0, 'errors': 0}
        self.idle = self.stopping = False
        # NPUD_PHASES=1: wall ms summed over a client's requests (stage: stem columns into the window; submit:
        # the DSP jobs as framenpu runs them; read: the final maps to outputs), logged when it leaves
        self.phases = {} if os.environ.get('NPUD_PHASES') else None

    def _phase(self, k, t0):
        if self.phases is not None:
            self.phases[k] = self.phases.get(k, 0.0) + (time.perf_counter() - t0) * 1e3

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

    def crops(self, runner, crops, done=None):
        """crops (u8 224 x 224 arrays) through the DSP, the plan's chunk at a time: (f32 [n, FLOATS], DSP ms). done(outs, dsp
        ms) runs from the last job's early answer; a stray padding half or an overflow lane raises ValueError"""
        s, mem = self.s, runner.mem
        outs, dsp = np.empty((len(crops), IO.FLOATS), np.float32), [0.0]

        def read(at, m, meta):
            t0 = time.perf_counter()
            if meta['overflow_lanes']:
                raise ValueError('FP16 overflow in %d lanes' % meta['overflow_lanes'])
            for k in range(m):
                a = s.final_at[k]
                outs[at + k], stray = s.final(bytes(mem[a:a + s.final.nbytes]))
                if stray:
                    raise ValueError('crop %d: %d stray padding halves' % (at + k, stray))
            dsp[0] += meta['wrapper_ms']
            self._phase('read', t0)

        for at in range(0, len(crops), s.chunk):
            m = min(s.chunk, len(crops) - at)
            t0 = time.perf_counter()
            for k in range(m):
                a = s.stem_at[k]
                mem[a:a + s.stem.nbytes] = s.stem(crops[at + k])
            self._phase('stage', t0)
            last = at + m == len(crops)
            got = []

            def ready(meta, at=at, m=m):
                read(at, m, meta)
                got.append(True)
                if done is not None:
                    done(outs, dsp[0])
            t0 = time.perf_counter()
            meta = runner.run(s.mp, m, ready if last else None)
            self._phase('submit', t0)   # holds the last job's read (the early answer runs inside it)
            if self.phases is not None:   # framenpu's own split of it (_SubmitHooks: device.py marks)
                for k, v in (runner.last_phases or {}).items():
                    self.phases['fw.' + k] = self.phases.get('fw.' + k, 0.0) + v
            if not last:
                read(at, m, meta)
            elif not got:
                raise ValueError('a request finished without its answer')
        return outs, dsp[0]

    def selftest(self, runner, model, crops):
        import v30a_graph as G
        alone = []
        for c in crops:
            alone.append(self.crops(runner, [c])[0][0])
        host = G.run(model, crops[0], 'fp32')
        o = alone[0]
        errs = {'out0_px': float(np.abs(o[:63] - host['out0']).max()),
                'prob': float(max(abs(o[63] - host['out1'][0]), abs(o[64] - host['out2'][0]))),
                'world': float(np.abs(o[65:] - host['out3']).max())}
        if any(errs[k] > SELFTEST[k] for k in SELFTEST):
            raise SystemExit('self-test: crop 0 against the host FP32 graph: %s' % errs)
        want = np.stack(alone)
        for n in (4, 3, 2, 1):
            got = self.crops(runner, crops[:n])[0]
            if not np.array_equal(got.view(np.uint32), want[:n].view(np.uint32)):
                raise SystemExit('self-test: a %d-crop request differs from the crops alone' % n)
        return errs

    def _header(self, last, wrap, daemon_ms):
        frames = self.stats['frames']
        T.HDR.pack_into(self.shm, 0, T.MAGIC, T.VERSION, last, frames, 0, frames, self.stats['errors'],
                        self.s.gid, wrap, daemon_ms, 0, 0, 0, 0, 0, 0)

    def serve(self, runner):
        server, stats = self.server, self.stats
        if self.phases:
            self.phases.clear()       # the self-test's
        self._header(0, 0, 0)
        joined = 0
        server.listen()
        while True:
            self.idle = True
            try:
                self.check_stop()
                ev, b = server.wait()
            finally:
                self.idle = False
            if ev == 'joined':
                self.log('client connected')
                joined = stats['frames']
                continue
            if ev == 'left':
                self._left(stats['frames'] - joined)
                continue
            req = T.parse(b, (0,), ())
            if req is None:
                stats['errors'] += 1
                if not server.reply(T.BAD):
                    self._left(stats['frames'] - joined)
                continue
            _, n, _ = req
            t0 = time.perf_counter()
            crops = [self.shm_np[T.CROPS + k * T.CROP: T.CROPS + (k + 1) * T.CROP].reshape(224, 224).copy()
                     for k in range(n)]
            answer = {}

            def done(outs, wrap):
                self.shm_np[T.OUTS:T.OUTS + outs.nbytes] = outs.view(np.uint8).ravel()
                stats['frames'] += 1
                self._header(b[0], wrap, (time.perf_counter() - t0) * 1e3)
                answer['sent'] = server.reply(T.OK)
            try:
                self.crops(runner, crops, done)
            except ValueError as e:    # a bad request's outputs (not the session's): BAD, and the next request
                stats['errors'] += 1
                self.log('request refused: %s' % e)
                if 'sent' not in answer and not server.reply(T.BAD):
                    self._left(stats['frames'] - joined)
                continue
            if answer.get('sent') is False:
                self._left(stats['frames'] - joined)

    def _left(self, nframes):
        self.log('client left after %d frames' % nframes)
        if self.phases:
            self.log('phases ms a request: ' + ', '.join('%s %.3f' % (k, v / max(nframes, 1))
                                                       for k, v in self.phases.items()))
            self.phases.clear()


def main(a, log):
    """ft_npud.main's arguments (a.graph a v30a graph file); root, cores already set"""
    import v30a_device as D       # framenpu on sys.path, and HL's 20 KiB reservation
    from framenpu import Device, RecoveryRequired
    from framenpu import _mercury_common as C
    from framenpu.errors import DeviceBusy
    from framenpu.resident_gemm import _lease, RECORD
    from framenpu.resident_landmark import CONTROL, ARENA_START, ARENA_END
    from hardware_hmx import PowerSnapshot, audited_device
    from hardware_resident import settled_health
    from instrument import Pace
    from trust import untrusted, loaded_files, run_dir_untrusted
    HL = D.HL

    if a.det is not None:
        log('v30a: --det not served (the detector runs on the INT8 image); the client keeps its CPU palm search')
    log('v30a: link %s (--link names the INT8 image)' % LINK)
    try:
        loader, image, record = HL.load_link(LINK)
    except (OSError, ValueError, KeyError) as e:
        raise SystemExit('v30a link: %s' % e)
    if (record.get('npu_hands', {}).get('ops') != 'stride-2 nk 2 (max pool), 3, 5' or record.get('s2_even')
            or record.get('reservation', {}).get('bytes') != HL.CODE_BYTES):
        raise SystemExit('v30a link: not a build_v30a_link.py image')
    chunk = record.get('max_crops', CHUNK)
    if type(chunk) is not int or chunk not in CHUNKS:
        raise SystemExit('v30a link: max_crops %r' % (chunk,))
    log('v30a: %d crops a job' % chunk)
    model = load_model(a.graph)
    s = Served(model, image, chunk)
    heads = heads_file.pack_float(s.gid)
    Runner = runner_class()
    if a.require_root_owned:
        bad = untrusted(loaded_files([LINK / n for n in LINK_FILES] + [a.graph]))
        if bad:
            raise SystemExit('ft-npud: loaded from files that aren\'t root-only: %s' % '; '.join(bad))
    bad = run_dir_untrusted(a.dir)
    if bad:
        raise SystemExit('ft-npud: %s must be root-owned and not group/other-writable: %s' % (a.dir, '; '.join(bad)))
    a.dir = Path(os.path.realpath(a.dir))
    sock_path, heads_path, old_shm = a.dir / 'ft-npud.sock', a.dir / 'ft-npud.heads', a.dir / 'ft-npud.shm'
    try:
        lease = _lease()
    except DeviceBusy:
        raise SystemExit('ft-npud: another process holds the NPU (a running ft-npud?); %s left as it is' % sock_path)
    selftest_crops = list(np.random.default_rng(5).integers(0, 256, (4, 224, 224), dtype=np.uint8))
    server = svc = None
    wrote_heads = False
    try:
        a.dir.mkdir(mode=0o755, exist_ok=True)
        for p in (heads_path, old_shm):
            if p.exists() or p.is_symlink():
                p.unlink()
        hfd = os.open(heads_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o644)
        wrote_heads = True
        os.write(hfd, heads)
        os.close(hfd)
        memory = T.Memory()
        server = T.Server(sock_path, a.uid, memory)
        svc = Service(server, memory, s, log)
        svc.install_signals()

        with PowerSnapshot() as power:
            baseline = power.read()
            settled_health(power, baseline)
            with Device() as observer:
                mem = observer._transport.memory
                if bytes(mem[CONTROL:CONTROL + 8]) == RECORD:
                    raise SystemExit('resident refusal record present')
                before = bytes(mem[ARENA_START:ARENA_END])
            window = before[C.WIN_LO - ARENA_START:C.WIN_HI_WIDE - ARENA_START]
            reservation = before[HL.CODE - ARENA_START:HL.CODE + HL.CODE_BYTES - ARENA_START]
            unsafe, retired = False, True
            audits = {'restored_jobs': 0, 'restored_hmx_jobs': 0}
            try:
                with audited_device(power, audits)(dispatch='direct') as dev:
                    runner = Runner(dev, dev._transport.memory, loader, image)
                    runner.mem[C.WIN_LO:C.WIN_HI_WIDE] = bytes(C.WIN_HI_WIDE - C.WIN_LO)
                    runner.install(s.mp)      # the table and the resident images; a fresh residency tag
                    try:
                        runner.seal()
                        runner.held_power = dev._hold_power()
                        log('image sealed, power held: self-test')
                        errs = svc.selftest(runner, model, selftest_crops)
                        svc.check_stop()
                        if not os.environ.get('NPUD_NOPACE'):
                            runner.pace = Pace(dev._transport)
                            dev._transport.wait = runner.pace.wait
                        log('self-test bit-exact (crop 0 vs host fp32: %s); graph %08x (v30a); serving %s' % (
                            ', '.join('%s %.3g' % kv for kv in errs.items()), s.gid, sock_path))
                        svc.serve(runner)
                    except RecoveryRequired:
                        unsafe = True
                        raise
                    except Stop:
                        log('stopping')
                    finally:
                        if runner.held_power is not None:
                            held, runner.held_power = runner.held_power, None
                            if unsafe or dev._transport.poisoned:
                                held.abandon()
                            else:
                                dev._release_power(held)
                        if runner.sealed:
                            retired = False
                            if not unsafe and not dev._transport.poisoned:
                                runner.admin(8)
                                retired = True
            except RecoveryRequired:
                unsafe = True
                raise
            finally:
                after = None
                if not unsafe and retired:
                    with Device() as observer:
                        mem = observer._transport.memory
                        mem[C.WIN_LO:C.WIN_HI_WIDE] = window
                        mem[HL.CODE:HL.CODE + HL.CODE_BYTES] = reservation
                        after = bytes(mem[ARENA_START:ARENA_END])
                log('arena restored: %s' % (after == before))
            if after != before:
                raise SystemExit('arena differs after restoration')
            settled_health(power, baseline)
    finally:
        if server is not None:
            server.close()
        if wrote_heads:
            heads_path.unlink(missing_ok=True)
        lease.close()
        if svc is not None:
            log('exit after %d frames, %d bad requests, %d connections refused' % (
                svc.stats['frames'], svc.stats['errors'], server.rejected))


if __name__ == '__main__':
    if len(sys.argv) == 4 and sys.argv[1] == 'pack':
        pack(sys.argv[2], sys.argv[3])
    else:
        sys.exit(__doc__)
