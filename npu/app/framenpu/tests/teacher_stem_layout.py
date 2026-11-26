"""Fixed 8-output-row banded RGB prefix. Planning and CPU contract, no device."""

from dataclasses import replace

import teacher_heads_layout as H
import teacher_resident_layout as R
import teacher_tail_layout as T

KIND, BLOCK, PARAMS = 7, 672, 4704
RGB, OUT8, OUT10, A, B, PAR = 4, 2356, 5972, 6880, 9728, 12576
BAND = T.Grid(112, 20)
OUTPUT = T.Grid(56, 56)
NAMES = ("convclip_0", "convdwclip_0", "conv_10", "convclip_1", "convdwclip_1")
OFFSETS = (0, 112, 224, 336, 512)
LENGTHS = (112, 100, 112, 176, 145)
CHANNELS = (24, 24, 16, 64, 64)


def stem_weights(graph, spec):
    import numpy as np
    from framenpu.landmark import weights_and_bias
    from hmx_block_layout import pack

    w, bias = weights_and_bias(graph, spec)
    tile = np.zeros((32, 32), np.float16)
    tile[:24, :27] = np.asarray(w, np.float16).reshape(24, 27)
    return pack(tile).tobytes(), R._splat_pairs(bias, 12)


def parameters(graph, specs):
    import numpy as np
    from framenpu.resident_landmark import PARAM_SHA, WEIGHTS_SHA
    from framenpu.landmark import weights_and_bias

    if (graph.param_sha256, graph.weights_sha256) != (PARAM_SHA, WEIGHTS_SHA):
        raise ValueError("banded prefix requires the pinned teacher")
    by = {s["name"]: s for s in specs}
    result = bytearray(PARAMS * 128)
    for band in range(7):
        mask = np.frombuffer(BAND.mask_bytes(), "<u4").copy()
        pos = np.arange(len(mask))
        y = 16 * band - 1 + pos // BAND.ws
        mask[(y < 0) | (y >= 112)] = 0
        for stage, name in enumerate(NAMES):
            spec = by[name]
            _, b = weights_and_bias(graph, spec)
            bias = np.repeat(np.asarray(b, "<f4")[:, None], 32, 1).tobytes()
            if stage == 0:
                tiles, _ = stem_weights(graph, spec)
                image = tiles + bias + mask.tobytes()
                image += bytes(-len(image) % 2048)
            else:
                weights = (
                    T.dw_weights(graph, spec)[0]
                    if stage in (1, 4)
                    else T.pw_weights(graph, spec)[0]
                )
                image = weights + bias + mask.tobytes()
                if stage in (2, 3):
                    image += bytes(-len(image) % 2048)
            if len(image) != LENGTHS[stage] * 128:
                raise ValueError("prefix parameter geometry differs")
            at = (band * BLOCK + OFFSETS[stage]) * 128
            result[at : at + len(image)] = image
    return bytes(result)


def build_plan(graph, specs, *, grayscale=False):
    """RGB reuses the tail's unused final-map slot; prefix writes tail inputs."""
    tail = H.build_plan(graph, specs)
    at = tail.table_at + len(tail.table)
    end = R._al(tail.end) + PARAMS * 128
    if end > T.WIN_HI or at + 128 > min(tail.fixed):
        raise ValueError("prefix exceeds owned DDR window")
    words = [
        8 if grayscale else KIND,
        0,
        7,
        8,
        20,
        tail.where[R.FINAL],
        R._al(tail.end),
        PARAMS,
        BLOCK,
        tail.where["8"],
        tail.where["10"],
        RGB,
        OUT8,
        OUT10,
        A,
    ]
    table = R._words(*words)
    job = R.Job("rs", end=1, ops=["banded_prefix"], stores=["8", "10"], vtcm_peak=13009)
    prefix = replace(
        tail,
        mode="stem",
        jobs=[job],
        table=table,
        table_at=at,
        fixed={words[6]: parameters(graph, specs)},
        where=tail.where | {"in0": words[5]},
        end=end,
    )
    if not native_ok(words + [0] * 17, grayscale=grayscale):
        raise ValueError("invalid prefix layout")
    return prefix, tail


