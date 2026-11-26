"""hnet_real.py FOLDED.npz: a trained flat H-net (train/fold.py) as a device graph, and its CPU heads in numpy.

Run on the host, it checks the pieces without a device: the FP16 host reference of the device graph plus
these heads against the float model's outputs stored in the npz. synth_device.py --real runs the same
comparison with the DSP producing the head tensor.
"""
import sys

import numpy as np

import hnet_plan as H


def load(path):
    z = dict(np.load(path, allow_pickle=False))
    arch = str(z['arch'])
    w = {str(n): (z['w.' + str(n)], z['b.' + str(n)]) for n in z['names']}
    return z, H.graph(arch, weights=w)


def heads(o, z):
    """The head tensor (84 + kpd + gd channels at stride 16) -> screen (63), presence, right, world (63)."""
    o = np.asarray(o, np.float32)
    kpd, gd, n = int(z['kpd']), int(z['gd']), o.shape[-1]
    heat = o[:84].reshape(21, 2, 2, n, n).transpose(0, 3, 1, 4, 2).reshape(21, 2 * n, 2 * n)   # pixel shuffle
    e = np.exp(heat.reshape(21, -1) - heat.reshape(21, -1).max(1, keepdims=True))
    h = (e / e.sum(1, keepdims=True)).reshape(21, 2 * n, 2 * n)
    grid = z['grid']
    px, py = (h.sum(1) * grid).sum(-1), (h.sum(2) * grid).sum(-1)
    screen = np.stack([px, py, np.zeros_like(px)], -1).reshape(63)
    h2 = h.reshape(21, n, 2, n, 2).sum((2, 4)).reshape(21, -1)
    k = (h2 @ o[84: 84 + kpd].reshape(kpd, -1).T).reshape(-1)
    g = o[84 + kpd: 84 + kpd + gd].mean((1, 2))
    sig = lambda v: 1.0 / (1.0 + np.exp(-v))  # noqa: E731
    hid = np.maximum(z['world0.w'] @ np.concatenate([g, k]) + z['world0.b'], 0.0)
    return (screen, sig(z['pres.w'] @ g + z['pres.b']), sig(z['right.w'] @ g + z['right.b']),
            z['world2.w'] @ hid + z['world2.b'])


def compare(z, outs):
    """outs: per crop (screen, pres, right, world). Differences from the float model, and both against the labels."""
    k = len(outs)
    s = np.stack([o[0] for o in outs]).reshape(k, 21, 3)[:, :, :2]
    want = z['screen'][:k].reshape(k, 21, 3)[:, :, :2]
    d = np.linalg.norm(s - want, axis=-1)
    lab = z['kp'][:k]
    vis = z['kpw'][:k] > 0
    return {'crops': k, 'px_vs_float_median': float(np.median(d)), 'px_vs_float_max': float(d.max()),
            'presence_vs_float_max': float(max(abs(float(o[1][0]) - float(z['pres_out'][i, 0])) for i, o in enumerate(outs))),
            'world_vs_float_max_m': float(max(np.abs(o[3] - z['world'][i]).max() for i, o in enumerate(outs))),
            'px_error_labels_median_device': float(np.median(np.linalg.norm(s - lab, axis=-1)[vis])),
            'px_error_labels_median_float': float(np.median(np.linalg.norm(want - lab, axis=-1)[vis]))}


if __name__ == '__main__':
    z, model = load(sys.argv[1])
    n = min(4, len(z['x16']))
    outs = [heads(H.reference(model, z['x16'][i].astype(np.float32)), z) for i in range(n)]
    print(compare(z, outs))
    for b in (1, 3):
        mp = H.plan(model, b)
        print(f'plan x{b}: ok, {mp.descriptors} descriptors, VTCM peak {mp.vtcm_peak}')
