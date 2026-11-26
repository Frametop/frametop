"""ft-npud (prototype): the root daemon that owns the Frame's CDSP session for NPU hands and runs the INT8 student
graph for an unprivileged client (ft-hands). Root on the Frame.

  ft_npud.py --link LINK_DIR (--graph G.npz | --synthetic) [--det D.npz] --uid UID [--dir DIR]

Startup, then serving, then teardown are framenpu's teacher-link protocol (spikes/int8/run_int8_link.py, itself
tests/hardware_teacher_link.py): lease, refusal record, arena saved, audited direct dispatch, image sealed, HMX power
held, and the self-test (service.py) per graph, which must equal int8_ref.run_npu bit for bit before the socket
opens: 1 crop (every tensor), then 4-, 3-, 2- and 1-crop requests through the serving path (a two-view graph's in
each pair variant). With --det the detector
graph (train/student/det.py) shares the window: its weights after the landmark graph's, one crops and outputs area
(run_int8_link.Plan.pair). On SIGTERM/SIGINT or any error: power released (abandoned if the session is unsafe),
image retired, window and reservation restored, the arena compared with its start, lease released. Never a retry
or a recovery: a RecoveryRequired ends the daemon with the DSP left for manual recovery (RESULTS.md "Poison
recovery").

Client interface: transport.py (a sealed memfd handed over the socket, one client at a time, replies that never
block). DIR/ft-npud.heads (root-owned, 644): the graph's CPU head weights (heads_file.py); its graph id is the
header's word 7, written before the socket opens. A synthetic graph has no heads file.

Modules: transport.py (the client interface), service.py (the self-test, the requests, the signals), dsp.py (one
request's DSP job), gather.py (the outputs), instrument.py (NPUD_PHASES timing), affinity.py (--cpus),
heads_file.py. A client that leaves mid-request (a timeout) only ends its connection; the daemon serves the next.
"""
import argparse, json, os, sys, time
from types import SimpleNamespace
from pathlib import Path
import numpy as np

HERE = Path(__file__).resolve().parent
# both by hand: under python -I (the unit) the script's own directory isn't on sys.path either
sys.path[:0] = [str(HERE), str(HERE.parent / 'spikes/int8')]
import run_int8_link as RL     # the plan, the record checks and the oracle helpers
import heads_file              # the CPU heads' weights for the client, and the graph id
import transport as T          # the client interface
import gather as G             # the output gather
from instrument import Pace, Stamps
from dsp import Frames         # one request's DSP job
from service import Service, Stop   # the self-test and the requests
from affinity import pick_cpus
from trust import untrusted, loaded_files, run_dir_untrusted   # the run dir's ownership; --require-root-owned
import v30a_io                 # is_graph: a v30a graph file runs v30a_serve instead (the HMX image)


# a two-worker link (build_int8_link.py --mw) splits requests of this many crops or more over HVX workers 0 and 1:
# 4 crops 2.93 -> 2.19 ms of kernel, 3 crops 2.20 -> 1.96; 2 crops gain 0.06 ms (a split loses the packing)
SPLIT_FROM = 3
# each loader build_int8_link.py makes, and the HVX workers it runs (link.json loader.workers must agree)
LOADER_WORKERS = {'teacher_link_loader_v1': 1, 'npu_mw_loader_v1': 2}
halves = RL.halves


class Served:
    """one graph in the window: its plan, the serving requests (1..4 crops; on a two-worker link, SPLIT_FROM and
    up split over two workers) and their gathers per worker section, the self-test's observe request and int8_ref
    outputs. A two-view graph's requests come in pair variants: data[(n, pairs)] has the FUSE records' has2 bits for
    the request's pair bits (transport.parse), want[(n, pairs)] its int8_ref outputs."""

    def __init__(self, ops, arrays, plan, shapes, crops, workers=1):
        self.ops, self.plan, self.shapes = ops, plan, shapes
        self.slot = sum(c * h * w for _, c, h, w in shapes)
        split = lambda n: workers == 2 and n >= SPLIT_FROM
        self.reqs = {n: plan.split(*halves(n)) if split(n) else plan.records(n, False) for n in (1, 2, 3, 4)}
        self.gather = {n: G.parts(plan, r[2], halves(n), shapes) if split(n) else G.parts(plan, (r[2],), (n,), shapes)
                       for n, r in self.reqs.items()}
        self.observe = plan.records(1, True)
        crops = [c.ravel()[:plan.crop ** 2].reshape(plan.crop, plan.crop) for c in crops]
        dense = lambda r: np.concatenate([r[nm].astype(np.int64).astype(np.uint8).ravel() for nm in plan.out_names])
        # each crop alone, and (two-view) each pair read together: a crop's outputs depend only on its own pixels
        # and, with its pair's bit, its partner's (int8_ref.run_set)
        alone = [RL.run_npu(ops, arrays, c) for c in crops]
        both = RL.run_set(ops, arrays, crops[:2], [1, 1]) + RL.run_set(ops, arrays, crops[2:4], [1, 1]) \
            if plan.two_view else None
        self.data, self.want = {}, {}
        for n, r in self.reqs.items():
            for pairs in (range(4) if plan.two_view else (0,)):
                if T.parse(bytes([n | pairs << 5]), (0,), (0,)) is None:
                    continue
                mask = T.pair_mask(pairs)
                self.data[n, pairs] = RL.set_has2(r[0], mask) if plan.two_view else r[0]
                self.want[n, pairs] = np.concatenate([dense(both[c] if mask >> c & 1 else alone[c]) for c in range(n)])
        self.ref = alone[0]
        for o in plan.ops:
            if o['op'] == 'conv' and o['stride'] == 2 and o['src'] != 'in':
                self.ref[o['src'] + '/2'] = self.ref[o['src']]


