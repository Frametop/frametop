"""Fixed-shape resident GAP + FP32 four-head epilogue; opens no device."""

from dataclasses import replace

import teacher_resident_layout as R
import teacher_tail_layout as T

KIND, PARAM_VECTORS = 6, 2692
ORDER = ("out0", "out3", "out2", "out1")
SLICES = {
    "out0": slice(0, 63),
    "out3": slice(63, 126),
    "out2": slice(126, 127),
    "out1": slice(127, 128),
}
# Frozen before device qualification; incremental error against host heads of
# the same FP16 tail tensor, in addition to the existing ncnn head ceilings.
CAPS = {"pool": 2e-5, "out0": 0.01, "out3": 1e-4, "out2": 2e-5, "out1": 2e-5}


def parameters(graph, specs):
    import numpy as np
    from framenpu.landmark import weights_and_bias
    from framenpu.resident_landmark import PARAM_SHA, WEIGHTS_SHA

    if (graph.param_sha256, graph.weights_sha256) != (PARAM_SHA, WEIGHTS_SHA):
        raise ValueError("heads require the pinned public teacher")
    matrix, bias = np.zeros((672, 128), "<f4"), np.zeros(128, "<f4")
    found = set()
    for s in specs:
        if s["operator"] != "InnerProduct":
            continue
        name = s["output"][0]
        span = SLICES[name]
        if tuple(s["input_shape"]) != (672,) or tuple(s["output_shape"]) != (
            span.stop - span.start,
        ):
            raise ValueError("head shape differs")
        w, b = weights_and_bias(graph, s)
        matrix[:, span] = np.asarray(w, "<f4").reshape(-1, 672).T
        bias[span] = np.asarray(b, "<f4")
        found.add(name)
    if found != set(ORDER):
        raise ValueError("four heads required")
    return bias.tobytes() + matrix.tobytes()


def build_plan(graph, specs, *, observe=False):
    import numpy as np

    base = R.build_plan(graph, specs, "merged-observe" if observe else "merged")
    rows = np.frombuffer(base.table, "<u4").reshape(-1, 32)
    assert rows[-1, 0] == R.KIND["store"] and rows[-1, 10] == 2352
    vin = int(rows[-1, 14])
    vt = R._Vtcm()
    vt.free = [(R.VTCM_FIRST, vin - R.VTCM_FIRST), (vin + 2352, R.SPAN - vin - 2352)]
    vt.free = [(a, n) for a, n in vt.free if n]
    vpar, pool, out = vt.take(PARAM_VECTORS), vt.take(21), vt.take(4)
    dpar = R._al(base.end)
    pooled_at = dpar + PARAM_VECTORS * 128
    heads_at = pooled_at + (21 * 128 if observe else 0)
    end = heads_at + 512
    if end > T.WIN_HI:
        raise ValueError("heads exceed owned window")
    desc = R._words(KIND, 336, 4, 672, 49, 7, 1, 7, 9, dpar, PARAM_VECTORS, vin, out, pool, vpar)
    table = base.table if observe else base.table[:-128]
    table += desc
    if observe:
        table += R._words(4, 0, 0, 0, 0, 0, 0, 0, 0, pooled_at, 21, 0, 0, 0, pool)
    table += R._words(4, 0, 0, 0, 0, 0, 0, 0, 0, heads_at, 4, 0, 0, 0, out)
    job = replace(
        base.jobs[0],
        end=len(table) // 128,
        ops=base.jobs[0].ops + ["gap_heads"],
        stores=base.jobs[0].stores if observe else [],
        vtcm_peak=max(base.jobs[0].vtcm_peak, vt.peak),
    )
    return replace(
        base,
        mode="heads-observe" if observe else "heads",
        jobs=[job],
        table=table,
        fixed=base.fixed | {dpar: parameters(graph, specs)},
        end=end,
        where=base.where | {"pooled": pooled_at, "heads": heads_at},
    )


def native_ok(row):
    r = [int(x) for x in row]
    if len(r) != 32 or r[:9] != [6, 336, 4, 672, 49, 7, 1, 7, 9] or r[10] != PARAM_VECTORS:
        return False
    if not T.WIN_LO <= r[9] <= T.WIN_HI or r[9] % 128 or PARAM_VECTORS > (T.WIN_HI - r[9]) // 128:
        return False
    if r[15] > 2 or any(r[16:]):
        return False
    lengths = [2352, 4, 21, PARAM_VECTORS]
    for i, n in enumerate(lengths):
        at = r[11 + i]
        if not 0 < at <= R.SPAN or n > R.SPAN - at:
            return False
        if any(at < r[11 + j] + lengths[j] and r[11 + j] < at + n for j in range(i)):
            return False
    return True


def split(values):
    return {name: values[span] for name, span in SLICES.items()}


def pool_reference(last):
    import numpy as np

    return (
        np.asarray(last, np.float32).reshape(672, 49).mean(1, dtype=np.float64).astype(np.float32)
    )


def simulate_op(row, vt):
    """FP32 contract model; QFloat32 hardware differences are qualified separately."""
    import numpy as np

    if not native_ok(row):
        raise ValueError("invalid heads descriptor")
    r = [int(x) for x in row]
    sf = vt.view("<f4").reshape(-1, 32)
    grid = vt[r[11] : r[11] + 2352].reshape(336, 224, 2)
    mean = np.zeros(672, np.float32)
    for y in range(7):
        for x in range(7):
            mean = (mean + grid[:, 34 + y * 9 + x].reshape(672).astype(np.float32)).astype(
                np.float32
            )
    mean = (mean * np.float32(1 / 49)).astype(np.float32)
    sf[r[13] : r[13] + 21] = mean.reshape(21, 32)
    if r[15] == 1:
        raise RuntimeError("diagnostic fault after GAP")
    par = sf[r[14] : r[14] + PARAM_VECTORS].reshape(-1)
    out = par[:128].copy()
    weights = par[128:].reshape(672, 128)
    for ch in range(672):
        out = (out + (weights[ch] * mean[ch]).astype(np.float32)).astype(np.float32)
    out[126:] = (1 / (1 + np.exp(-out[126:].astype(np.float64)))).astype(np.float32)
    sf[r[12] : r[12] + 4] = out.reshape(4, 32)
    if r[15] == 2:
        raise RuntimeError("diagnostic fault after heads")
