"""ft-npud's output gather: a request's outputs from the graph's guard-column planes in the DSP's memory, dense,
into the client's output slots (deguard.c), and the check that holds the gather to the plane layout before the DSP
is touched."""
import ctypes, sys, time
from pathlib import Path
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'spikes/int8'))
import run_int8_link as RL


def deguard_index(C, h, w, y0=0, hp=None):
    """indices into a [C4][PIN][4] plane's bytes for the dense [C, h, w] tensor: rows y0.. of a plane hp rows high
    (a packed plane holds crop c at y0 = c * (h + 1), RL.stacked)."""
    PIN, ws = RL.pin_of(hp or h, w), w + 1
    c = np.arange(C)[:, None, None]
    y = np.arange(h)[None, :, None] + y0
    x = np.arange(w)[None, None, :]
    return ((c // 4) * PIN + 1 + (y + 1) * ws + 1 + x) * 4 + c % 4


SCRATCH = np.empty(1 << 20, np.uint8)   # one cached copy of an output's planes (the DSP's memory reads slowly)


def gathers(plan, ddr, n, shapes):
    """per output of an n-crop request: (DDR offset, bytes, view) in the graph's output order, view = (first crop's
    byte offset, crop stride, C4, PIN, h, w). Channel 4*g + k, row y, column x of crop c is byte
    at + c*stride + (g*PIN + y*(w+1) + x)*4 + k, at = ((y0 + 1)*(w + 1) + 2)*4 for its first row y0 (deguard_index;
    check_gathers holds these to it)."""
    g = []
    for nm, (_, ch, h, w) in zip(plan.out_names, shapes):
        off, size = ddr[nm]
        C4, ws = (ch + 3) // 4, w + 1
        if plan.packed(nm, n):
            g.append((off, size, ((ws + 2) * 4, (h + 1) * ws * 4, C4, RL.pin_of(RL.pack_rows(n, h), w), h, w)))
        else:
            g.append((off, n * size, ((ws + 2) * 4, size, C4, RL.pin_of(h, w), h, w)))
        nbytes, (at, stride, C4, pin, h, w) = g[-1][1], g[-1][2]
        if nbytes > SCRATCH.size or at + (n - 1) * stride + (C4 - 1) * pin * 4 + ((h - 1) * ws + w) * 4 > nbytes:
            raise SystemExit('%s: the gather would read outside its %d output bytes' % (nm, nbytes))
    return g


_deguard = ctypes.CDLL(str(Path(__file__).with_name('deguard.so'))).deguard   # deguard.c
_deguard.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_long, ctypes.c_int, ctypes.c_long,
                     ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_long]
_deguard.restype = None


def publish(mem, g, n, out, shapes, t=None):
    """the request's outputs, dense, into out (a contiguous u8 array of n output slots, one slot = the shapes'
    bytes): each output's planes in one copy (the DSP's memory reads slowly, ~1 GB/s), then deguard.c splits all
    crops' channels out of that copy"""
    if out.dtype != np.uint8 or not out.flags.c_contiguous or out.size < n * sum(c * h * w for _, c, h, w in shapes):
        raise ValueError('publish needs a contiguous u8 output of n slots')
    dst, slot = 0, sum(c * h * w for _, c, h, w in shapes)
    tmp, outp = SCRATCH.ctypes.data, out.ctypes.data
    t0 = time.perf_counter()
    for (off, nbytes, (at, stride, C4, pin, h, w)), (_, ch, _, _) in zip(g, shapes):
        np.copyto(SCRATCH[:nbytes], np.frombuffer(mem, np.uint8, nbytes, off))
        _deguard(tmp + at, outp + dst, n, stride, C4, pin, h, w, ch, slot)
        dst += ch * h * w
    if t is not None:
        t['p_gather'] = t.get('p_gather', 0.0) + (time.perf_counter() - t0) * 1e3


def parts(plan, ddrs, ns, shapes):
    """a request's gathers per worker section: [(crops, gathers)], section i's crops after section i-1's in the
    client's slots (a one-worker request is one section). Each held to the plane layout (check_gathers)."""
    out = []
    for ddr, n in zip(ddrs, ns):
        g = gathers(plan, ddr, n, shapes)
        check_gathers(plan, ddr, n, shapes, g)
        out.append((n, g))
    return out


def publish_parts(mem, parts, out, shapes, t=None):
    """publish() of every section of a request into consecutive slots of out"""
    slot, c = sum(ch * h * w for _, ch, h, w in shapes), 0
    for n, g in parts:
        publish(mem, g, n, out[c * slot:(c + n) * slot], shapes, t)
        c += n


def check_gathers(plan, ddr, n, shapes, g):
    """publish() of random planes equals the deguard_index gather (the layout the self-test checks on the DSP)"""
    rng = np.random.default_rng(n)
    span = max(off + nbytes for off, nbytes, _ in g)
    mem = bytearray(rng.integers(0, 256, span, dtype=np.uint8).tobytes())
    slot = sum(c * h * w for _, c, h, w in shapes)
    got = np.zeros(n * slot, np.uint8)
    publish(mem, g, n, got, shapes)
    want, dst = np.zeros_like(got), 0
    for nm, (_, ch, h, w) in zip(plan.out_names, shapes):
        off, size = ddr[nm]
        p = np.frombuffer(bytes(mem), np.uint8)[off:]
        for c in range(n):
            if plan.packed(nm, n):
                ix = deguard_index(ch, h, w, c * (h + 1), RL.pack_rows(n, h))
            else:
                ix = deguard_index(ch, h, w) + c * size
            want[dst + c * slot:dst + c * slot + ch * h * w] = p[ix].ravel()
        dst += ch * h * w
    if not np.array_equal(got, want):
        raise SystemExit('output gather differs from the plane layout (%d-crop request)' % n)
