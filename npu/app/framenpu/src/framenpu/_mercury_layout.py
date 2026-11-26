"""Host side of the Mercury keypoint network on the resident kernel (steps 2a, 2b).

Step 2a runs ``Conv_19 .. Conv_139`` of the lowered Mercury graph (the 32x32
stage to the 160x8x8 feature map); step 2b (``prefix=True``) also runs the
64x64 prefix ``Conv_0 .. Conv_18``. Both use tests/teacher_resident_probe.c
built with RS_STRIDE2 and RS_S2_EVEN, through the linked operator image. The
42->8192 hint branch, pooling and all heads stay on the host. Opens no device.

Layout and descriptors are those of tests/teacher_resident_layout.py: every
tensor uses the shared grid of tests/teacher_tail_layout.py, 128-byte
descriptors (kinds 1 = 1x1, 2 = depthwise, 3 = load, 4 = store, 5 = stride 2)
and the same parameter images. Mercury-specific placement:

* Composite tensors are free: their parts are pair-offset views of one VTCM
  (and window) tensor. The ``Concat`` of the 64 feature channels and the
  host-staged 32-channel hint plane is one; in step 2b the 192-channel
  ``Conv_14`` output, written by six 32-channel groups, is the other.
* Step 2b bands the 64x64 expansion by channels, not rows: ``Conv_10`` and
  the stride-2 depthwise ``Conv_14`` run per 32-channel group, so the
  192x64x64 tensor (13,728 vectors) never exists; there are no halos.
* The 3x3 stride-2 stem runs on HMX as a pointwise over host im2col columns
  of the FP16 image (data movement only on the host): K=10 for Mercury's one
  channel, K=18 for the stereo student's two (stem_planes).
* Stride-2 depthwise uses the even decimation phase (symmetric padding).

Plan modes: "merged" runs every op in one request with tensors resident in
VTCM and stores only the final tensor; "qualify" runs one op per request and
stores every output, for per-layer FP16 bound checks against the device's own
inputs (composites are staged whole from the device's part bytes).
``start``/``stop_after`` truncate either mode (early-layer pilots).
"""

import struct
from dataclasses import dataclass, replace

from ._mercury_common import pack
from . import _mercury_common as RL
from . import _mercury_common as T

FIRST, LAST = "Conv_19", "Conv_139"
DSP_FIRST = "Conv_0"  # first op with prefix=True
HOST_PREFIX = "Conv_18"  # last op computed by the host before a backbone-only request
INPUT = "input.44"  # Conv_18 output, 32 x 32 x 32
STEM_COLS = "stem.cols"  # host im2col of the FP16 image: 9 taps + 1 zero plane, 64 x 64
HINT = "onnx::Concat_382"  # hint plane after the useLastKeypoints scale, 32 x 16 x 16
FEATURES = "onnx::Concat_369"  # Conv_56 (+ residual) output, 64 x 16 x 16
CONCAT = "input.144"
EXPAND = "onnx::Conv_311"  # Conv_10 output, 192 x 64 x 64 (only ever per group)
DOWN = "onnx::Conv_316"  # Conv_14 output, 192 x 32 x 32 (composite of the groups)
OUTPUT = "input.336"  # Conv_139 output, 160 x 8 x 8
HOST_HINT_OPS = ("Gemm_58", "Reshape_63", "Mul_70", "Concat_71")
GROUP = 32  # channels per Conv_10/Conv_14 group
S2_PHASE = 0  # even: symmetric (1, 1) padding
MAX_DESCRIPTORS = 160
# Step 2c (RS_MHEADS builds, heads=True): the heatmap head after LAST on the DSP.
# Both padding-2 3x3 convolutions become padding-1 kind-8 ops on a grid with a
# one-pixel zero ring (kind-7 regrid embeds the input); the 2x2 stride-2
# transposed convolution is a 1x1 to phase-major channels, then a kind-7
# pixel shuffle straight into the next ring grid. The pooled vector heads stay
# on the host, so LAST's output is stored as well.
HEAD_OPS = ("Conv_159", "ConvTranspose_163", "Conv_164")
E1 = "heads.e1"  # LAST output (160 x 8 x 8) inside a 10x10 grid with a zero ring
H1 = "heads.h1"  # Conv_159 output, 63 channels + 1 zero channel, 10 x 10
DEC = "heads.dec"  # ConvTranspose_163 as 1x1: 168 channels, phase (2a + b) major, 10 x 10
E2 = "heads.e2"  # shuffled transposed convolution (42 x 20 x 20) inside a 22x22 ring grid
HEATMAP = "heads.hm"  # Conv_164 output, 21 channels + 1 zero channel, 22 x 22
KIND_RG, KIND_P3 = 7, 8
# The hand detector (mercury.load_detector): every convolution runs on the DSP, the 3x3 stride-2
# stem as a 1x1 over host im2col columns (as the keypoint prefix); its last convolution's output
# (160 x 3 x 3) is stored, and the Flatten, three Gemm layers, Slices and Sigmoid run on the host.
DET_LAST = "Conv_162"
DET_OUTPUT = "onnx::Flatten_721"
UNDERFLOW_DW = 18 * 2.0**-14  # qf16 flush-to-zero allowance per depthwise output


