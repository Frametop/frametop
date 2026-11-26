"""hnet_serve.py --graph FOLDED.npz --uid UID [--dir RUN_DIR] [--cpus 2-4] [--require-root-owned]: a trained
flat H-net served like v30a.

A prototype, to measure a flat H-net (train/hnet.py, folded by train/fold.py) through ft-npud's real socket and
with Frametop's own hands client. It is v30a_serve.py's session unchanged (the lease, the seal, HMX power
held, the paced wait, the early answer, the request loop, the teardown) with the three model-specific parts
replaced:

  the graph file   train/fold.py's npz: the nine device convolutions, the CPU heads, and 48 evaluation crops
                   with the float model's outputs for them
  Served           probe/hnet_plan.py's plan and probe/host_c.py's host path (a crop's pixels scattered into
                   the window in C; the head tensor gathered, soft-argmax and heads in C)
  the self-test    the npz's first four crops alone against the float model's outputs for them (a 160
                   network: the first against the host reference, selftest_set), inside v30a_serve.SELFTEST,
                   then 4-, 3-, 2- and 1-crop requests bit-equal to the crops alone

The client sees what it sees from v30a: heads file version 3, 128 f32 a crop (63 screen, presence, right, 63
world). A 224 network needs no client change. A 160 network reads the first 160 x 160 bytes of each slot; the
hands client's version 3 parser accepts only 224 (track/npuheads.cpp), so only this repo's test client can
feed it.

Lives in a tree built by ship/hnet/mktree.sh (beside v30a_serve.py; TREE/probe links this repo's probe/) or,
installed as the live service, in the root-owned tree of ship/hnet/mkinstall.sh, where probe/ and the C
helper are copies inside the tree and the unit passes --require-root-owned (v30a_serve's audit of every
loaded file, which then covers them). Root, as ft_npud.py. Still a prototype: the model swap is three
assignments on v30a_serve, and Final has no padding check (ship/hnet/README.md).
"""
import argparse
import ctypes
import hashlib
import os
import struct
import sys
import time
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import v30a_serve as V                   # noqa: E402  (this tree's: four crops a job, the early answer)

sys.path.insert(0, str(HERE.parent / 'probe'))
import hnet_plan as H                    # noqa: E402
import hnet_real as HR                   # noqa: E402
import host_c                            # noqa: E402
import host_fast                         # noqa: E402
from synth_plan import C                 # noqa: E402  (framenpu's _mercury_common)

FLOATS = 128


def load(path):
    """-> (the npz's arrays, the device graph)"""
    return HR.load(path)


def selftest_set(z, model):
    """-> (four u8 crops as a client sends them, the reference outputs [k, 128] of the first k).
    A 224 network's file holds such crops and the float model's outputs for them: k = 4. A 160 network's file
    holds 224 crops and the reduced input the model made of each, which is not u8; the crops are then that
    input rounded to u8, and the reference is the device graph's FP16 host reference with the numpy heads
    on the first crop as staged: k = 1 (as v30a's self-test, which checks crop 0 against its host graph)."""
    side = int(z['inp'])
    if z['img'].shape[1] == side:
        ref = np.concatenate([z['screen'][:4], z['pres_out'][:4], z['right_out'][:4], z['world'][:4]], 1)
        return list(z['img'][:4]), ref.astype(np.float32)
    x = z['x16'][:4].astype(np.float32)                  # [4, 16, q, q], channel dy * 4 + dx
    q = x.shape[-1]
    img = x.reshape(4, 4, 4, q, q).transpose(0, 3, 1, 4, 2).reshape(4, side, side)
    crops = list(np.rint(img * 255).clip(0, 255).astype(np.uint8))
    staged = host_fast.LUT.view('<f2')[crops[0]].astype(np.float32)
    o = HR.heads(H.reference(model, staged.reshape(q, 4, q, 4).transpose(1, 3, 0, 2).reshape(16, q, q)), z)
    return crops, np.concatenate([np.ravel(v) for v in o]).astype(np.float32)[None]


