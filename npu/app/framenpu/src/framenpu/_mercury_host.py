"""The host side of a Mercury request on the native kernels (_native.Host).

Same contract as the NumPy path in mercury_runtime (input checks, hint prefix, stem
columns, grid staging, readback checks, pooled vector heads). The constructor refuses
any graph or plan this module does not cover, so a session falls back to NumPy instead
of computing something else.

Each batch size (1..3 crops) has one preallocated workspace whose buffers, pointers
and kernel calls are bound once, so a request makes no per-call pointer or size
checks. A request is prepare -> stage -> read (per crop) -> finish on one workspace;
callers serialize requests (MercurySession holds its lock throughout).

Covers the Mercury keypoint graph and the 2-view stereo student (inputImg 2x128x128, an
84-value hint, a geom input and the 3D joint head: a 1x1 conv on the FP16 feature map, a
concat and FP32 dense layers); only the ops the requested outputs need run. Mercury's hand
detector (inputImg 1x160x160) has its own request class, NativeDetector.
"""

from . import mercury
from . import _mercury_common as C
from . import _mercury_layout as L


class NativeRequest:
    def __init__(self, host, model, mplan, heads, outputs=None):
        import numpy as np

        self.host, self.heads = host, heads
        self.views = getattr(model, "views", 1)
        self.inputs = dict(getattr(model, "inputs", mercury.INPUTS))
        self.out_shapes = dict(getattr(model, "outputs", mercury.OUTPUTS))
        self.outputs = tuple(self.out_shapes if outputs is None else outputs)
        if self.views not in (1, 2) or self.inputs["inputImg"] != (1, self.views, 128, 128):
            raise ValueError("native staging covers one or two 128x128 views")
        if any(n not in self.out_shapes for n in self.outputs):
            raise ValueError("unknown output")
        mp = mplan.crop(0)
        if list(mp.external + mp.host_parts) != [L.STEM_COLS, L.HINT]:
            raise ValueError("native staging covers the stem columns and the hint plane only")
        gemm, reshape, scale = (model.op(n) for n in L.HOST_HINT_OPS[:3])
        if (
            gemm.kind != "dense"
            or gemm.inputs != ("lastKeypoints",)
            or gemm.params["weight"].shape != (8192, 42 * self.views)
            or gemm.params["act"] not in (None, "relu6")
            or reshape.kind != "reshape"
            or reshape.inputs != (gemm.output,)
            or scale.kind != "scale"
            or scale.inputs != (reshape.output, "useLastKeypoints")
            or scale.output != L.HINT
            or tuple(scale.shape) != (32, 16, 16)
        ):
            raise ValueError("unexpected Mercury hint prefix")
        # FP32 weights: the kernel widens them, exactly (the model stores FP32)
        if gemm.params["weight"].dtype != np.float32:
            raise ValueError("unexpected hint weight type")
        self.hint_w = np.ascontiguousarray(gemm.params["weight"].T, np.float32)
        self.hint_b = np.ascontiguousarray(gemm.params["bias"], np.float32)
        self.hint_relu6 = gemm.params["act"] == "relu6"

        self.grids = {}
        names = [L.STEM_COLS, L.HINT, L.OUTPUT] + ([L.HEATMAP] if heads else [])
        for name in names:
            g, c = mp.grid(name), mp.shape[name][0]
            idx, _ = C._grid_index(g.w, g.h)
            nwords = g.nbin * 32
            if c % 2 or g.tensor_bytes(c) != 2 * c * nwords:
                raise ValueError(f"unexpected grid for {name}")
            for k in range(len(mplan.crops) or 1):
                ck = mplan.crop(k)
                if ck.grid(name) != g or ck.shape[name] != mp.shape[name]:
                    raise ValueError(f"crop {k} lays out {name} differently")
            self.grids[name] = (c, np.ascontiguousarray(idx, np.int32), nwords, g)
        if self.grids[L.STEM_COLS][0] != L.stem_planes(self.views) or self.grids[L.HINT][0] != 32:
            raise ValueError("unexpected staged channel counts")
        self.mplan = mplan
        self._works = {}

        # pooled vector heads: every op after L.LAST that the DSP does not run and the
        # requested outputs need
        names = [op.name for op in model.ops]
        skip = L.HEAD_OPS if heads else ()
        tail = [op for op in model.ops[names.index(L.LAST) + 1 :] if op.name not in skip]
        want, keep = set(self.outputs), []
        for op in reversed(tail):
            if op.output in want:
                keep.append(op)
                want.update(op.inputs)
        have = {L.OUTPUT} | ({"heatmap_xy"} if heads else set()) | set(self.inputs)
        missing = want - have - {op.output for op in keep}
        if missing:
            raise ValueError(f"tail needs tensors the request does not produce: {sorted(missing)}")
        self.tail = []
        for op in keep[::-1]:
            p = op.params
            if op.kind == "avgpool":
                kh, kw = p["kernel"]
                self.tail.append(("avgpool", op, (kh, kw)))
            elif op.kind == "reshape":
                self.tail.append(("reshape", op, None))
            elif op.kind == "dense":
                if p["act"] not in (None, "relu6"):
                    raise ValueError(f"{op.name}: unsupported activation")
                if p["weight"].dtype != np.float32:
                    raise ValueError(f"{op.name}: unexpected weight type")
                w = np.ascontiguousarray(p["weight"].T, np.float32)
                b = np.ascontiguousarray(p["bias"], np.float32)
                self.tail.append(("dense", op, (w, b, p["act"] == "relu6")))
            elif (
                op.kind == "conv"
                and len(op.inputs) == 1
                and mercury.unit(op) == "fp32"
                and p["weight"].shape[2:] == (1, 1)
                and p["group"] == 1
                and tuple(p["stride"]) == (1, 1)
                and tuple(p["pads"]) == (0, 0, 0, 0)
                and p["act"] in (None, "relu6")
            ):
                w = np.ascontiguousarray(p["weight"].reshape(p["weight"].shape[0], -1).T, np.float32)
                b = np.ascontiguousarray(p["bias"], np.float32)
                self.tail.append(("conv1x1", op, (w, b, p["act"] == "relu6")))
            elif (
                op.kind == "conv"
                and len(op.inputs) == 1
                and mercury.unit(op) == "hmx"
                and p["weight"].shape[2:] == (1, 1)
                and p["group"] == 1
                and tuple(p["stride"]) == (1, 1)
                and tuple(p["pads"]) == (0, 0, 0, 0)
                and p["act"] in (None, "relu6")
            ):
                # the HMX contract: FP16 weights and bias (mercury._conv)
                w = np.ascontiguousarray(mercury._f16(p["weight"].reshape(p["weight"].shape[0], -1)), np.float32)
                b = np.ascontiguousarray(mercury._f16(p["bias"]), np.float32)
                self.tail.append(("conv1x1_f16", op, (w, b, p["act"] == "relu6")))
            elif op.kind == "concat" and len(op.shape) == 1:
                self.tail.append(("concat", op, None))
            else:
                raise ValueError(f"{op.name}: no native kernel for {op.kind}")

    # ------------------------------------------------------------------ workspace
    def _work(self, nb, lane=0):
        """The workspace of nb-crop requests; lanes 0..2 let up to three requests of the same
        size be in flight at once (MercurySession.infer_many)."""
        if not 1 <= nb <= 3 or lane not in (0, 1, 2):
            raise ValueError("Mercury requests carry one to three crops")
        w = self._works.get((nb, lane))
        if w is None:
            w = self._works[(nb, lane)] = _Work(self, nb)
        return w

    # ------------------------------------------------------------------ inputs
    def prepare(self, feeds, lane=0):
        """Check and convert the 1..3 crops of a request into their workspace's staged
        FP16 grid words. The hint layer reads its weights once for all crops."""
        import numpy as np

        w = self._work(len(feeds), lane)
        lib = self.host.lib
        for k, feed in enumerate(feeds):
            for name, shape in self.inputs.items():
                v = np.asarray(feed[name])
                if v.shape != shape:
                    raise ValueError(f"{name}: finite values of shape {shape} required")
                dst = w.inputs[name][k]
                np.copyto(dst, v.reshape(dst.shape), casting="unsafe")
                if lib.fnpu_nonfinite32(w.input_ptr[name][k], dst.size):
                    raise ValueError(f"{name}: finite values of shape {shape} required")
            if lib.fnpu_to_f16(w.input_ptr["inputImg"][k], w.img16_p, self.views * 128 * 128):
                raise ValueError("image exceeds FP16 storage")
            for c in range(self.views):  # 9 planes per view, each call zeroes the next plane
                lib.fnpu_stem_columns(w.img16_p + c * 2 * 128 * 128, w.cols_p + c * 2 * 9 * 4096)
            lib.fnpu_grid_pack(*w.stem_pack[k])
        if lib.fnpu_hint(*w.hint_args):
            raise ValueError("hint exceeds FP16 storage")
        for args in w.hint_pack:
            lib.fnpu_grid_pack(*args)
        return w

    def stage(self, mem, w):
        for k in range(w.nb):
            where = self.mplan.crop(k).where
            for name, words in w.staged[k]:
                at = where[name]
                mem[at : at + len(words)] = words

    # ------------------------------------------------------------------ outputs
    def read(self, w, get, k):
        """Unpack and check crop k's device outputs into the workspace; False if invalid."""
        lib = self.host.lib
        where = self.mplan.crop(k).where
        for name in w.read_names:
            raw = w.raw[name]
            w.raw_bytes[name][:] = get(where[name], raw.nbytes)
            lib.fnpu_grid_unpack(*w.unpack[name][k])
            if w.stats[0] or w.stats[1]:
                return False
        return True

    def load_maps(self, w, maps_list):
        """Put given device maps in the workspace (tests)."""
        for k, maps in enumerate(maps_list):
            for name in w.read_names:
                w.maps[name][k] = maps[name]

    def finish(self, w):
        """The four Mercury outputs of each crop; every tail layer reads its weights once
        for all crops of the request. The returned arrays are the caller's own."""
        lib = self.host.lib
        for fn, args in w.tail_calls:
            if callable(fn):
                fn(*args)
            else:
                getattr(lib, fn)(*args)
        return [
            {name: w.outputs[name][k].reshape(self.out_shapes[name]).copy() for name in self.outputs}
            for k in range(w.nb)
        ]


