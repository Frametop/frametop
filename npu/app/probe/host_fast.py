"""The flat H-net's host side with precomputed index tables instead of per-crop layout code. Still numpy.

serve_path.py's first version did what the probes do: unshuffle the crop to float32, let framenpu's layout
code build the window bytes (stage_inputs), read the head tensor back through read_compact (float64 planes
and a padding check), then run the heads one crop at a time. Those three cost 0.8 ms a crop at 160. Here:

  stage  one table lookup (u8 -> FP16 bits of v / 255) and one scatter straight from crop pixels to the
         window's halves; the padding words stay zero in a kept buffer
  read   one gather from the stored bytes to [C, n, n], no padding check
  heads  every crop of the set in one batched pass

The tables are derived from framenpu's own layout code (grid index for the stage; the read table by pushing
position codes through read_compact), and `python3 host_fast.py` checks all three against the slow path and
times both. No device.
"""
import sys
import time

import numpy as np

import hnet_plan as H
import hnet_real as HR
from synth_plan import C, L, X

LUT = (np.arange(256, dtype=np.float32) * (1 / 255.0)).astype('<f2').view('<u2')


class Fast:
    def __init__(self, model, mp, z):
        self.mp, self.z = mp, z
        c, q, _ = model.shape[X]
        s = 4 * q
        g = mp.grid(X)
        idx, _ = C._grid_index(g.w, g.h)                       # window word of every (y, x), row-major
        ch = np.arange(c)[:, None]
        p = np.arange(q * q)[None, :]
        y, x = p // q, p % q
        # channel dy * 4 + dx of the unshuffle is pixel (4y + dy, 4x + dx); channel 2i + j is half j of pair i
        self.src = ((4 * y + ch // 4) * s + 4 * x + ch % 4).reshape(-1)
        self.dst = (((ch // 2) * g.nbin * 32 + idx[p]) * 2 + ch % 2).reshape(-1)
        self.buf = np.zeros(c // 2 * g.nbin * 64, '<u2')
        self.final = mp.final
        fc = model.shape[mp.final][0]
        fg = mp.grid(mp.final)
        self.nraw = fc // 2 * fg.nj * 64                        # halves of a compact store
        codes = np.arange(self.nraw)
        lo = self._through(codes % 1024 + 1)
        hi = self._through(codes // 1024 + 1)
        self.ridx = ((lo - 1) + 1024 * (hi - 1)).astype(np.intp)   # [C, n, n] -> half of the stored bytes
        crops = [mp.crop(k) for k in range(len(mp.crops) or 1)]
        self.x_at, self.o_at = [c.where[X] for c in crops], [c.where[mp.final] for c in crops]
        n = model.shape[mp.final][1]
        grid = np.asarray(z['grid'], np.float32)
        self.gx = np.ascontiguousarray(grid.reshape(n, 2).T).reshape(-1)          # [b, xc] -> grid[2 xc + b]
        self.w0t, self.w2t = np.ascontiguousarray(z['world0.w'].T), np.ascontiguousarray(z['world2.w'].T)

    def _through(self, values):
        raw = values.astype('<f2').tobytes()
        got, _ = L.read_compact(self.mp.crop(0), lambda at, n: raw[:n], self.final)   # codes in the padding: ignored
        return np.rint(got).astype(np.int64)

    def stage(self, put, k, img):
        """crop k's window slot <- a u8 [s, s] crop"""
        self.buf[self.dst] = LUT[img.reshape(-1)[self.src]]
        put(self.x_at[k], self.buf.tobytes())

    def read(self, get, m):
        """[m, C, n, n] float32: the head tensors of crops 0..m-1"""
        out = np.empty((m,) + self.ridx.shape, np.float32)
        for k in range(m):
            raw = np.frombuffer(get(self.o_at[k], 2 * self.nraw), '<f2')
            out[k] = raw[self.ridx]
        return out

    def heads(self, o):
        """hnet_real.heads for a batch: screen [m, 63], presence [m], right [m], world [m, 63].
        The soft-argmax works on the four sub-pixel planes as stored (plane a * 2 + b of cell (yc, xc) is
        heatmap pixel (2 yc + a, 2 xc + b)), so the pixel shuffle is never built."""
        z = self.z
        m, _, n, _ = o.shape
        kpd, gd = int(z['kpd']), int(z['gd'])
        heat = o[:, :84].reshape(m, 21, 4 * n * n)
        e = np.exp(heat - heat.max(-1, keepdims=True))
        e *= 1.0 / e.sum(-1, keepdims=True)
        e = e.reshape(m, 21, 2, 2, n, n)                       # kp, a, b, yc, xc
        screen = np.zeros((m, 21, 3), np.float32)
        screen[..., 0] = (e.sum(2).sum(3).reshape(m, 21, 2 * n)) @ self.gx     # over a, then yc -> [b, xc]
        screen[..., 1] = (e.sum(3).sum(4).reshape(m, 21, 2 * n)) @ self.gx     # over b, then xc -> [a, yc]
        h2 = e.reshape(m, 21, 4, n * n).sum(2)
        k = (h2 @ o[:, 84: 84 + kpd].reshape(m, kpd, n * n).transpose(0, 2, 1)).reshape(m, 21 * kpd)
        g = o[:, 84 + kpd: 84 + kpd + gd].reshape(m, gd, n * n).mean(2)
        hid = np.maximum(np.concatenate([g, k], 1) @ self.w0t + z['world0.b'], 0.0)
        sig = lambda v: 1.0 / (1.0 + np.exp(-v))  # noqa: E731
        return (screen.reshape(m, 63), sig(g @ z['pres.w'].T + z['pres.b'])[:, 0],
                sig(g @ z['right.w'].T + z['right.b'])[:, 0], hid @ self.w2t + z['world2.b'])


def _med(f, n=60):
    t = []
    for _ in range(n):
        t0 = time.perf_counter()
        f()
        t.append((time.perf_counter() - t0) * 1e3)
    return float(np.median(t))


if __name__ == '__main__':
    import serve_path as SP
    rng = np.random.default_rng(5)
    for arch in sys.argv[1:] or ('hx_f160', 'hx_f'):
        model = H.graph(arch)
        C.MAX_CROPS = 4                                         # plans only; no device here
        mp = H.plan(model, 4)
        z = SP.zero_heads(model)
        for key in z:                                           # random head weights: the comparison must see them
            if key.endswith('.w') or key.endswith('.b'):
                z[key] = rng.standard_normal(z[key].shape).astype(np.float32) * 0.1
        f = Fast(model, mp, z)
        s = 4 * model.shape[X][1]
        img = rng.integers(0, 256, (s, s), dtype=np.uint8)
        img[0, :256 if s >= 256 else s] = np.arange(256)[:s]    # every u8 value at least once
        slow, fast = {}, {}
        L.stage_inputs(mp.crop(1), lambda at, d: slow.update({at: bytes(d)}), {X: SP.unshuffle(img)})
        f.stage(lambda at, d: fast.update({at: bytes(d)}), 1, img)
        c, n, _ = model.shape[mp.final]
        raw = rng.standard_normal(f.nraw).astype('<f2').tobytes()
        want, _ = L.read_compact(mp.crop(0), lambda at, k: raw[:k], mp.final)
        got = f.read(lambda at, k: raw[:k], 2)
        o = rng.standard_normal((3, c, n, n)).astype(np.float32)
        a, b = [HR.heads(x, z) for x in o], f.heads(o)
        err = max(float(np.abs(np.asarray(a[i][j]).reshape(-1) - np.asarray(b[j][i]).reshape(-1)).max())
                  for i in range(3) for j in range(4))
        print('%s: stage bytes equal %s, read equal %s, heads max difference %.1e' % (
            arch, slow == fast, bool(np.array_equal(want.astype(np.float32), got[0])), err))
        print('   a crop, slow -> fast: stage %.3f -> %.3f ms, read %.3f -> %.3f ms, heads %.3f -> %.3f ms' % (
            _med(lambda: L.stage_inputs(mp.crop(0), lambda at, d: None, {X: SP.unshuffle(img)})),
            _med(lambda: f.stage(lambda at, d: None, 0, img)),
            _med(lambda: L.read_compact(mp.crop(0), lambda at, k: raw[:k], mp.final)),
            _med(lambda: f.read(lambda at, k: raw[:k], 4)) / 4,
            _med(lambda: HR.heads(o[0], z)), _med(lambda: f.heads(np.concatenate([o, o[:1]]))) / 4))
