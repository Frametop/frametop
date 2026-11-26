"""v30a_io.Stem and v30a_io.Final with their two large loops in C (v30a_hostc.c, built to v30a_hostc.so beside
this file). Same calls and the same results: Stem's bytes are equal; Final's sums are FP64 as there, added in
map order instead of numpy's pairwise order, so an output can differ in its last float32 bit. test_v30a_c.py
checks both against v30a_io on the CPU. On the Frame a crop's stem falls from 0.50 to 0.09 ms and a final map
from 0.16 to 0.04 ms (npu-rethink REPORT.md)."""
import ctypes
from pathlib import Path

import numpy as np

SO = Path(__file__).with_name('v30a_hostc.so')
P, I = ctypes.c_void_p, ctypes.c_int


def _lib():
    lib = ctypes.CDLL(str(SO))
    lib.v30a_stage.argtypes = [P, P, P, I, P, P]
    lib.v30a_stage.restype = None
    lib.v30a_gap.argtypes = [P, I, I, P, I, P, I, P]
    lib.v30a_gap.restype = I
    return lib


class Stem:
    """stem: a v30a_io.Stem. Calls return its bytes as a view of one kept buffer (valid until the next call)."""

    def __init__(self, stem):
        self.lib, self.size, self.nbytes = _lib(), stem.size, stem.nbytes
        self.dst = np.flatnonzero(stem.src != stem.size * stem.size).astype(np.int32)   # halves a pixel fills
        self.src = np.ascontiguousarray(stem.src[self.dst], np.int32)
        self.lut = np.ascontiguousarray(stem.lut)
        self.buf = np.zeros(stem.nbytes // 2, '<u2')          # the other halves stay zero
        self.bytes = memoryview(self.buf).cast('B')

    def __call__(self, crop):
        crop = np.ascontiguousarray(crop)
        if crop.shape != (self.size, self.size) or crop.dtype != np.uint8:
            raise ValueError('crop %s %s' % (crop.shape, crop.dtype))
        self.lib.v30a_stage(crop.ctypes.data, self.src.ctypes.data, self.dst.ctypes.data, len(self.dst),
                            self.lut.ctypes.data, self.buf.ctypes.data)
        return self.bytes


class Final:
    """final: a v30a_io.Final. The 128 x C heads stay in numpy, in FP64 as there."""

    def __init__(self, final):
        self.lib, self.f, self.nbytes = _lib(), final, final.nbytes
        self.inside, self.outside = final.inside.astype(np.int32), final.outside.astype(np.int32)
        self.sum = np.empty(2 * final.pairs, np.float64)

    def __call__(self, raw):
        """raw: bytes of one compact final store -> (f32 [FLOATS], nonzero halves outside the map)"""
        f = self.f
        if len(raw) != self.nbytes:
            raise ValueError('final store of %d bytes' % len(raw))
        stray = self.lib.v30a_gap(raw, f.pairs, f.nj * 32, self.inside.ctypes.data, len(self.inside),
                                  self.outside.ctypes.data, len(self.outside), self.sum.ctypes.data)
        y = f.w @ (self.sum / f.n) + f.b
        y[f.sig] = 1.0 / (1.0 + np.exp(-y[f.sig]))
        return y.astype(np.float32), stray