class Stem:
    """a u8 crop (the slot's first side x side bytes) -> crop k's window bytes"""

    def __init__(self, host, side):
        self.host, self.side, self.nbytes = host, side, len(host.bytes)

    def __call__(self, crop):
        crop = np.ascontiguousarray(crop)
        if crop.dtype != np.uint8 or crop.size < self.side * self.side:
            raise ValueError('a crop of %s %s' % (crop.shape, crop.dtype))
        self.host.lib.hc_stage(crop.ctypes.data, *self.host.stage_args)
        return self.host.bytes


class Final:
    """one crop's stored head tensor -> (128 f32, 0): the fast path has no padding check, so no stray count"""

    def __init__(self, host):
        self.host, self.nbytes = host, 2 * host.nraw

    def __call__(self, raw):
        if len(raw) != self.nbytes:
            raise ValueError('a head tensor of %d bytes' % len(raw))
        out = np.empty(FLOATS, np.float32)
        self.host.lib.hc_heads(raw, *self.host.head_args, out.ctypes.data)
        return out, 0


class Served:
    """v30a_serve.Served's fields for the H-net: the plan, crop k's input and head addresses, the transforms"""

    def __init__(self, loaded, image, chunk=V.CHUNK):
        z, model = loaded
        self.chunk = chunk
        C.MAX_CROPS = chunk
        self.mp = mp = H.plan(model, chunk)       # raises if the kernel would reject a descriptor
        host = host_c.CHost(model, mp, z)
        self.side = int(z['inp'])
        self.stem, self.final = Stem(host, self.side), Final(host)
        self.stem_at, self.final_at = host.x_at, host.o_at
        w = hashlib.sha256()
        for name in sorted(k for k in z if k[:2] in ('w.', 'b.') or k.split('.')[0] in ('pres', 'right', 'world0', 'world2')):
            w.update(name.encode() + np.ascontiguousarray(z[name]).tobytes())
        h = hashlib.sha256(b'hnet' + w.digest() + hashlib.sha256(image).digest() + mp.plan.table)
        self.gid = struct.unpack('<I', h.digest()[:4])[0]


class Service(V.Service):
    def selftest(self, runner, loaded, _crops):
        crops, ref = selftest_set(*loaded)
        alone = np.stack([self.crops(runner, [c])[0][0] for c in crops])
        d = np.abs(alone[:len(ref)] - ref)
        errs = {'out0_px': float(d[:, :63].max()), 'prob': float(d[:, 63:65].max()), 'world': float(d[:, 65:].max())}
        if any(errs[k] > V.SELFTEST[k] for k in V.SELFTEST):
            raise SystemExit('self-test: %d crops against the reference: %s' % (len(ref), errs))
        for n in (4, 3, 2, 1):
            got = self.crops(runner, crops[:n])[0]
            if not np.array_equal(got.view(np.uint32), alone[:n].view(np.uint32)):
                raise SystemExit('self-test: a %d-crop request differs from the crops alone' % n)
        return errs


def main(a, log):
    V.load_model, V.Served, V.Service = load, Served, Service
    log('H-net prototype: %s' % a.graph)
    return V.main(a, log)


if __name__ == '__main__':
    from affinity import pick_cpus
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--graph', type=Path, required=True)
    ap.add_argument('--uid', type=int, required=True)
    ap.add_argument('--dir', type=Path, default=Path('/run/ft-npud-rethink'))
    ap.add_argument('--cpus', default='2-4')
    ap.add_argument('--require-root-owned', action='store_true', help='refuse to serve unless every file loaded '
                    '(code, link build, graph, the C helper) is root-only all the way up (trust.py)')
    a = ap.parse_args()
    a.det = None
    ctypes.CDLL(None, use_errno=True).prctl(29, 1000, 0, 0, 0)   # PR_SET_TIMERSLACK, 1 us (as ft_npud.py)
    if os.geteuid() != 0:
        ap.error('root required')
    cpus = pick_cpus(a.cpus)
    if cpus:
        os.sched_setaffinity(0, cpus)
    sys.exit(main(a, lambda m: print(time.strftime('%T ') + m, file=sys.stderr, flush=True)))