def grid(shape):
    return T.Grid(shape[2], shape[1])


def _part(name, k):
    return f"{name}.g{k}"


def stem_planes(cin):
    """im2col planes of the stem: 9 taps per input channel, rounded up to an even count"""
    return 9 * cin + (9 * cin) % 2


def _prefix_ops(model):
    """Conv_0 .. Conv_18 rewritten for the resident kernel (see module docstring)."""
    import numpy as np

    stem, expand, down = model.op("Conv_0"), model.op("Conv_10"), model.op("Conv_14")
    names = [op.name for op in model.ops]
    if names[:6] != ["Conv_0", "Conv_4", "Conv_8", "Conv_10", "Conv_14", "Conv_18"]:
        raise ValueError("unexpected Mercury prefix")
    cin = stem.params["weight"].shape[1]
    if (
        stem.params["weight"].shape != (32, cin, 3, 3)
        or cin not in (1, 2)
        or stem.params["stride"] != (2, 2)
        or stem.params["pads"] != (1, 1, 1, 1)
        or stem.inputs != ("inputImg",)
        or expand.output != EXPAND
        or down.inputs != (EXPAND,)
        or down.output != DOWN
        or down.params["stride"] != (2, 2)
        or expand.shape[0] % GROUP
    ):
        raise ValueError("unexpected Mercury prefix geometry")
    w = np.zeros((32, stem_planes(cin), 1, 1), np.float32)
    w[:, : 9 * cin, 0, 0] = stem.params["weight"].reshape(32, 9 * cin)
    ops = [
        replace(
            stem,
            inputs=(STEM_COLS,),
            params=stem.params | {"weight": w, "stride": (1, 1), "pads": (0, 0, 0, 0)},
        ),
        model.op("Conv_4"),
        model.op("Conv_8"),
    ]
    for k in range(expand.shape[0] // GROUP):
        sl = slice(GROUP * k, GROUP * (k + 1))
        ops.append(
            replace(
                expand,
                name=_part("Conv_10", k),
                output=_part(EXPAND, k),
                params=expand.params
                | {"weight": expand.params["weight"][sl], "bias": expand.params["bias"][sl]},
                shape=(GROUP, *expand.shape[1:]),
            )
        )
        ops.append(
            replace(
                down,
                name=_part("Conv_14", k),
                inputs=(_part(EXPAND, k),),
                output=_part(DOWN, k),
                params=down.params
                | {
                    "weight": down.params["weight"][sl],
                    "bias": down.params["bias"][sl],
                    "group": GROUP,
                },
                shape=(GROUP, *down.shape[1:]),
            )
        )
    ops.append(model.op("Conv_18"))
    return ops


def composites(model, prefix):
    """parent -> [(part, source)]; parts are pair-major slices in order."""
    if model.detector:
        return {}
    out = {CONCAT: [(FEATURES, "device"), (HINT, "host")]}
    if prefix:
        n = model.op("Conv_10").shape[0] // GROUP
        out[DOWN] = [(_part(DOWN, k), "device") for k in range(n)]
    return out


def detector_ops(model, cut=DET_LAST):
    """The detector's device ops: Conv_0 rewritten as a 1x1 over STEM_COLS, then every
    convolution up to ``cut`` unchanged. The host runs the rest (mercury.run_tail after
    ``cut``), which may read no device tensor but the cut's output."""
    import numpy as np

    names = [op.name for op in model.ops]
    if not model.detector or DET_LAST not in names or cut not in names:
        raise ValueError("not the Mercury hand detector")
    last = names.index(cut)
    if last > names.index(DET_LAST):
        raise ValueError("the detector's device part ends at or before " + DET_LAST)
    stem, body, tail = model.ops[0], model.ops[1 : last + 1], model.ops[last + 1 :]
    if (
        stem.name != "Conv_0"
        or stem.params["weight"].shape != (32, 1, 3, 3)
        or stem.params["stride"] != (2, 2)
        or stem.params["pads"] != (1, 1, 1, 1)
        or stem.params["act"] != "relu6"
        or stem.inputs != ("inputImg",)
        or model.op(DET_LAST).output != DET_OUTPUT
        or any(op.kind != "conv" for op in body)
        or any(op.kind not in ("conv", "reshape", "dense", "slice", "sigmoid") for op in tail)
        or not {i for op in tail for i in op.inputs} <= {(body or [stem])[-1].output} | {op.output for op in tail}
    ):
        raise ValueError("unexpected Mercury detector geometry")
    w = np.zeros((32, stem_planes(1), 1, 1), np.float32)
    w[:, :9, 0, 0] = stem.params["weight"].reshape(32, 9)
    stem = replace(stem, inputs=(STEM_COLS,),
                   params=stem.params | {"weight": w, "stride": (1, 1), "pads": (0, 0, 0, 0)})
    program, shape = [stem], {op.output: op.shape for op in model.ops}
    for op in body:
        c, h, wd = shape[op.inputs[0]]
        if op.conv_class == "depthwise" and op.params["stride"] == (2, 2) and (h | wd) & 1:
            # the stride-2 kernel needs an even input (ws == 2 ow + 2): embed the odd input at
            # (0, 0) of the next even grid (kind 7 zeroes its destination), so the added row and
            # column read as the convolution's own zero padding (even phase, outputs 2y, 2x)
            even = op.inputs[0] + ".even"
            program.append(replace(op, name=op.name + ".even", kind="regrid", inputs=op.inputs,
                                   output=even, params={"mode": "embed", "at": 0},
                                   shape=(c, h + (h & 1), wd + (wd & 1))))
            op = replace(op, inputs=(even,))
        program.append(op)
    return program


def shapes(model, prefix=False, heads=False):
    shape = {op.output: op.shape for op in model.ops}
    if model.detector:
        shape[STEM_COLS] = (stem_planes(1), *model.op("Conv_0").shape[1:])
        for op in detector_ops(model):
            shape[op.output] = op.shape
        return shape
    if heads:
        for op in head_ops(model):
            shape[op.output] = op.shape
    shape[INPUT] = model.op(HOST_PREFIX).shape
    if prefix:
        shape[STEM_COLS] = (stem_planes(model.views), *model.op("Conv_0").shape[1:])
        for op in _prefix_ops(model):
            shape[op.output] = op.shape
    return shape


def head_ops(model):
    """Step 2c ops after LAST, rewritten for kinds 1, 7 and 8 (module constants)."""
    import numpy as np

    c159, dec, c164 = (model.op(n) for n in HEAD_OPS)
    c, h, w = model.op(LAST).shape
    if (
        c159.inputs != (OUTPUT,)
        or c159.params["weight"].shape != (63, c, 3, 3)
        or c159.params["pads"] != (2, 2, 2, 2)
        or c159.params["stride"] != (1, 1)
        or c159.params["act"] != "relu6"
        or c159.shape != (63, h + 2, w + 2)
        or dec.inputs != (c159.output,)
        or dec.params["weight"].shape != (63, 42, 2, 2)
        or dec.shape != (42, 2 * h + 4, 2 * w + 4)
        or c164.inputs != (dec.output,)
        or c164.params["weight"].shape != (21, 42, 3, 3)
        or c164.params["pads"] != (2, 2, 2, 2)
        or c164.params["stride"] != (1, 1)
        or c164.params["act"] != "relu6"
        or c164.shape != (21, 2 * h + 6, 2 * w + 6)
    ):
        raise ValueError("unexpected Mercury heatmap head geometry")

    def tap3(op, cin, cout, src, dst):
        """3x3 weights as a (cout, 9 * cin) matrix, K tap-major (t = 3 dy + dx)."""
        wt, b = op.params["weight"], op.params["bias"]
        co, ci = wt.shape[:2]
        m = np.zeros((cout, 9, cin), np.float32)
        m[:co, :, :ci] = wt.reshape(co, ci, 9).transpose(0, 2, 1)
        bias = np.zeros(cout, np.float32)
        bias[:co] = b
        params = dict(op.params, weight=m.reshape(cout, 9 * cin, 1, 1), bias=bias)
        params.update(pads=(0, 0, 0, 0), taps=9)
        return replace(op, inputs=(src,), output=dst, params=params, shape=(cout, *op.shape[1:]))

    wd, bd = dec.params["weight"], dec.params["bias"]  # (cin, cout, 2, 2)
    m = np.zeros((4 * 42, 64), np.float32)
    m[:, :63] = wd.transpose(2, 3, 1, 0).reshape(4 * 42, 63)  # row (2a + b) * 42 + c
    dparams = {"weight": m.reshape(168, 64, 1, 1), "bias": np.tile(bd, 4).astype(np.float32)}
    dparams.update(group=1, pads=(0, 0, 0, 0), stride=(1, 1), act=None)
    return [
        replace(c159, name="heads.embed", kind="regrid", inputs=(OUTPUT,), output=E1,
                params={"mode": "embed"}, shape=(c, h + 2, w + 2)),
        tap3(c159, c, 64, E1, H1),
        replace(dec, name=dec.name, kind="conv", inputs=(H1,), output=DEC, params=dparams,
                shape=(168, h + 2, w + 2)),
        replace(dec, name="heads.shuffle", kind="regrid", inputs=(DEC,), output=E2,
                params={"mode": "shuffle"}, shape=(42, 2 * h + 6, 2 * w + 6)),
        tap3(c164, 42, 22, E2, HEATMAP),
    ]


def regrid_words(op, gin, gout, vin, vout):
    """Kind-7 descriptor words of a head regrid op (see tests/teacher_resident_probe.c)."""
    if op.params["mode"] == "embed":
        pairs, oh, ow, at = op.shape[0] // 2, gin.h, gin.w, op.params.get("at", 1)
        return [KIND_RG, pairs, 1, oh, ow, gin.nbin, gout.nbin, vin, vout, gin.ws, gout.ws, 1,
                0, 0, gin.word(0, 0), gout.word(at, at)]
    pairs, oh, ow = op.shape[0] // 2, gin.h, gin.w  # 21 pairs per phase, 10 x 10 sources
    phases = []
    for a in (0, 1):
        for b in (0, 1):
            phases += [gin.word(0, 0), gout.word(1 + a, 1 + b)]
    return [KIND_RG, pairs, 4, oh, ow, gin.nbin, gout.nbin, vin, vout, gin.ws, 2 * gout.ws, 2,
            1, 0, *phases]


def _check(op):
    if op.kind != "conv" or op.conv_class not in ("pointwise", "depthwise"):
        raise ValueError(f"{op.name}: not a resident op")
    if op.conv_class == "depthwise":
        if op.params["weight"].shape[2:] != (3, 3) or op.params["act"] != "relu6":
            raise ValueError(f"{op.name}: resident depthwise is 3x3 + ReLU6")
        if op.params["pads"] != (1, 1, 1, 1) or len(op.inputs) != 1:
            raise ValueError(f"{op.name}: unexpected depthwise padding or residual")
    elif op.params["weight"].shape[1] % 2 or op.shape[0] % 2:
        raise ValueError(f"{op.name}: odd channel count")


def device_ops(model, stop_after=LAST, start=None, prefix=False, heads=False):
    """The resident ops of the request (prefix ops rewritten, then with heads the
    step 2c head ops), checked. For the detector: detector_ops (start/stop_after/prefix
    /heads must keep their defaults)."""
    if model.detector:
        # stop_after: the detector op the device part ends with (LAST = the default, DET_LAST)
        if start is not None or heads:
            raise ValueError("a detector plan starts at the stem and has no heads")
        program = detector_ops(model, DET_LAST if stop_after == LAST else stop_after)
        for op in program:
            if op.kind != "regrid":
                _check(op)
        return program
    names = [op.name for op in model.ops]
    lo, last = names.index(FIRST), names.index(LAST)
    skipped = [op.name for op in model.ops[lo : last + 1] if op.kind != "conv"]
    if tuple(skipped) != HOST_HINT_OPS:
        raise ValueError(f"unexpected non-conv ops in the backbone: {skipped}")
    program = (_prefix_ops(model) if prefix else []) + [
        op for op in model.ops[lo : last + 1] if op.kind == "conv"
    ]
    if heads:
        if stop_after != LAST:
            raise ValueError("the heads follow the whole network")
        for op in program:
            _check(op)
        program = program + head_ops(model)
        stop_after = program[-1].name
    order = [op.name for op in program]
    start = start or order[0]
    if start not in order or stop_after not in order:
        raise ValueError(f"start/stop_after must name resident ops in {order[0]}..{LAST}")
    a, b = order.index(start), order.index(stop_after)
    if a > b:
        raise ValueError("start after stop_after")
    for op in program[a : b + 1]:
        if op.kind != "regrid" and "taps" not in op.params and op.name not in HEAD_OPS:
            _check(op)
    return program[a : b + 1]


# ---------------------------------------------------------------- parameter images


def _bias(values, pairs, cbias):
    return RL._packed_pairs(values, pairs) if cbias else RL._splat_pairs(values, pairs)


def pw_image(op, gin, group=1, cbias=False):
    """Parameter image and VTCM scratch (vectors) of a 1x1 op; an RS_PW_GROUP build
    (group > 1) needs group * (nk + no) * 16 scratch vectors."""
    import numpy as np

    w = op.params["weight"]
    cout, cin = w.shape[:2]
    m16 = np.asarray(w.reshape(cout, cin), np.float16)
    nk, no = -(-cin // 32), -(-cout // 32)
    m = np.zeros((no * 32, nk * 32), np.float16)
    m[:cout, :cin] = m16
    tiles = b"".join(
        pack(m[32 * o : 32 * o + 32, 32 * t : 32 * t + 32]).tobytes()
        for o in range(no)
        for t in range(nk)
    )
    image = tiles + _bias(op.params["bias"], cout // 2, cbias) + gin.mask_bytes()[: gin.nj * 128]
    image += bytes(-len(image) % 2048)
    return image, (group * (nk + no) if group > 1 else nk) * 16


def dw_image(op, gin, cbias=False):
    import numpy as np

    w, b = op.params["weight"], op.params["bias"]
    c = w.shape[0]
    kk = 9
    h = np.asarray(w.reshape(c, kk), np.float16).view("<u2").astype("<u4")
    coef = np.zeros(((c // 2 * kk + 31) // 32) * 32, "<u4")
    coef[: c // 2 * kk] = (h[0::2] | (h[1::2] << 16)).reshape(-1)
    image = coef.tobytes() + _bias(b, c // 2, cbias) + gin.mask_bytes()
    return image, c // 2 * kk


# ---------------------------------------------------------------- plan


@dataclass
class MercuryPlan:
    mode: str
    plan: RL.Plan  # jobs/table/table_at as the teacher simulator and harness expect
    ops: list  # resident ops in request order
    final: str
    where: dict  # tensor -> window offset (inputs, host parts, stored tensors)
    shape: dict  # tensor -> (C, H, W)
    vtcm_peak: int
    descriptors: int
    external: list  # tensors the host stages whole into the window before the request
    host_parts: list  # composite parts the host stages into their parent's window slot
    composites: dict  # parent -> [(part, source)] for every composite of the network
    prefix: bool
    crops: tuple = ()  # batch: one window map per crop (parameter images shared)
    replay: bool = False  # RS_REPLAY: one crop's table; a request runs 1..batch crops
    resident: bool = False  # RS_RESIDENT: parameter images in the FeatureNN blob region
    res_end: int = 0  # first blob-region vector after this plan's resident images
    win_end: int = 0  # first window address after this plan
    cbias: bool = False  # RS_CBIAS: packed biases
    win_hi: int = RL.WIN_HI  # the build's window top

    def grid(self, name):
        return grid(self.shape[name])

    @property
    def batch(self):
        return len(self.crops) or 1

    def table_crops(self, crops):
        """Crops a request of this plan may carry: batch exactly, or 1..batch on replay."""
        if crops is None:
            crops = self.batch
        if not (1 <= crops <= self.batch if self.replay else crops == self.batch):
            raise ValueError(f"a request carries {'1..' if self.replay else ''}{self.batch} crops")
        return crops

    def crop(self, k):
        """The plan as seen by crop k of a batch: its own input/output window slots."""
        return replace(self, where=self.crops[k]) if self.crops else self

    @property
    def needs_hint(self):
        return HINT in self.host_parts


def build_plan(
    model,
    mode="merged",
    stop_after=LAST,
    start=None,
    prefix=False,
    batch=1,
    pw_group=1,
    replay=False,
    heads=False,
    resident=False,
    tiles=False,
    deep8=False,
    res_base=None,
    win_base=None,
    cbias=False,
    win_hi=None,
):
    """batch > 1 (merged only): one request runs the merged sequence once per crop,
    each crop with its own input and final-tensor slots; parameter images are
    shared, and VTCM is reused from crop to crop. pw_group must match the build's
    RS_PW_GROUP (it sizes the 1x1 scratch the kernel checks).

    replay (an RS_REPLAY build): batch is the most crops a request may carry
    (at most RL.MAX_CROPS) and a window slot set is reserved for each, but the table
    holds crop 0's descriptors only; the kernel runs them once per crop with
    replay_row addressing. The planner emits every crop and refuses the plan unless
    each equals crop 0 under that rule (same VTCM layout, slots a fixed stride apart).

    resident (an RS_RESIDENT build, replay + heads only): every compute op's parameter
    image gets its own permanent slot in the FeatureNN blob region (offsets RES_LO..
    RES_SPAN of the FR() span) and its scratch comes from the borrowed span; word 21
    of the descriptor is that scratch offset.

    tiles (an RS_TILES build, resident only): a tensor that only 1x1 ops read (as input
    or residual), made by a 1x1 or stride-1 depthwise op, with a pair count that is a
    multiple of 16, is kept in B-tile layout (vector (pair p, block j) at j * pairs + p,
    16-vector aligned) so the 1x1 loads its B tiles in place, with no gather. Word 15
    of a 1x1 descriptor flags its input (1), output (2) and residual (4) in that layout,
    of a depthwise one its output (2). Outputs are unchanged bit for bit.

    res_base / win_base (several models resident at once, an RS_RES_SLOTS build): the
    first resident slot (default RES_LO) and the window address of the op table
    (default WIN_LO, 4 KiB aligned), so a second model's plan lies after the first's
    in both the blob region and the window (MercuryPlan.res_end, .win_end).

    deep8 (a tiles plan for an RS_PW_DEEP, RS_PW_LG 3 build): a 1x1 whose input is a
    tile tensor, whose output is no wider (no <= nk) and which spans at least 64
    column blocks gets 32 * no * 16 scratch
    vectors, at least, and word 15 flag 8, so it runs 8-block groups two deep per
    worker. An unflagged op runs 4-block groups in the usual scratch.

    cbias (an RS_CBIAS build, resident only): biases packed one word per channel pair;
    resident images end RL.XB_N vectors below RES_SPAN. win_hi: the build's window top
    (RL.WIN_HI_WIDE for an RS_WIN_WIDE build; default RL.WIN_HI)."""
    win_hi = RL.WIN_HI if win_hi is None else win_hi
    if win_hi not in (RL.WIN_HI, RL.WIN_HI_WIDE):
        raise ValueError(f"window top {win_hi:#x}")
    if cbias and not resident:
        raise ValueError("packed biases are an option of the resident plan")
    res_top = RL.RES_SPAN - RL.XB_N if cbias else RL.RES_SPAN
    if mode not in ("merged", "qualify"):
        raise ValueError(mode)
    if batch < 1 or (batch > 1 and mode != "merged"):
        raise ValueError("batch > 1 needs a merged plan")
    if replay and (mode != "merged" or batch > RL.MAX_CROPS):
        raise ValueError(f"replay is a merged plan of at most {RL.MAX_CROPS} crops")
    if heads and (mode != "merged" or start is not None):
        raise ValueError("heads are a whole-network merged plan")
    if resident and not (replay and (heads or model.detector)):
        raise ValueError("resident parameters need a replay plan with heads")
    if tiles and not resident:
        raise ValueError("tile layout is an option of the resident plan")
    if deep8 and not (tiles and pw_group == 8):
        raise ValueError("deep8 is an option of the 8-group tiles plan")
    ops = device_ops(model, stop_after, start, prefix, heads)
    final = ops[-1].output
    shape = shapes(model, prefix, heads)
    pairs = lambda b: shape[b][0] // 2  # noqa: E731
    vec = lambda b: pairs(b) * grid(shape[b]).nbin  # noqa: E731
    comps = composites(model, prefix)
    produced = {op.output for op in ops}
    consumed = {i for op in ops for i in op.inputs}
    # parent -> {part: vector offset}; merged mode keeps a composite free when the
    # request produces one of its device parts (qualify always stages it whole).
    free = {}
    for parent, parts in comps.items():
        dev = [n for n, src in parts if src == "device"]
        made = [n for n in dev if n in produced]
        if mode != "merged" or not made:
            continue
        if parent in consumed and len(made) != len(dev):
            raise ValueError(f"{parent}: request produces only some of its parts")
        if sum(pairs(n) for n, _ in parts) != pairs(parent) or any(
            shape[n][1:] != shape[parent][1:] for n, _ in parts
        ):
            raise ValueError(f"{parent}: composite shapes differ")
        offsets, at = {}, 0
        for n, _ in parts:
            offsets[n] = at
            at += vec(n)
        free[parent] = offsets
    parent_of = {n: par for par, offs in free.items() for n in offs}
    host_parts = [n for par in free for n, src in comps[par] if src == "host"]
    made = produced | set(free) | set(host_parts)
    external = [b for b in dict.fromkeys(i for op in ops for i in op.inputs) if b not in made]
    tile = _tile_tensors(ops, shape, external, free, parent_of, host_parts, heads) if tiles else set()

    images = {}
    for op in ops:
        if op.kind == "regrid":
            continue
        gin = grid(shape[op.inputs[0]])
        images[op.name] = (
            pw_image(op, gin, pw_group, cbias) if op.conv_class == "pointwise" else dw_image(op, gin, cbias)
        )
    deep = set()
    if deep8:
        for op in ops:
            if op.kind == "regrid" or op.conv_class != "pointwise" or op.params.get("taps", 1) != 1:
                continue
            cout, cin = op.params["weight"].shape[:2]
            nk, no = -(-cin // 32), -(-cout // 32)
            # only long ops: at 34 blocks and under, 8-block groups timed 1-2.5 us slower
            # per op (fewer groups balance worse over the two workers); at 132, 4 us faster
            if op.inputs[0] in tile and no <= nk and grid(shape[op.inputs[0]]).nj >= 64:
                image, scratch = images[op.name]
                images[op.name] = (image, max(scratch, 32 * no * 16))
                deep.add(op.name)

    # window: table | parameter images | tensors (each 4 KiB aligned, never reused)
    table_at = T.WIN_LO if win_base is None else win_base
    if table_at & 4095 or not T.WIN_LO <= table_at < win_hi:
        raise ValueError(f"window base {table_at:#x}")
    cursor = RL._al(table_at + 128 * MAX_DESCRIPTORS * (len(ops) if mode == "qualify" else 1), 4096)
    par_at, fixed = {}, {}
    for op in ops:
        if op.kind == "regrid":
            continue
        par_at[op.name] = cursor
        fixed[cursor] = images[op.name][0]
        cursor = RL._al(cursor + len(images[op.name][0]))
    stored = ([OUTPUT, final] if heads else [final]) if mode == "merged" else [op.output for op in ops]
    with_host = [par for par in free if any(src == "host" for _, src in comps[par])]
    crops = []
    for _ in range(batch):
        where = {}
        for name in dict.fromkeys(external + with_host + stored):
            if name in parent_of and parent_of[name] in with_host:
                continue  # inside its parent's slot
            cursor = RL._al(cursor, 4096)
            where[name] = cursor
            cursor += vec(name) * 128
        for par in with_host:
            for n, off in free[par].items():
                where[n] = where[par] + 128 * off
        crops.append(where)
    where = crops[0]
    if cursor > win_hi:
        raise ValueError(f"window plan ends at {cursor:#x}")

    table, jobs = [], []
    peak = 0

    def words(*w):
        table.append(RL._words(*w))

    cur = dict(where)  # the crop being emitted
    res_at, res_next = {}, RL.RES_LO if res_base is None else res_base  # op name -> permanent slot
    if not RL.RES_LO <= res_next < res_top:
        raise ValueError(f"resident base {res_next}")

    def load(name, to):
        words(RL.KIND["load"], 0, 0, 0, 0, 0, 0, 0, 0, cur[name], vec(name), 0, 0, 0, to)

    def store(name, frm):
        words(RL.KIND["store"], 0, 0, 0, 0, 0, 0, 0, 0, cur[name], vec(name), 0, 0, 0, frm)

    def emit(op, at, vt):
        src = op.inputs[0]
        res = op.inputs[1] if len(op.inputs) > 1 else None
        dst = op.output
        gin, gout = grid(shape[src]), grid(shape[dst])
        if op.kind == "regrid":
            words(*regrid_words(op, gin, gout, at[src], at[dst]))
            return
        nonlocal res_next
        image, scratch = images[op.name]
        npar = len(image) // 128
        pw = op.conv_class == "pointwise"
        if resident:
            if op.name not in res_at:
                at0 = RL._al(res_next * 128, 2048 if pw else 128) // 128
                if at0 + npar > res_top:
                    raise ValueError("resident parameter images exceed the blob region")
                res_at[op.name], res_next = at0, at0 + npar
            vpar = res_at[op.name]
            scr = vt.take(scratch, align=16 if pw else 1)
            tail = (scr,)
        else:
            vpar = vt.take(npar + scratch, align=16 if pw else 1)
            tail = ()
        s2 = not pw and op.params["stride"] == (2, 2)
        tmp = vt.take(vec(src)) if s2 else None
        if pw:
            cout, cin = op.params["weight"].shape[:2]
            taps = op.params.get("taps", 1)
            words(
                KIND_P3 if taps == 9 else RL.KIND["pw"],
                cin // taps // 2,
                cout // 2,
                (cin // 2 + 15) >> 4,
                int(op.params["act"] == "relu6"),
                gin.nbin,
                gin.blk0,
                gin.nj,
                gin.ws,
                par_at[op.name],
                npar,
                at[src],
                at[dst],
                at[res] if res else 0,
                vpar,
                *(
                    (int(src in tile) | int(dst in tile) << 1 | int(res in tile) << 2
                     | int(op.name in deep) << 3,) + (0,) * 5 + tail
                    if tail
                    else ()
                ),
            )
        else:
            cp = pairs(src)
            extra = (gout.nbin, gout.blk0, gout.ws, gout.h, gout.w, tmp) if s2 else ()
            if tail:
                extra = (extra or (int(dst in tile) << 1,) + (0,) * 5) + tail
            words(
                RL.KIND["s2" if s2 else "dw"],
                cp,
                cp,
                3,
                1,
                gin.nbin,
                gin.blk0,
                gin.nj4,
                gin.ws,
                par_at[op.name],
                npar,
                at[src],
                at[dst],
                0,
                vpar,
                *extra,
            )
        if resident:
            vt.give(scr, scratch)
        else:
            vt.give(vpar, npar + scratch)
        if tmp is not None:
            vt.give(tmp, vec(src))

    if mode == "merged":
        # a part's uses are its parent's uses
        key = lambda b: parent_of.get(b, b)  # noqa: E731
        last_use = {}
        for i, op in enumerate(ops):
            for b in op.inputs:
                last_use[key(b)] = i
        vt = RL._Vtcm()
        for k in range(batch):
            cur.clear()
            cur.update(crops[k])
            _merged_crop(
                ops,
                external,
                free,
                comps,
                parent_of,
                final,
                last_use,
                key,
                vec,
                vt,
                load,
                store,
                emit,
                stored,
                tile,
            )
        job = RL.Job("rs", first=0, end=len(table), ops=[op.name for op in ops], stores=stored)
        job.vtcm_peak = peak = vt.peak
        jobs.append(job)
    else:
        for op in ops:
            first = len(table)
            vt, at = RL._Vtcm(), {}
            for b in dict.fromkeys(op.inputs):
                at[b] = vt.take(vec(b))
                load(b, at[b])
            at[op.output] = vt.take(vec(op.output))
            emit(op, at, vt)
            store(op.output, at[op.output])
            job = RL.Job("rs", first=first, end=len(table), ops=[op.name], stores=[op.output])
            job.vtcm_peak = vt.peak
            peak = max(peak, vt.peak)
            jobs.append(job)
    stride = 0
    if replay:
        per = len(table) // batch
        if batch > 1:
            stride = crops[1][final] - crops[0][final]
        for k in range(batch):
            if any(crops[k][b] - crops[0][b] != k * stride for b in crops[0]):
                raise ValueError("crop slots are not a fixed stride apart")
            for n in range(per):
                got = struct.unpack("<32I", table[k * per + n])
                want = RL.replay_row(struct.unpack("<32I", table[n]), k, stride, win_hi)
                if list(got) != want:
                    raise ValueError(f"crop {k} descriptor {n} is not crop 0's replayed")
        table = table[:per]
        jobs[0].end = per
    if any(j.end - j.first > MAX_DESCRIPTORS for j in jobs):
        raise ValueError("a request exceeds 160 descriptors")
    blob = b"".join(table)
    plan = RL.Plan(mode, jobs, blob, table_at, fixed, where, cursor, ops, shape)
    if replay:
        plan.max_crops, plan.crop_stride = batch, stride
    return MercuryPlan(
        mode,
        plan,
        ops,
        final,
        where,
        shape,
        peak,
        len(table),
        external,
        host_parts,
        comps,
        prefix,
        tuple(crops) if batch > 1 else (),
        replay,
        resident,
        res_next if resident else 0,
        cursor,
        cbias,
        win_hi,
    )


def _gkey(g):
    return g.nbin, g.blk0, g.nj, g.ws


def _tile_tensors(ops, shape, external, free, parent_of, host_parts, heads):
    """Tensors a tiles plan keeps in B-tile layout (see build_plan)."""
    pw1 = lambda op: op.conv_class == "pointwise" and op.params.get("taps", 1) == 1  # noqa: E731
    fixed = set(external) | set(free) | set(parent_of) | set(host_parts) | {ops[-1].output}
    if heads:
        fixed.add(OUTPUT)
    out = set()
    for op in ops:
        t = op.output
        if op.kind == "regrid" or t in fixed or shape[t][0] % 32:
            continue
        if not (pw1(op) or (op.conv_class != "pointwise" and op.params["stride"] == (1, 1))):
            continue
        users = [u for u in ops if t in u.inputs]
        g = grid(shape[t])
        if users and all(
            pw1(u) and t in u.inputs[:2] and _gkey(grid(shape[u.inputs[0]])) == _gkey(g) for u in users
        ):
            out.add(t)
    return out


def _merged_crop(
    ops, external, free, comps, parent_of, final, last_use, key, vec, vt, load, store, emit,
    stores=None, tile=frozenset(),
):
    """One crop of a merged request; every VTCM block it takes is given back.
    Tile-layout tensors are 16-vector aligned."""
    stores = stores or [final]
    at = {}
    for b in external:
        at[b] = vt.take(vec(b))
        load(b, at[b])
    for i, op in enumerate(ops):
        dst = op.output
        par = parent_of.get(dst)
        if par is not None:
            if par not in at:
                at[par] = vt.take(vec(par), top=len(free[par]) > 2)
                for n, off in free[par].items():
                    at[n] = at[par] + off
        else:
            later = [k for k in range(i + 1, len(ops)) if dst in ops[k].inputs]
            at[dst] = vt.take(vec(dst), top=bool(later) and later[-1] > i + 1, align=16 if dst in tile else 1)
        emit(op, at, vt)
        if par is not None:
            dev = [n for n, src in comps[par] if src == "device"]
            if dst == dev[-1]:
                for n, src in comps[par]:
                    if src == "host":
                        load(n, at[n])
        if dst in stores:
            store(dst, at[dst])
        for b in set(op.inputs):
            if last_use.get(key(b)) == i:
                k = key(b)
                if k in at:
                    vt.give(at.pop(k), vec(k))
                    for n in free.get(k, {}):
                        at.pop(n, None)
        k = key(dst)
        if k not in last_use and k in at and (par is None or dst in stores):
            vt.give(at.pop(k), vec(k))
    if at:
        raise ValueError(f"crop leaves VTCM blocks held: {sorted(at)}")


def qualify_segments(model, stop_after=LAST, prefix=False, pw_group=1):
    """Greedy split of the request range into qualify plans that each fit the window.
    A segment never both produces a part of a composite and consumes the composite:
    the host stages the composite whole from the device's part bytes."""
    ops = device_ops(model, stop_after, prefix=prefix)
    parts = {par: {n for n, _ in v} for par, v in composites(model, prefix).items()}
    names = [op.name for op in ops]
    plans, i = [], 0
    while i < len(names):
        best = None
        for j in range(i, len(names)):
            made = {o.output for o in ops[i:j]}
            if any(b in parts and parts[b] & made for b in ops[j].inputs):
                break
            try:
                best = build_plan(model, "qualify", names[j], names[i], prefix, pw_group=pw_group)
            except ValueError as exc:
                if "window plan" not in str(exc) or best is None:
                    raise
                break
        plans.append(best)
        i += len(best.ops)
    return plans


# ---------------------------------------------------------------- tensors in the window


def tensor_bytes(mplan, name, tensor):
    import numpy as np

    return mplan.grid(name).to_bytes(np.asarray(tensor, np.float16))


def compose(mplan, parent, parts):
    """Window bytes of a composite from its parts (bytes or arrays), pair-major."""
    return b"".join(
        v if isinstance(v, bytes) else tensor_bytes(mplan, n, v)
        for n, v in ((n, parts[n]) for n, _ in mplan.composites[parent])
    )


def stem_columns(image, size=128):
    """Host im2col of the FP16 C x size x size image (C = 1 or 2; size 128, or 160 for the
    detector) for the stride-2 stem (even phase): plane 9c + dy*3+dx holds
    image[c, 2y-1+dy, 2x-1+dx] (zero outside); a last plane that rounds the count up to even
    (stem_planes) is zero."""
    import numpy as np

    img = np.asarray(image, np.float16)
    img = img.reshape(-1, size, size)
    h = size // 2
    cols = np.zeros((stem_planes(len(img)), h, h), np.float16)
    for c, plane in enumerate(img):
        pad = np.zeros((size + 3, size + 3), np.float16)
        pad[1 : size + 1, 1 : size + 1] = plane
        for dy in range(3):
            for dx in range(3):
                cols[9 * c + 3 * dy + dx] = pad[dy : dy + size : 2, dx : dx + size : 2]
    return cols


def expected_tensors(model, t):
    """Add the prefix's rewritten tensors (columns, group slices) to an emulator run."""
    import numpy as np

    t = dict(t)
    t[STEM_COLS] = stem_columns(t["inputImg"]).astype(np.float32)
    for k in range(model.op("Conv_10").shape[0] // GROUP):
        sl = slice(GROUP * k, GROUP * (k + 1))
        t[_part(EXPAND, k)] = t[EXPAND][sl]
        t[_part(DOWN, k)] = t[DOWN][sl]
    return t


def stage_inputs(mplan, put, tensors):
    """put(window_address, bytes) for every external tensor and host composite part.
    A tensors value may be an array or the raw grid bytes read back from the device."""
    for name in mplan.external + mplan.host_parts:
        v = tensors[name]
        put(mplan.where[name], v if isinstance(v, bytes) else tensor_bytes(mplan, name, v))


def read_tensor(mplan, get, name):
    """get(window_address, n) -> bytes. Returns ([C, H, W] float64, stray nonzero padding)."""
    g, c = mplan.grid(name), mplan.shape[name][0]
    return g.from_bytes(get(mplan.where[name], g.tensor_bytes(c)), c)
