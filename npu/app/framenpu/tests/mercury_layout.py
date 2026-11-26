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
* The 1-channel 3x3 stride-2 stem runs on HMX as a K=10 pointwise over host
  im2col columns of the FP16 image (data movement only on the host).
* Stride-2 depthwise uses the even decimation phase (symmetric padding).

Plan modes: "merged" runs every op in one request with tensors resident in
VTCM and stores only the final tensor; "qualify" runs one op per request and
stores every output, for per-layer FP16 bound checks against the device's own
inputs (composites are staged whole from the device's part bytes).
``start``/``stop_after`` truncate either mode (early-layer pilots).
"""

import struct
from dataclasses import dataclass, replace

from hmx_block_layout import pack
import teacher_resident_layout as RL
import teacher_tail_layout as T

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
UNDERFLOW_DW = 18 * 2.0**-14  # qf16 flush-to-zero allowance per depthwise output


def grid(shape):
    return T.Grid(shape[2], shape[1])


def _part(name, k):
    return f"{name}.g{k}"


def _prefix_ops(model):
    """Conv_0 .. Conv_18 rewritten for the resident kernel (see module docstring)."""
    import numpy as np

    stem, expand, down = model.op("Conv_0"), model.op("Conv_10"), model.op("Conv_14")
    names = [op.name for op in model.ops]
    if names[:6] != ["Conv_0", "Conv_4", "Conv_8", "Conv_10", "Conv_14", "Conv_18"]:
        raise ValueError("unexpected Mercury prefix")
    if (
        stem.params["weight"].shape != (32, 1, 3, 3)
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
    w = np.zeros((32, 10, 1, 1), np.float32)
    w[:, :9, 0, 0] = stem.params["weight"].reshape(32, 9)
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
    out = {CONCAT: [(FEATURES, "device"), (HINT, "host")]}
    if prefix:
        n = model.op("Conv_10").shape[0] // GROUP
        out[DOWN] = [(_part(DOWN, k), "device") for k in range(n)]
    return out


def shapes(model, prefix=False, heads=False):
    shape = {op.output: op.shape for op in model.ops}
    if heads:
        for op in head_ops(model):
            shape[op.output] = op.shape
    shape[INPUT] = model.op(HOST_PREFIX).shape
    if prefix:
        shape[STEM_COLS] = (10, *model.op("Conv_0").shape[1:])
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
        pairs, oh, ow = op.shape[0] // 2, gin.h, gin.w
        return [KIND_RG, pairs, 1, oh, ow, gin.nbin, gout.nbin, vin, vout, gin.ws, gout.ws, 1,
                0, 0, gin.word(0, 0), gout.word(1, 1)]
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
    step 2c head ops), checked."""
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


def pw_image(op, gin, group=1):
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
    image = tiles + RL._splat_pairs(op.params["bias"], cout // 2) + gin.mask_bytes()[: gin.nj * 128]
    image += bytes(-len(image) % 2048)
    return image, (group * (nk + no) if group > 1 else nk) * 16


def dw_image(op, gin):
    import numpy as np

    w, b = op.params["weight"], op.params["bias"]
    c = w.shape[0]
    kk = 9
    h = np.asarray(w.reshape(c, kk), np.float16).view("<u2").astype("<u4")
    coef = np.zeros(((c // 2 * kk + 31) // 32) * 32, "<u4")
    coef[: c // 2 * kk] = (h[0::2] | (h[1::2] << 16)).reshape(-1)
    image = coef.tobytes() + RL._splat_pairs(b, c // 2) + gin.mask_bytes()
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
):
    """batch > 1 (merged only): one request runs the merged sequence once per crop,
    each crop with its own input and final-tensor slots; parameter images are
    shared, and VTCM is reused from crop to crop. pw_group must match the build's
    RS_PW_GROUP (it sizes the 1x1 scratch the kernel checks).

    replay (an RS_REPLAY build): batch is the most crops a request may carry
    (at most RL.MAX_CROPS) and a window slot set is reserved for each, but the table
    holds crop 0's descriptors only; the kernel runs them once per crop with
    replay_row addressing. The planner emits every crop and refuses the plan unless
    each equals crop 0 under that rule (same VTCM layout, slots a fixed stride apart)."""
    if mode not in ("merged", "qualify"):
        raise ValueError(mode)
    if batch < 1 or (batch > 1 and mode != "merged"):
        raise ValueError("batch > 1 needs a merged plan")
    if replay and (mode != "merged" or batch > RL.MAX_CROPS):
        raise ValueError(f"replay is a merged plan of at most {RL.MAX_CROPS} crops")
    if heads and (mode != "merged" or start is not None):
        raise ValueError("heads are a whole-network merged plan")
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

    images = {}
    for op in ops:
        if op.kind == "regrid":
            continue
        gin = grid(shape[op.inputs[0]])
        images[op.name] = (
            pw_image(op, gin, pw_group) if op.conv_class == "pointwise" else dw_image(op, gin)
        )

    # window: table | parameter images | tensors (each 4 KiB aligned, never reused)
    table_at = T.WIN_LO
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
    if cursor > T.WIN_HI:
        raise ValueError(f"window plan ends at {cursor:#x}")

    table, jobs = [], []
    peak = 0

    def words(*w):
        table.append(RL._words(*w))

    cur = dict(where)  # the crop being emitted

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
        image, scratch = images[op.name]
        npar = len(image) // 128
        pw = op.conv_class == "pointwise"
        vpar = vt.take(npar + scratch, align=16 if pw else 1)
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
            )
        else:
            cp = pairs(src)
            extra = (gout.nbin, gout.blk0, gout.ws, gout.h, gout.w, tmp) if s2 else ()
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
                want = RL.replay_row(struct.unpack("<32I", table[n]), k, stride)
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
    )


def _merged_crop(
    ops, external, free, comps, parent_of, final, last_use, key, vec, vt, load, store, emit,
    stores=None,
):
    """One crop of a merged request; every VTCM block it takes is given back."""
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
            at[dst] = vt.take(vec(dst), top=bool(later) and later[-1] > i + 1)
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


def stem_columns(image):
    """Host im2col of the FP16 128x128 image for the stride-2 stem (even phase):
    plane dy*3+dx holds image[2y-1+dy, 2x-1+dx] (zero outside); plane 9 is zero."""
    import numpy as np

    img = np.asarray(image, np.float16).reshape(128, 128)
    pad = np.zeros((131, 131), np.float16)
    pad[1:129, 1:129] = img
    cols = np.zeros((10, 64, 64), np.float16)
    for dy in range(3):
        for dx in range(3):
            cols[3 * dy + dx] = pad[dy : dy + 128 : 2, dx : dx + 128 : 2]
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


def simulate(mplan, tensors, s2_direct=False):
    """Lane-level simulation; returns {stored tensor: (array, stray)}.
    s2_direct models the RS_S2_DIRECT image's stride-2 data path. For a batch
    plan, tensors is one dict per crop and the result one dict per crop; a replay
    plan takes 1..batch of them (the request's crop count)."""
    window = bytearray(T.WIN_HI - T.WIN_LO)

    def put(off, data):
        window[off - T.WIN_LO : off - T.WIN_LO + len(data)] = data

    def get(off, n):
        return bytes(window[off - T.WIN_LO : off - T.WIN_LO + n])

    for off, data in RL.setup_writes(mplan.plan).items():
        put(off, data)
    crops = tensors if mplan.crops else [tensors]
    n = mplan.table_crops(len(crops))
    for k, ts in enumerate(crops):
        stage_inputs(mplan.crop(k), put, ts)
    RL.simulate(
        mplan.plan, window, s2_phase=S2_PHASE, s2_direct=s2_direct, crops=n if mplan.replay else 1
    )
    outs = [
        {b: read_tensor(mplan.crop(k), get, b) for job in mplan.plan.jobs for b in job.stores}
        for k in range(n)
    ]
    return outs if mplan.crops else outs[0]


# ---------------------------------------------------------------- FP16 bounds


def exact(op, x, res=None):
    """FP32-weight, FP64-accumulated result of one op from the device's input."""
    import numpy as np
    from framenpu import mercury

    return mercury._conv(op, np.asarray(x, np.float32), res, "fp32").astype(np.float64)


def bound(op, x, res=None):
    """Per-value FP16 allowance for one device op given its actual input
    (the formulas of tests/teacher_tail_layout.py bound_pw / bound_dw)."""
    import numpy as np

    w = np.abs(np.asarray(op.params["weight"], np.float16).astype(np.float64))
    b = np.abs(np.asarray(op.params["bias"], np.float64))
    ax = np.abs(np.asarray(x, np.float64))
    e = 2.0**-10
    if op.conv_class == "pointwise":
        cout, cin = w.shape[:2]
        mag = (w.reshape(cout, cin) @ ax.reshape(cin, -1)).reshape(cout, *ax.shape[1:])
        allow = 4 * e * mag + 3 * e * (mag + b[:, None, None]) + 2.0**-20
        if res is not None:
            allow = allow + 2 * e * np.abs(np.asarray(res, np.float64))
        return allow
    c, h, wd = ax.shape
    pad = np.zeros((c, h + 2, wd + 2))
    pad[:, 1:-1, 1:-1] = ax
    mag = np.zeros((c, h, wd))
    for dy in range(3):
        for dx in range(3):
            mag += w[:, 0, dy, dx, None, None] * pad[:, dy : dy + h, dx : dx + wd]
    # The resident depthwise multiplies and accumulates in HVX qf16, which flushes values
    # below the FP16 normal range (2^-14) to zero: each of the 9 products and 9 partial
    # sums may lose up to 2^-14 (observed: Mercury Conv_14, a 2.07e-5 product dropped).
    allow = 13 * e * mag + 3 * e * b[:, None, None] + UNDERFLOW_DW
    if op.params["stride"] == (2, 2):
        allow = allow[:, ::2, ::2]
    return allow