class _Work:
    """Buffers, pointers and bound kernel arguments for requests of nb crops."""

    def __init__(self, nr, nb):
        import numpy as np

        def ptr(a):
            if not a.flags.c_contiguous:
                raise ValueError("native workspace arrays must be C-contiguous")
            return a.ctypes.data

        self.nb = nb
        f32, u16 = np.float32, np.uint16
        self.inputs = {
            name: np.zeros((nb, int(np.prod(shape))), f32) for name, shape in nr.inputs.items()
        }
        self.input_ptr = {n: [ptr(a[k]) for k in range(nb)] for n, a in self.inputs.items()}
        self.img16 = np.zeros(nr.views * 128 * 128, u16)
        self.cols = np.zeros((9 * nr.views + 1) * 4096, u16)  # + the plane the last call zeroes
        self.img16_p, self.cols_p = ptr(self.img16), ptr(self.cols)

        c, idx, nwords, _ = nr.grids[L.STEM_COLS]
        self.stem = np.zeros((nb, c * nwords), u16)
        self.stem_pack = [
            (self.cols_p, c, idx.size, ptr(idx), nwords, ptr(self.stem[k])) for k in range(nb)
        ]
        self.hint16 = np.zeros((nb, 8192), u16)
        self.hint_tmp = np.zeros(nb * 8192, f32)
        self.hint_acc = np.zeros(nb * 8192, np.float64)
        n_in, n_out = nr.hint_w.shape
        if (n_in, n_out) != (42 * nr.views, 8192):
            raise ValueError("unexpected hint weight shape")
        self.hint_args = (
            ptr(nr.hint_w), ptr(nr.hint_b), ptr(self.inputs["lastKeypoints"]),
            ptr(self.inputs["useLastKeypoints"]), nb, n_out, n_in, int(nr.hint_relu6),
            ptr(self.hint_acc), ptr(self.hint_tmp), ptr(self.hint16),
        )
        c, idx, nwords, _ = nr.grids[L.HINT]
        self.hint_grid = np.zeros((nb, c * nwords), u16)
        self.hint_pack = [
            (ptr(self.hint16[k]), c, idx.size, ptr(idx), nwords, ptr(self.hint_grid[k]))
            for k in range(nb)
        ]
        self.staged = [
            ((L.STEM_COLS, memoryview(self.stem[k]).cast("B")),
             (L.HINT, memoryview(self.hint_grid[k]).cast("B")))
            for k in range(nb)
        ]

        self.read_names = (L.OUTPUT, L.HEATMAP) if nr.heads else (L.OUTPUT,)
        self.stats = np.zeros(2, np.int32)
        self.raw, self.raw_bytes, self.maps, self.unpack = {}, {}, {}, {}
        for name in self.read_names:
            c, idx, nwords, g = nr.grids[name]
            self.raw[name] = np.zeros(c * nwords, u16)
            self.raw_bytes[name] = memoryview(self.raw[name]).cast("B")
            self.maps[name] = np.zeros((nb, c, g.h, g.w), f32)
            self.unpack[name] = [
                (ptr(self.raw[name]), c, idx.size, ptr(idx), nwords, ptr(self.maps[name][k]),
                 ptr(self.stats))
                for k in range(nb)
            ]

        # the pooled vector heads, bound once
        t = {L.OUTPUT: self.maps[L.OUTPUT]}
        if nr.heads:
            t["heatmap_xy"] = self.maps[L.HEATMAP][:, :21]
        for name in nr.inputs.keys() - mercury.INPUTS.keys():  # host-only inputs (geom)
            t[name] = self.inputs[name]
        scratch = np.zeros(max([nb * op.shape[0] for _, op, _ in nr.tail]
                               + [int(np.prod(op.shape[1:])) for k, op, _ in nr.tail if k == "conv1x1_f16"]
                               + [1]), np.float64)
        self._keep = [scratch]
        self.tail_calls = []
        for kind, op, extra in nr.tail:
            x = t[op.inputs[0]]
            if kind == "reshape":
                t[op.output] = x.reshape(nb, *op.shape)
                continue
            y = np.zeros((nb, *op.shape), f32)
            self._keep.append(y)
            if kind == "concat":
                parts = [t[i] for i in op.inputs]
                if any(q.ndim != 2 or q.shape[0] != nb for q in parts) or sum(q.shape[1] for q in parts) != op.shape[0]:
                    raise ValueError(f"{op.name}: unexpected shapes")
                at = 0
                for q in parts:
                    self.tail_calls.append((np.copyto, (y[:, at : at + q.shape[1]], q)))
                    at += q.shape[1]
            elif kind == "conv1x1_f16":
                wt, b, relu = extra
                cout, cin = wt.shape
                if x.shape[1] != cin or not x[0].flags.c_contiguous or op.shape[0] != cout:
                    raise ValueError(f"{op.name}: unexpected shapes")
                hw = int(np.prod(x.shape[2:]))
                for k in range(nb):
                    self.tail_calls.append(
                        ("fnpu_conv1x1_f16", (ptr(wt), ptr(b), ptr(x[k]), cout, cin, hw, int(relu), ptr(scratch), ptr(y[k])))
                    )
            elif kind == "avgpool":
                (kh, kw), (c, h, wd) = extra, x.shape[1:]
                for k in range(nb):
                    self.tail_calls.append(
                        ("fnpu_avgpool", (ptr(x[k]), c, h, wd, kh, kw, op.shape[1], op.shape[2], ptr(y[k])))
                    )
            else:
                wt, b, relu = extra
                n_in, n_out = wt.shape
                if x[0].size != n_in or y[0].size != n_out:
                    raise ValueError(f"{op.name}: unexpected shapes")
                fn = "fnpu_dense" if kind == "dense" else "fnpu_conv1x1"
                self.tail_calls.append(
                    (fn, (ptr(wt), ptr(b), ptr(x), nb, n_out, n_in, int(relu), ptr(scratch), ptr(y)))
                )
            t[op.output] = y
        self.outputs = {name: t[name] for name in nr.outputs}