def native_ok(row, *, grayscale=False):
    r = [int(x) for x in row]
    if len(r) != 32 or r[0] != (8 if grayscale else KIND) or not 0 <= r[1] < r[2] <= 7:
        return False
    if r[3:5] != [8, 20] or r[7:9] != [PARAMS, BLOCK] or r[11:15] != [RGB, OUT8, OUT10, A]:
        return False
    if not 0 <= r[15] <= 6 or not 0 <= r[16] <= 5 or any(r[18:]):
        return False
    spans = [(r[5], 784 if grayscale else 2352), (r[6], PARAMS), (r[9], 3616), (r[10], 904)]
    if r[16]:
        spans.append((r[17], CHANNELS[r[16] - 1] // 2 * BAND.nbin))
    elif r[17]:
        return False
    for i, (at, n) in enumerate(spans):
        if not T.WIN_LO <= at <= T.WIN_HI or at % 128 or n > (T.WIN_HI - at) // 128:
            return False
        if any(at < b + m * 128 and b < at + n * 128 for b, m in spans[:i]):
            return False
    return True


def stem_patches(rgb, rows):
    """TF-SAME 3x3/s2, with padding only on the bottom and right."""
    import numpy as np

    patch = np.zeros((27, len(rows), 112), np.float64)
    pad = np.pad(T.f16(rgb), ((0, 0), (0, 1), (0, 1)))
    for j, y in enumerate(rows):
        if not 0 <= y < 112:
            continue
        for ch in range(3):
            for dy in range(3):
                for dx in range(3):
                    patch[ch * 9 + dy * 3 + dx, j] = pad[ch, 2 * y + dy, dx : 224 + dx : 2]
    return patch


def emulate_stem(graph, spec, rgb, rows=range(112)):
    import numpy as np
    from framenpu.landmark import weights_and_bias

    w, b = weights_and_bias(graph, spec)
    patch = stem_patches(rgb, rows)
    acc = T.f16(T.f16(w).reshape(24, 27) @ patch.reshape(27, -1))
    out = np.clip(T.f16(acc + np.asarray(b)[:, None]), 0, 6).reshape(24, len(rows), 112)
    out[:, (np.asarray(rows) < 0) | (np.asarray(rows) >= 112)] = 0
    return out


def bound_stem(graph, spec, rgb, rows):
    import numpy as np
    from framenpu.landmark import weights_and_bias

    w, b = weights_and_bias(graph, spec)
    mag = np.abs(T.f16(w)).reshape(24, 27) @ np.abs(stem_patches(rgb, rows)).reshape(27, -1)
    e = 2.0**-10
    return (7 * e * mag + 3 * e * np.abs(np.asarray(b))[:, None] + 2.0**-20).reshape(
        24, len(rows), 112
    )


def maxpool(x):
    import numpy as np

    return np.maximum(
        np.maximum(np.maximum(x[:, 0::2, 0::2], x[:, 0::2, 1::2]), x[:, 1::2, 0::2]),
        x[:, 1::2, 1::2],
    )


def emulate_layer(graph, spec, x, *, dw=False):
    """Prefix contract: FP16 accumulation store, original FP32 bias, FP16 output."""
    import numpy as np
    from framenpu.landmark import weights_and_bias

    w, b = weights_and_bias(graph, spec)
    cin, h, wd = x.shape
    cout = spec["output_shape"][0]
    if dw:
        wt = T.f16(w).reshape(cin, 3, 3).astype(np.float32)
        pad = np.pad(T.f16(x).astype(np.float32), ((0, 0), (1, 1), (1, 1)))
        acc = np.zeros((cin, h, wd), np.float32)
        for dy in range(3):
            for dx in range(3):
                acc = acc + (wt[:, dy, dx, None, None] * pad[:, dy : dy + h, dx : dx + wd]).astype(
                    np.float32
                )
    else:
        acc = (T.f16(w).reshape(cout, cin) @ T.f16(x).reshape(cin, -1)).reshape(cout, h, wd)
    out = T.f16(T.f16(acc) + np.asarray(b)[:, None, None])
    return np.clip(out, 0, 6) if spec["activation"] == "clip" else out


def emulate(graph, specs, rgb, *, band=None):
    """Full-image oracle, or one band with masks after each biased stage."""
    import numpy as np

    by = {s["name"]: s for s in specs}
    rows = range(112) if band is None else range(16 * band - 1, 16 * band + 19)
    valid = (np.asarray(rows) >= 0) & (np.asarray(rows) < 112)
    stage = [emulate_stem(graph, by[NAMES[0]], rgb, rows)]
    for i, name in enumerate(NAMES[1:], 1):
        spec = by[name]
        if i in (1, 4):
            spec = spec | {"stride": (1, 1)}
            x = emulate_layer(graph, spec, stage[-1], dw=True)
        else:
            x = emulate_layer(graph, spec, stage[-1])
        x[:, ~valid] = 0
        stage.append(x)
    if band is None:
        outputs = {"8": stage[4][:, 1::2, 1::2], "10": maxpool(stage[2])}
    else:
        outputs = {"8": stage[4][:, 2:18:2, 1::2], "10": maxpool(stage[2][:, 1:17])}
    return stage, outputs
