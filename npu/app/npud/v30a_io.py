"""ft-npud's v30a mode, host side: one crop's stem columns as the window bytes the plan reads, and one crop's
final map (compact store) to v30a's four outputs (GAP + heads). No DSP here, so it can be tested on the CPU
against the spike code it replaces on the hot path (test_v30a_io.py: v30a_layout.stem_columns and tensor_bytes,
read_compact and v30a_graph.finish).

Outputs, per crop, f32 [128] (heads_file.FLOAT_OUTS): out0 [63] (x, y, z of 21 keypoints in crop pixels), out1
presence, out2 side (right), out3 world [63]; out1 and out2 after the sigmoid. The same numbers trackd reads
from ncnn's out0..out3 (nets.cpp Nets::landmarks)."""
import numpy as np

SIZE = 224
HEAD_ORDER = ('out0', 'out1', 'out2', 'out3')
FLOATS = 63 + 1 + 1 + 63


class Stem:
    """crop (SIZE x SIZE u8) -> the stem columns' window bytes: plane 3 dy + dx holds the FP16 image (v / 255)
    at (2y + dy, 2x + dx), zero past the edge; plane 9 zero; laid out as the grid's channel-pair words
    (v30a_layout.stem_columns, then framenpu Grid.to_bytes). One gather per crop through a precomputed index."""

    def __init__(self, grid, planes, size=SIZE):
        if planes % 2:
            raise ValueError('stem planes come in pairs')
        h = size // 2
        if (grid.h, grid.w) != (h, h):
            raise ValueError('stem grid %dx%d for a %d crop' % (grid.h, grid.w, size))
        self.size, self.nbytes = size, planes // 2 * grid.nbin * 128
        sentinel = size * size   # an always-zero pixel: past the edge, padding words, plane 9
        src = np.full((planes // 2, grid.nbin * 32, 2), sentinel, np.int32)
        y, x = np.mgrid[0:h, 0:h]
        word = (grid.word(0, 0) + y * grid.ws + x).ravel()
        for p in range(min(planes, 9)):
            dy, dx = divmod(p, 3)
            yy, xx = (2 * y + dy).ravel(), (2 * x + dx).ravel()
            ok = (yy < size) & (xx < size)
            src[p // 2, word[ok], p % 2] = yy[ok] * size + xx[ok]
        self.src = src.ravel()
        # u8 -> FP16 bits, as v30a_graph.normalize then the f16 cast: float32(v) / 255, rounded to nearest
        self.lut = (np.arange(256, dtype=np.float32) / np.float32(255)).astype('<f2').view('<u2')
        self.ext = np.zeros(sentinel + 1, '<u2')

    def __call__(self, crop):
        crop = np.asarray(crop)
        if crop.shape != (self.size, self.size) or crop.dtype != np.uint8:
            raise ValueError('crop %s %s' % (crop.shape, crop.dtype))
        self.ext[:-1] = self.lut[crop.ravel()]
        return self.ext[self.src].tobytes()


class Final:
    """one crop's compact final store (pairs x nj vectors from blk0, k/ kind 4 with nj) -> v30a's outputs.
    heads: v30a_graph.V30a.heads (name -> (weight [n, C], bias [n], sigmoid))."""

    def __init__(self, grid, channels, heads):
        self.pairs, self.nj, self.n = channels // 2, grid.nj, grid.h * grid.w
        self.nbytes = self.pairs * self.nj * 128
        inside = grid.interior()[32 * grid.blk0: 32 * (grid.blk0 + grid.nj)]
        if int(inside.sum()) != self.n:
            raise ValueError('the compact blocks do not hold the whole map')
        self.inside, self.outside = np.flatnonzero(inside), np.flatnonzero(~inside)
        w = [np.asarray(heads[k][0], np.float64) for k in HEAD_ORDER]
        if [x.shape for x in w] != [(63, channels), (1, channels), (1, channels), (63, channels)]:
            raise ValueError('v30a heads: %s' % [x.shape for x in w])
        self.w = np.concatenate(w)
        self.b = np.concatenate([np.asarray(heads[k][1], np.float64) for k in HEAD_ORDER])
        self.sig = np.concatenate([np.full(len(heads[k][1]), bool(heads[k][2])) for k in HEAD_ORDER])

    def __call__(self, raw):
        """-> (f32 [FLOATS], nonzero halves outside the map). The stray count must be 0 on a sound run."""
        v = np.frombuffer(raw, '<f2', self.pairs * self.nj * 64).reshape(self.pairs, self.nj * 32, 2)
        stray = int(np.count_nonzero(v[:, self.outside]))
        # GAP: channel 2i + j is half j of pair i (framenpu unplanar), summed in FP64 as v30a_graph.finish
        gap = v[:, self.inside].astype(np.float64).sum(axis=1).ravel() / self.n
        y = self.w @ gap + self.b
        y[self.sig] = 1.0 / (1.0 + np.exp(-y[self.sig]))
        return y.astype(np.float32), stray


def is_graph(path):
    """a v30a graph file (v30a_serve.py pack): the ncnn model's two files in an npz"""
    with np.load(path) as z:
        return 'v30a.param' in z.files
