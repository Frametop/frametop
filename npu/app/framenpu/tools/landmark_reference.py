"""Independent CPU precision model used only by optional graph qualification.

NumPy is deliberately optional and never imported by the library. HMX uses
FP64 dot products as an independent precision estimate, followed by its explicit
FP16 store and FP32 bias. Depthwise uses ordered, separately rounded FP32 ops.
The pinned ncnn export remains the full-precision graph oracle.
"""

import math


def evaluate(graph, spec, inputs, *, hmx=True, logits=False, condition=False):
    import numpy as np
    from framenpu.landmark import weights_and_bias

    s = spec
    op = s["operator"]
    if op == "Input":
        raise ValueError("input is supplied externally")
    source = np.asarray(inputs[0], dtype=np.float32).reshape(s["input_shape"])
    if condition:
        source = np.abs(source)
    if op in ("Split", "Reshape", "Squeeze"):
        return source.reshape(s["output_shape"])
    if op == "BinaryOp":
        other = np.asarray(inputs[1], dtype=np.float32).reshape(source.shape)
        if condition:
            other = np.abs(other)
        return (source + other).astype(np.float32)
    if op in ("Convolution", "ConvolutionDepthWise", "InnerProduct"):
        w, bias = weights_and_bias(graph, s)
        bias = np.asarray(bias, dtype=np.float32)
        if condition:
            w = np.abs(w)
            bias = np.abs(bias)
        n = s["output_shape"][0]
        if op == "InnerProduct":
            patch = source.reshape(-1, 1)
        else:
            c, h, width = s["input_shape"]
            _, oh, ow = s["output_shape"]
            kh, kw = s["kernel"]
            sh, sw = s["stride"]
            dh, dw = s["dilation"]
            pt, pb, pl, pr = s["padding"]
            padded = np.pad(source, ((0, 0), (pt, pb), (pl, pr)))
            patch = np.stack(
                [
                    padded[:, ky * dh : ky * dh + oh * sh : sh, kx * dw : kx * dw + ow * sw : sw]
                    for ky in range(kh)
                    for kx in range(kw)
                ],
                axis=1,
            )
            patch = patch.reshape(c * kh * kw, oh * ow)
        if op == "ConvolutionDepthWise":
            taps = math.prod(s["kernel"])
            patch = patch.reshape(n, taps, -1)
            weights = np.asarray(w, dtype=np.float32).reshape(n, taps)
            value = np.broadcast_to(bias[:, None], (n, patch.shape[-1])).copy()
            for t in range(taps):
                value = (value + (patch[:, t] * weights[:, t, None]).astype(np.float32)).astype(
                    np.float32
                )
        else:
            weights = np.asarray(w, dtype=np.float64).reshape(n, -1)
            operand = (
                patch.astype(np.float16).astype(np.float64) if hmx else patch.astype(np.float64)
            )
            value = weights @ operand
            if hmx:
                value = value.astype(np.float16).astype(np.float32)
            value = (value + bias[:, None]).astype(np.float32)
        if condition:
            return value.reshape(s["output_shape"])
        if s["activation"] == "clip":
            value = np.clip(value, 0, 6)
        elif s["activation"] == "sigmoid" and not logits:
            value = (1 / (1 + np.exp(-value.astype(np.float64)))).astype(np.float32)
        return value.reshape(s["output_shape"])
    if op == "Pooling":
        p = s["params"]
        if p.get(4, 0):
            flat = source.reshape(source.shape[0], -1)
            value = np.zeros(flat.shape[0], dtype=np.float32)
            for t in range(flat.shape[1]):
                value = (value + flat[:, t]).astype(np.float32)
            return (value * np.float32(1 / flat.shape[1])).reshape(s["output_shape"])
        kw = p[1]
        kh = p.get(11, kw)
        sw = p.get(2, 1)
        sh = p.get(12, sw)
        _, oh, ow = s["output_shape"]
        patches = [
            source[:, y : y + oh * sh : sh, x : x + ow * sw : sw]
            for y in range(kh)
            for x in range(kw)
        ]
        return np.maximum.reduce(patches)
    raise ValueError("unsupported reference operator " + op)


def errors(actual, reference):
    import numpy as np

    error = (
        np.asarray(actual, dtype=np.float64).ravel()
        - np.asarray(reference, dtype=np.float64).ravel()
    )
    return {"max_abs": float(np.max(np.abs(error))), "rms": float(np.sqrt(np.mean(error * error)))}


def frozen_limits(calibration):
    """Only calibration rows may influence the frozen per-layer graph caps."""
    limits = {}
    for row in calibration:
        if row["split"] != "calibration":
            continue
        candidate = {
            "max_abs": max(0.002, 2 * row["error"]["max_abs"] + 0.002 * row["reference_max_abs"]),
            "rms": max(0.0002, 2 * row["error"]["rms"] + 0.0002 * row["reference_rms"]),
        }
        target = limits.setdefault(row["layer"], {"max_abs": 0.0, "rms": 0.0})
        for key in target:
            target[key] = max(target[key], candidate[key])
    return limits