def load_graph(path):
    z = np.load(path)
    return json.loads(str(z['graph'])), {k: z[k] for k in z.files if k != 'graph'}


def prepare(a, ap, workers=1):
    """the graphs to serve, from the arguments: plans, shapes, the heads file's bytes, the self-test's crops and
    references (nothing touches the DSP); workers: the link's HVX workers (link.json)"""
    ops, arrays = load_graph(a.graph) if a.graph else RL.synthetic(crop=a.crop or RL.SLOT)[:2]
    if a.crop and a.crop != RL.SLOT:
        [o for o in ops if o['op'] == 'unshuffle'][0]['crop'] = a.crop
    try:
        crop = RL.crop_of(ops)
    except ValueError as e:
        ap.error(str(e))
    gid = heads_file.graph_id(ops, arrays)
    # the self-test's crops as the 4 DDR slots hold them; a graph at a smaller crop reads each slot's first rows
    selftest_crops = np.random.default_rng(5).integers(0, 256, (4, RL.SLOT, RL.SLOT), dtype=np.uint8)
    dops = darrays = None
    if a.det:
        dops, darrays = load_graph(a.det)
        lplan, dplan = RL.Plan.pair((ops, arrays), (dops, darrays))
        if (dplan.out_names != ['obj', 'reg'] or
                [tuple(dplan.shape[n]) for n in dplan.out_names] != [s[1:] for s in T.DET_SHAPES]):
            ap.error('--det: not a detector graph (outputs obj [1, 28, 28], reg [4, 28, 28])')
        det_scales = [[o['scale'] for o in dops if o.get('name') == n][0] for n in dplan.out_names]
    else:
        lplan, dplan, det_scales = RL.Plan(ops, arrays), None, [0.0, 0.0]
    kinds = ('heat', 'kp_feat', 'pool') + (('pool_pre',) if lplan.two_view else ())
    if lplan.two_view != ('pool_pre' in lplan.outputs):
        ap.error('a two-view graph (fuse) must output pool_pre, and only one')
    lplan.out_names = [lplan.outputs[k] for k in kinds]
    # the landmark outputs at this crop (28, 28, 7 at 224; 20, 20, 5 at 160), and up to the slots' channels (the 21
    # keypoints exactly): dense in the shm slots, which are sized for 224 and 128 pool channels
    shapes = tuple((k,) + tuple(lplan.shape[nm]) for k, nm in zip(kinds, lplan.out_names))
    if not T.fits_slots(shapes):
        ap.error('landmark outputs %s do not fit the shm slots %s' % (shapes, T.OUT_SHAPES))
    heads = heads_file.pack(ops, arrays, gid, H=shapes[0][2], W=shapes[0][3], ph=shapes[2][2], pw=shapes[2][3],
                            crop=crop) if 'head.world0.w' in arrays else None
    graphs = {0: Served(ops, arrays, lplan, shapes, selftest_crops, workers)}
    if dplan is not None:
        graphs[T.DET] = Served(dops, darrays, dplan, T.DET_SHAPES, selftest_crops, workers)
    features = T.FEAT_DET if dplan is not None else 0
    outs_at = {0: T.OUTS, T.DET: T.DET_OUTS}
    crops_at = lplan.crops_at
    assert all(g.plan.crops_at == crops_at for g in graphs.values())
    det_gid = heads_file.graph_id(dops, darrays) if dplan is not None else 0
    return SimpleNamespace(graphs=graphs, crops_at=crops_at, outs_at=outs_at, selftest_crops=selftest_crops,
                           gid=gid, det_gid=det_gid, features=features, det_scales=det_scales, heads=heads)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--link', type=Path, required=True)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument('--graph', type=Path)
    g.add_argument('--synthetic', action='store_true')
    ap.add_argument('--det', type=Path, help='also serve this detector graph (det.py export)')
    ap.add_argument('--crop', type=int, help='the landmark graph\'s crop size when its file does not say (192: a '
                    'student trained at 192); the detector stays at 224')
    ap.add_argument('--uid', type=int, required=True)
    ap.add_argument('--dir', type=Path, default=Path('/run/ft-npud'))
    ap.add_argument('--require-root-owned', action='store_true', help='refuse to serve unless every file loaded '
                    '(code, link build, graphs) is root-only all the way up (trust.py; the installed unit sets it)')
    ap.add_argument('--cpus', default='auto', help='the cores to run on: auto (every core with at least half the '
                    'biggest core\'s capacity: not the Frame\'s little cores 0-1), all, or a list such as 2-4')
    a = ap.parse_args()
    # framenpu's completion polls sleep 0.1 ms at a time: the default 50 us timer slack adds up to half that to
    # each one (and to the Pace sleep)
    import ctypes
    ctypes.CDLL(None, use_errno=True).prctl(29, 1000, 0, 0, 0)   # PR_SET_TIMERSLACK, 1 us
    if os.geteuid() != 0:
        ap.error('root required')
    log = (lambda m: print(time.strftime('%T ') + m, file=sys.stderr, flush=True))
    try:
        cpus = pick_cpus(a.cpus)
    except ValueError as e:
        ap.error(str(e))
    if cpus:
        os.sched_setaffinity(0, cpus)
    log('cores %s' % ','.join(map(str, sorted(os.sched_getaffinity(0)))))
    if a.graph is not None and v30a_io.is_graph(a.graph):
        import v30a_serve      # imports framenpu's device side: only here
        return v30a_serve.main(a, log)

    try:
        rec = json.loads((a.link / 'link.json').read_text())
        loader_rec = rec['loader']
        workers, name = loader_rec.get('workers', 1), loader_rec['name']
    except (OSError, ValueError, KeyError, AttributeError, TypeError) as e:
        ap.error('link build record: %s' % e)
    if type(workers) is not int or not isinstance(name, str) or LOADER_WORKERS.get(name) != workers:
        ap.error('link build record: loader %r with %r workers' % (name, workers))
    c = prepare(a, ap, workers)
    graphs, crops_at, outs_at, selftest_crops = c.graphs, c.crops_at, c.outs_at, c.selftest_crops
    gid, features, det_scales, heads = c.gid, c.features, c.det_scales, c.heads

    from framenpu import Device, Job, RecoveryRequired
    from framenpu.resident_gemm import _lease, RECORD
    from framenpu.errors import DeviceBusy
    from framenpu.resident_landmark import CONTROL, ARENA_START, ARENA_END
    from hardware_hmx import PowerSnapshot, audited_device
    from hardware_resident import settled_health
    import hardware_teacher_link as TL

    loader = Job.load(a.link / 'teacher_link_loader.fjob')
    image = (a.link / 'int8_link.bin').read_bytes()
    import hashlib
    if (hashlib.sha256(loader.code).hexdigest() != rec['loader']['sha256'] or
            hashlib.sha256(image).hexdigest() != rec['linked']['sha256'] or not 0 < len(image) <= TL.SEAL_AT - TL.CODE):
        ap.error('link build record differs')

    # NPUD_PHASES=1: log framenpu's per-phase wall times (summed over a client's requests) when the client leaves
    phases = {} if os.environ.get('NPUD_PHASES') else None
    frame = Frames(loader, image, phases)
    # every import is done by here (Frames imports framenpu's last ones): a second check after the unit's
    # ExecStartPre (trust.py over the install dirs, before anything ran), for anything loaded from outside them
    if a.require_root_owned:
        bad = untrusted(loaded_files([a.link / n for n in ('link.json', 'teacher_link_loader.fjob', 'int8_link.bin')] +
                                     [p for p in (a.graph, a.det) if p is not None] +
                                     ([HERE / 'ticks.so'] if phases is not None else [])))   # Stamps loads it later
        if bad:
            raise SystemExit('ft-npud: loaded from files that aren\'t root-only: %s' % '; '.join(bad))

    class Link:
        _check_reservation, admin, seal = TL.LinkRunner._check_reservation, TL.LinkRunner.admin, TL.LinkRunner.seal

    # the run dir, checked before anything is touched (trust.run_dir_untrusted)
    bad = run_dir_untrusted(a.dir)
    if bad:
        raise SystemExit('ft-npud: %s must be root-owned and not group/other-writable: %s' % (a.dir, '; '.join(bad)))
    # from here on one spelling: the checked, resolved one (every part of it is root's, so it can't be retargeted)
    a.dir = Path(os.path.realpath(a.dir))
    sock_path, heads_path, old_shm = a.dir / 'ft-npud.sock', a.dir / 'ft-npud.heads', a.dir / 'ft-npud.shm'
    # then the lease: a second daemon stops here (DeviceBusy), before it touches the running one's socket and heads
    try:
        lease = _lease()
    except DeviceBusy:
        raise SystemExit('ft-npud: another process holds the NPU (a running ft-npud?); %s left as it is' % sock_path)
    server = svc = None
    wrote_heads = False
    try:
        # the client side, created before the DSP is touched, opened for requests after the self-test
        a.dir.mkdir(mode=0o755, exist_ok=True)
        for p in (heads_path, old_shm):   # old_shm: protocol 1's client-owned file
            if p.exists() or p.is_symlink():
                p.unlink()
        if heads is not None:
            hfd = os.open(heads_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o644)
            wrote_heads = True
            os.write(hfd, heads)
            os.close(hfd)
        memory = T.Memory()
        server = T.Server(sock_path, a.uid, memory)
        svc = Service(server, memory, graphs, crops_at, outs_at, selftest_crops, (features, gid, det_scales), frame, log)
        svc.install_signals()

        with PowerSnapshot() as power:
            baseline = power.read()
            settled_health(power, baseline)
            with Device() as observer:
                mem = observer._transport.memory
                if bytes(mem[CONTROL:CONTROL + 8]) == RECORD:
                    raise SystemExit('resident refusal record present')
                before = bytes(mem[ARENA_START:ARENA_END])
            window = before[RL.WIN_LO - ARENA_START:RL.WIN_HI - ARENA_START]
            reservation = before[TL.CODE - ARENA_START:TL.CODE + TL.CODE_BYTES - ARENA_START]
            unsafe, retired = False, True
            audits = {'restored_jobs': 0, 'restored_hmx_jobs': 0}
            try:
                with audited_device(power, audits)(dispatch='direct') as dev:
                    lk = Link()
                    lk.dev, lk.mem, lk.loader, lk.image, lk.sealed, lk.held = dev, dev._transport.memory, loader, image, False, None
                    lk.reservation = image + bytes(TL.CODE_BYTES - len(image))
                    lk.pace = None
                    lk.stamp = None
                    mem = lk.mem
                    mem[RL.WIN_LO:RL.WIN_HI] = bytes(RL.WIN_HI - RL.WIN_LO)
                    for g in graphs.values():
                        for off, blob in g.plan.blobs:
                            mem[off:off + len(blob)] = blob
                    try:
                        lk.seal()
                        lk.held = dev._hold_power()
                        log('image sealed, power held: self-test')
                        svc.selftest(lk, mem)
                        svc.check_stop()
                        if not os.environ.get('NPUD_NOPACE'):
                            lk.pace = Pace(dev._transport)       # after the self-test: it ran unpaced
                            dev._transport.wait = lk.pace.wait
                        if phases is not None:
                            lk.stamp = Stamps(dev._transport, phases)
                        log('self-test bit-exact; graph %08x%s; serving %s' % (
                            gid, ' + detector %08x' % c.det_gid if c.det_gid else '', sock_path))
                        svc.serve(lk, mem)
                    except RecoveryRequired:
                        unsafe = True
                        raise
                    except Stop:
                        log('stopping')
                    finally:
                        if lk.held is not None:
                            held, lk.held = lk.held, None
                            if unsafe or dev._transport.poisoned:
                                held.abandon()
                            else:
                                dev._release_power(held)
                        if lk.sealed:
                            retired = False
                            if not unsafe and not dev._transport.poisoned:
                                lk.admin(8)
                                retired = True
            except RecoveryRequired:
                unsafe = True
                raise
            finally:
                after = None
                if not unsafe and retired:
                    with Device() as observer:
                        mem = observer._transport.memory
                        mem[RL.WIN_LO:RL.WIN_HI] = window
                        mem[TL.CODE:TL.CODE + TL.CODE_BYTES] = reservation
                        after = bytes(mem[ARENA_START:ARENA_END])
                log('arena restored: %s' % (after == before))
            if after != before:
                raise SystemExit('arena differs after restoration')
            settled_health(power, baseline)
    finally:
        if server is not None:
            server.close()
        if wrote_heads:               # only the file this run made (O_EXCL), never one found in the dir
            heads_path.unlink(missing_ok=True)
        lease.close()
        if svc is not None:
            log('exit after %d frames, %d bad requests, %d connections refused' % (
                svc.stats['frames'], svc.stats['errors'], server.rejected))


if __name__ == '__main__':
    main()