class NativeDetector:
    """The host side of a hand-detector request (mercury.load_detector, a build_plan with
    the device part ending at L.DET_LAST) on the native kernels; same contract as the
    NumPy path in mercury_runtime (stem_columns at DET_SIZE, read_tensor of the final map,
    run_tail): FP16 conversion by fnpu_to_f16, the stem im2col and grid packing as one
    precomputed gather (every staged half is an image pixel or zero), the readback by
    fnpu_grid_unpack, the three dense layers by fnpu_dense (FP64 sums; only their order may
    differ from NumPy's, by one FP32 ulp in rare cases) and the slices and sigmoid on the
    8 logits in NumPy, as mercury._step computes them. Same workspace scheme as
    NativeRequest. The constructor refuses anything else (the session then uses NumPy)."""

    DENSE = ("Gemm_164", "Gemm_169", "Gemm_174")

    def __init__(self, host, model, mplan, outputs=None):
        import numpy as np

        self.host = host
        if not getattr(model, "detector", False) or dict(model.inputs) != mercury.DET_INPUTS:
            raise ValueError("not the Mercury hand detector")
        self.inputs = dict(model.inputs)
        self.out_shapes = dict(model.outputs)
        self.outputs = tuple(self.out_shapes if outputs is None else outputs)
        if any(n not in self.out_shapes for n in self.outputs):
            raise ValueError("unknown output")
        size = mercury.DET_SIZE
        mp = mplan.crop(0)
        if list(mp.external + mp.host_parts) != [L.STEM_COLS] or mplan.final != L.DET_OUTPUT:
            raise ValueError("native detector staging covers the stem columns of a whole device part")
        names = [op.name for op in model.ops]
        tail = model.ops[names.index(L.DET_LAST) + 1 :]
        kinds = [(op.kind, op.name) for op in tail]
        if [k for k, _ in kinds[:4]] != ["reshape", "dense", "dense", "dense"] or tuple(
            n for _, n in kinds[1:4]
        ) != self.DENSE:
            raise ValueError("unexpected detector tail")
        flat, *dense = tail[:4]
        if flat.inputs != (L.DET_OUTPUT,) or tuple(flat.shape) != (1440,):
            raise ValueError("unexpected detector flatten")
        self.dense, x = [], flat.output
        for op in dense:
            p = op.params
            if op.inputs != (x,) or p["act"] not in (None, "relu6") or p["weight"].dtype != np.float32:
                raise ValueError(f"{op.name}: unexpected dense layer")
            self.dense.append((np.ascontiguousarray(p["weight"].T, np.float32),
                               np.ascontiguousarray(p["bias"], np.float32), p["act"] == "relu6"))
            x = op.output
        self.logits = x
        self.heads = []  # (output name, start, end, sigmoid)
        by_out = {op.output: op for op in tail[4:]}
        for name in self.outputs:
            op = by_out.get(name)
            sig = op is not None and op.kind == "sigmoid"
            sl = by_out.get(op.inputs[0]) if sig else op
            if sl is None or sl.kind != "slice" or sl.inputs != (self.logits,):
                raise ValueError(f"{name}: unexpected detector head")
            self.heads.append((name, sl.params["start"], sl.params["end"], sig))

        # staged stem columns: half (pair p, word i, j) holds plane 2p+j at the position
        # whose word is i (C._grid_index), plane 3dy+dx = image[2y-1+dy, 2x-1+dx]; anything
        # else (outside the image, plane 9, non-interior words) is the zero at index size^2
        c = mp.shape[L.STEM_COLS][0]
        g = mp.grid(L.STEM_COLS)
        h = size // 2
        if c != L.stem_planes(1) or (g.w, g.h) != (h, h):
            raise ValueError("unexpected stem grid")
        idx, _ = C._grid_index(g.w, g.h)
        nwords = g.nbin * 32
        if g.tensor_bytes(c) != 2 * c * nwords:
            raise ValueError("unexpected stem grid size")
        zero = size * size
        gather = np.full((c // 2, nwords, 2), zero, np.int32)
        yy, xx = np.divmod(np.arange(h * h), h)
        for plane in range(9):
            dy, dx = divmod(plane, 3)
            iy, ix = 2 * yy - 1 + dy, 2 * xx - 1 + dx
            src = np.where((iy >= 0) & (iy < size) & (ix >= 0) & (ix < size), iy * size + ix, zero)
            gather[plane // 2, idx, plane % 2] = src
        self.gather = gather.reshape(-1)
        self.size, self.stem_c = size, c

        gf = mp.grid(L.DET_OUTPUT)
        cf = mp.shape[L.DET_OUTPUT][0]
        fidx, _ = C._grid_index(gf.w, gf.h)
        fwords = gf.nbin * 32
        if cf % 2 or gf.tensor_bytes(cf) != 2 * cf * fwords or cf * gf.w * gf.h != 1440:
            raise ValueError("unexpected final grid")
        self.final = (cf, np.ascontiguousarray(fidx, np.int32), fwords)
        for k in range(len(mplan.crops) or 1):
            ck = mplan.crop(k)
            for name in (L.STEM_COLS, L.DET_OUTPUT):
                if ck.grid(name) != mp.grid(name) or ck.shape[name] != mp.shape[name]:
                    raise ValueError(f"crop {k} lays out {name} differently")
        self.mplan = mplan
        self._works = {}

    def _work(self, nb, lane=0):
        if not 1 <= nb <= 3 or lane not in (0, 1, 2):
            raise ValueError("Mercury requests carry one to three crops")
        w = self._works.get((nb, lane))
        if w is None:
            w = self._works[(nb, lane)] = _DetWork(self, nb)
        return w

    def prepare(self, feeds, lane=0):
        """Check and convert the 1..3 crops of a request into their staged grid words."""
        import numpy as np

        w = self._work(len(feeds), lane)
        lib = self.host.lib
        shape = self.inputs["inputImg"]
        for k, feed in enumerate(feeds):
            v = np.asarray(feed["inputImg"])
            if v.shape != shape:
                raise ValueError(f"inputImg: finite values of shape {shape} required")
            np.copyto(w.img32, v.reshape(-1), casting="unsafe")
            if lib.fnpu_nonfinite32(w.img32_p, w.img32.size):
                raise ValueError(f"inputImg: finite values of shape {shape} required")
            if lib.fnpu_to_f16(w.img32_p, w.img16_p, w.img32.size):
                raise ValueError("image exceeds FP16 storage")
            np.take(w.img16, self.gather, out=w.stem[k])
        return w

    def stage(self, mem, w):
        for k in range(w.nb):
            at = self.mplan.crop(k).where[L.STEM_COLS]
            mem[at : at + len(w.staged[k])] = w.staged[k]

    def read(self, w, get, k):
        """Unpack and check crop k's final map into the workspace; False if invalid."""
        at = self.mplan.crop(k).where[L.DET_OUTPUT]
        w.raw_bytes[:] = get(at, w.raw.nbytes)
        self.host.lib.fnpu_grid_unpack(*w.unpack[k])
        return not (w.stats[0] or w.stats[1])

    def load_maps(self, w, maps_list):
        """Put given device maps in the workspace (tests)."""
        for k, maps in enumerate(maps_list):
            w.maps[k] = maps[L.DET_OUTPUT].reshape(w.maps[k].shape)

    def finish(self, w):
        import numpy as np

        lib = self.host.lib
        for args in w.dense_calls:
            lib.fnpu_dense(*args)
        out = []
        for k in range(w.nb):
            z = w.logits[k]
            o = {}
            for name, a, b, sig in self.heads:
                v = z[a:b]
                if sig:
                    v = (1.0 / (1.0 + np.exp(-v.astype(np.float64)))).astype(np.float32)
                o[name] = np.array(v, np.float32).reshape(self.out_shapes[name])
            out.append(o)
        return out


class _DetWork:
    """Buffers, pointers and bound kernel arguments for detector requests of nb crops."""

    def __init__(self, nd, nb):
        import numpy as np

        def ptr(a):
            if not a.flags.c_contiguous:
                raise ValueError("native workspace arrays must be C-contiguous")
            return a.ctypes.data

        self.nb = nb
        n = nd.size * nd.size
        self.img32 = np.zeros(n, np.float32)
        self.img16 = np.zeros(n + 1, np.uint16)  # + the zero every outside half reads
        self.img32_p, self.img16_p = ptr(self.img32), ptr(self.img16)
        self.stem = np.zeros((nb, nd.gather.size), np.uint16)
        self.staged = [memoryview(self.stem[k]).cast("B") for k in range(nb)]
        c, idx, nwords = nd.final
        self.raw = np.zeros(c * nwords, np.uint16)
        self.raw_bytes = memoryview(self.raw).cast("B")
        self.stats = np.zeros(2, np.int32)
        self.maps = np.zeros((nb, c * idx.size), np.float32)  # flattened = Flatten_163
        self.unpack = [
            (ptr(self.raw), c, idx.size, ptr(idx), nwords, ptr(self.maps[k]), ptr(self.stats))
            for k in range(nb)
        ]
        self.dense_calls, x, self._keep = [], self.maps, []
        for wt, b, relu in nd.dense:
            n_in, n_out = wt.shape
            if x.shape[1] != n_in:
                raise ValueError("unexpected detector dense shapes")
            y = np.zeros((nb, n_out), np.float32)
            acc = np.zeros(nb * n_out, np.float64)
            self.dense_calls.append((ptr(wt), ptr(b), ptr(x), nb, n_out, n_in, int(relu), ptr(acc), ptr(y)))
            self._keep += [acc, y]
            x = y
        self.logits = x
