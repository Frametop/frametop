"""The CPU heads' weights as one flat file for ft-hands (track/npuheads.cpp), and the graph id both sides check.

ft-npud writes DIR/ft-npud.heads from the graph it serves, and puts graph_id in its shm header, so a client always
decodes the outputs with the weights of the graph that made them.

Layout (little-endian): 12 u32  magic 'FTNH', version, graph_id, K, H, W, kpd, g, hidden, pool_h, pool_w, pad
                        3 f32   heat, kp_feat, pool scales
                        f32     grid[W], pres w[g] b[1], right w[g] b[1], world0 w[hidden][g + K * kpd] b[hidden],
                                world1 w[3 * K][hidden] b[3 * K]
Version 2 is a two-view student's (two_view.py mode f): a 4th f32 scale (pool_pre's) after the three, and after
world1 the partner terms world_p w[hidden][g + K * kpd], pres_p w[g], right_p w[g] (no biases). Version 1 files
stay what one-view graphs get, so clients from before version 2 keep working with them.
Version 3 (pack_float) is a float-output graph's (v30a on the HMX image, v30a_serve.py): the header only, K = 21,
every shape word 0 and word 11 the crop; ft-npud writes the finished outputs, FLOAT_OUTS f32 a crop (v30a_io.py),
and the client decodes nothing. Clients from before version 3 refuse the file and keep the CPU model.
The maths is int8_ref.heads (train/student in the hands training repo).

  python heads_file.py fixture G.npz OUT.bin [--n 4]   cases for tests/npuheads_test.cpp: the heads file's bytes,
                                                         then n x (heat s8, kp_feat s8, pool u8, screen f32[K][2],
                                                         pres, right, world f32[3K]) from int8_ref.heads. A
                                                         two-view graph's case is (outputs, the partner's outputs,
                                                         f32 has2, then the same floats); every 4th case has
                                                         has2 = 0 (read alone)
"""
import argparse, hashlib, json, struct, sys
from pathlib import Path

import numpy as np

MAGIC, VERSION, VERSION_2V, VERSION_FLOAT = 0x484E5446, 1, 2, 3        # 'FTNH'
FLOAT_OUTS = 128    # f32 a crop in a version 3 graph's output slot: out0 [63], out1, out2, out3 [63]


def graph_id(ops, arrays):
    """32 bits of a sha256 over the graph JSON and every array (sorted by name)."""
    h = hashlib.sha256(json.dumps(ops, sort_keys=True).encode())
    for k in sorted(arrays):
        v = np.ascontiguousarray(arrays[k])
        h.update(k.encode() + str(v.dtype).encode() + str(v.shape).encode() + v.tobytes())
    return struct.unpack('<I', h.digest()[:4])[0]


def dims(ops, arrays):
    out = [o for o in ops if o['op'] == 'outputs'][0]
    conv = {o['name']: o for o in ops if o['op'] in ('conv', 'fuse')}   # a two-view graph's pool is its fuse
    K, kpd, g = conv[out['heat']]['cout'], conv[out['kp_feat']]['cout'], conv[out['pool']]['cout']
    hidden = arrays['head.world0.w'].shape[0]
    return out, K, kpd, g, hidden


def pack(ops, arrays, gid, H=28, W=28, ph=7, pw=7, crop=224):
    """The heads file's bytes; refuses a graph without head.* arrays or with shapes that disagree."""
    if 'head.world0.w' not in arrays:
        raise ValueError('graph has no head.* arrays (exported before f287544?)')
    if H != W:
        raise ValueError('heatmap %dx%d: the client reads one grid for both axes (H must equal W)' % (H, W))
    out, K, kpd, g, hidden = dims(ops, arrays)
    sc = {o['name']: o['scale'] for o in ops if 'scale' in o}
    want = {'head.grid': (W,), 'head.pres.w': (1, g), 'head.pres.b': (1,), 'head.right.w': (1, g),
            'head.right.b': (1,), 'head.world0.w': (hidden, g + K * kpd), 'head.world0.b': (hidden,),
            'head.world1.w': (3 * K, hidden), 'head.world1.b': (3 * K,)}
    two = 'pool_pre' in out
    if two != ('head.world_p.w' in arrays):
        raise ValueError('a two-view graph needs both pool_pre and head.world_p.w')
    if two:
        pre = [o['cout'] for o in ops if o.get('name') == out['pool_pre']][0]
        if pre != g:
            raise ValueError('pool_pre: %d channels, pool %d' % (pre, g))
        want.update({'head.world_p.w': (hidden, g + K * kpd), 'head.pres_p.w': (1, g), 'head.right_p.w': (1, g)})
    for k, s in want.items():
        if tuple(arrays[k].shape) != s:
            raise ValueError('%s: shape %s, want %s' % (k, arrays[k].shape, s))
    # word 11: the crop size in pixels (files before it hold 0 there: 224)
    b = struct.pack('<12I', MAGIC, VERSION_2V if two else VERSION, gid, K, H, W, kpd, g, hidden, ph, pw, crop)
    b += struct.pack('<4f' if two else '<3f', sc[out['heat']], sc[out['kp_feat']], sc[out['pool']],
                     *([sc[out['pool_pre']]] if two else []))
    for k in want:
        b += np.ascontiguousarray(arrays[k], np.float32).tobytes()
    return b


def pack_float(gid, crop=224):
    """a version 3 heads file: the header alone (the outputs come finished, FLOAT_OUTS f32 a crop)"""
    return struct.pack('<12I', MAGIC, VERSION_FLOAT, gid, 21, 0, 0, 0, 0, 0, 0, 0, crop)


def fixture(a):
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'spikes/int8'))   # int8_ref, beside run_int8_link
    from int8_ref import heads, run_npu, run_set
    z = np.load(a.graph)
    ops = json.loads(str(z['graph']))
    arrays = {k: z[k] for k in z.files if k != 'graph'}
    out = [o for o in ops if o['op'] == 'outputs'][0]
    names = [out[k] for k in ('heat', 'kp_feat', 'pool', 'pool_pre') if k in out]
    two = 'pool_pre' in out
    side = int([o for o in ops if o['op'] == 'unshuffle'][0].get('crop', 224))
    t0 = run_npu(ops, arrays, np.zeros((side, side), np.uint8))
    b = pack(ops, arrays, graph_id(ops, arrays), *t0[out['heat']].shape[1:], *t0[out['pool']].shape[1:], crop=side)
    outs = lambda t: b''.join(t[nm].astype(np.int64).astype(np.uint8).tobytes() for nm in names)
    floats = lambda r: (r[0].astype(np.float32).tobytes() + struct.pack('<2f', r[1], r[2]) +
                        r[3].astype(np.float32).tobytes())
    rng = np.random.default_rng(3)
    for i in range(a.n):
        crops = [rng.integers(0, 256, (side, side), dtype=np.uint8) for _ in range(1 + two)]
        if not two:
            t = run_npu(ops, arrays, crops[0])
            b += outs(t) + floats(heads(ops, arrays, t))
            continue
        has2 = int(i % 4 != 3)
        t, tp = run_set(ops, arrays, crops, [has2, has2])
        b += outs(t) + outs(tp) + struct.pack('<f', has2) + floats(heads(ops, arrays, t, tp, has2))
    Path(a.out).write_bytes(b)
    print('wrote %s: %d cases, %d bytes' % (a.out, a.n, len(b)))


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    f = sub.add_parser('fixture')
    f.add_argument('graph')
    f.add_argument('out')
    f.add_argument('--n', type=int, default=4)
    fixture(ap.parse_args())
