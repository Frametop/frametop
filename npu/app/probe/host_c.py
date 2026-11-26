"""host_fast.Fast with the compute in C (hostc.c, through ctypes): the scatter of a crop into the window's
halves, and the gather, soft-argmax and heads of each crop's head tensor. The device copies stay with the
caller's put and get. `python3 host_c.py` builds work/hostc.so (gcc), checks it against the numpy path and
times both; build it as the user before a device block, which runs as root and only loads it.
"""
import ctypes
import subprocess
import sys
from pathlib import Path

import numpy as np

import host_fast

HERE = Path(__file__).resolve().parent
SO = HERE.parent / 'work/hostc.so'
P = ctypes.c_void_p


def build():
    SO.parent.mkdir(exist_ok=True)
    subprocess.run(['gcc', '-O3', '-ffast-math', '-march=native', '-shared', '-fPIC', '-o', str(SO),
                    str(HERE / 'hostc.c'), '-lm'], check=True)


class CHost(host_fast.Fast):
    def __init__(self, model, mp, z):
        super().__init__(model, mp, z)
        lib = ctypes.CDLL(str(SO))
        lib.hc_stage.argtypes = [P, P, P, ctypes.c_int, P, P]
        lib.hc_stage.restype = None
        lib.hc_heads.argtypes = [P, P] + [ctypes.c_int] * 5 + [P] * 6 + [ctypes.c_float, P, ctypes.c_float, P, P, P]
        lib.hc_heads.restype = None
        self.lib = lib
        c, n, _ = self.ridx.shape
        kpd, gd = int(z['kpd']), int(z['gd'])
        f32 = lambda a: np.ascontiguousarray(a, np.float32)  # noqa: E731
        self.keep = k = {
            'src': np.ascontiguousarray(self.src, np.int32), 'dst': np.ascontiguousarray(self.dst, np.int32),
            'lut': np.ascontiguousarray(host_fast.LUT), 'ridx': np.ascontiguousarray(self.ridx.reshape(-1), np.int32),
            'grid': f32(z['grid']), 'w0': f32(z['world0.w']), 'b0': f32(z['world0.b']), 'w2': f32(z['world2.w']),
            'b2': f32(z['world2.b']), 'wp': f32(z['pres.w']).reshape(-1), 'wr': f32(z['right.w']).reshape(-1),
            'o': np.empty(c * n * n, np.float32), 'feat': np.empty(gd + 21 * kpd + 256 + n * n, np.float32)}
        if k['w0'].shape != (256, gd + 21 * kpd) or k['w2'].shape != (63, 256):
            raise ValueError('world head shapes %s %s' % (k['w0'].shape, k['w2'].shape))
        a = {key: v.ctypes.data for key, v in k.items()}
        self.n_stage = len(k['src'])
        self.bytes = memoryview(self.buf).cast('B')     # the kept window buffer, as put wants it
        self.stage_args = (a['src'], a['dst'], self.n_stage, a['lut'], self.buf.ctypes.data)
        self.head_args = (a['ridx'], c, n, kpd, gd, 256, a['grid'], a['w0'], a['b0'], a['w2'], a['b2'], a['wp'],
                          float(np.asarray(z['pres.b']).reshape(-1)[0]), a['wr'],
                          float(np.asarray(z['right.b']).reshape(-1)[0]), a['o'], a['feat'])

    def stage(self, put, k, img):
        img = np.ascontiguousarray(img)
        self.lib.hc_stage(img.ctypes.data, *self.stage_args)
        put(self.x_at[k], self.bytes)

    def outputs(self, get, m):
        """[m, 128] float32: 63 screen, presence, right, 63 world of crops 0..m-1"""
        out = np.empty((m, 128), np.float32)
        for k in range(m):
            raw = get(self.o_at[k], 2 * self.nraw)
            self.lib.hc_heads(raw if isinstance(raw, bytes) else bytes(raw), *self.head_args, out[k].ctypes.data)
        return out


if __name__ == '__main__':
    import hnet_plan as H
    import serve_path as SP
    from synth_plan import C, X
    build()
    rng = np.random.default_rng(5)
    for arch in sys.argv[1:] or ('hx_f160', 'hx_f'):
        model = H.graph(arch)
        C.MAX_CROPS = 4                                         # plans only; no device here
        mp = H.plan(model, 4)
        z = SP.zero_heads(model)
        for key in z:
            if key.endswith('.w') or key.endswith('.b'):
                z[key] = rng.standard_normal(z[key].shape).astype(np.float32) * 0.1
        f, c = host_fast.Fast(model, mp, z), CHost(model, mp, z)
        s = 4 * model.shape[X][1]
        img = rng.integers(0, 256, (s, s), dtype=np.uint8)
        a, b = {}, {}
        f.stage(lambda at, d: a.update({at: bytes(d)}), 1, img)
        c.stage(lambda at, d: b.update({at: bytes(d)}), 1, img)
        raw = (rng.standard_normal(f.nraw) * 4).astype('<f2').tobytes()
        get = lambda at, k: raw[:k]  # noqa: E731
        want = f.heads(f.read(get, 4))
        got = c.outputs(get, 4)
        err = [float(np.abs(got[:, :63] - want[0]).max()), float(np.abs(got[:, 63] - want[1]).max()),
               float(np.abs(got[:, 64] - want[2]).max()), float(np.abs(got[:, 65:] - want[3]).max())]
        print('%s: stage bytes equal %s; C against numpy, max difference: screen %.1e px, presence %.1e, right %.1e, '
              'world %.1e' % (arch, a == b, *err))
        print('   a crop, numpy -> C: stage %.3f -> %.3f ms, read and heads %.3f -> %.3f ms' % (
            host_fast._med(lambda: f.stage(lambda at, d: None, 0, img)), host_fast._med(lambda: c.stage(lambda at, d: None, 0, img)),
            host_fast._med(lambda: f.heads(f.read(get, 4))) / 4, host_fast._med(lambda: c.outputs(get, 4)) / 4))
