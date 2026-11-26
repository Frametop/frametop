"""Pinned resident landmark graph: FP32 tensors and original FP16 weights.

The one specialized in-place downsample has an explicit row-order proof and a
separate forward compaction phase. It is not a generic alias/in-place facility.
All persistent data uses the uncached DSP alias; the fixed cooperating lease
and refusal record are shared with the earlier resident GEMM proof.
"""

import math
import struct
from array import array
import hashlib
import secrets
import threading
import time

from .landmark import PARAM_SHA, WEIGHTS_SHA, OUTPUTS, ALIASES, specifications
from .resident import ARENA_START, ARENA_END
from .errors import InvalidModel, UnsupportedOperator, RecoveryRequired, JobFailed
from .device import Device, _fp32
from .job import Job
from .resident_gemm import _lease, RECORD, SSR

CONTROL = ARENA_START + 4096
STATE = CONTROL + 128
PROGRAM = ARENA_START + 8192
PROGRAM_BYTES = 8192
INPUT_BYTES = 602112
WEIGHTS = PROGRAM + PROGRAM_BYTES
RAW_WEIGHT_BYTES = 2046732
WEIGHT_BYTES = 2051904
WEIGHT_SPAN = (WEIGHT_BYTES + 127) & ~127
ACTIVATIONS = WEIGHTS + WEIGHT_SPAN + 128
GUARD = ARENA_END - 4096
SCRATCH = GUARD - 45056
SPAN = ARENA_END - ARENA_START
# Linked execution: one read-only operator image in the unused pages between
# the activation high-water mark and the packed-input page.
CODE = 0x1FF0000
CODE_BYTES = 12288
# The wide layout adds the one page left below SCRATCH; a loader accepts
# exactly its own reservation, so the loader name selects it.
CODE_LAYOUTS = {
    "resident_graph_loader_link_v1": CODE_BYTES,
    "resident_graph_loader_link_wide_v1": 16384,
}
SEAL = 0x434F4445
BATCH = 12
UNIT_LIMIT = 1 << 20
BUDGET_TICKS_MAX = 480000
TICKS_PER_MS = 19200
MAGIC = 0x524C4D4B
KINDS = {
    "Convolution": 1,
    "InnerProduct": 1,
    "ConvolutionDepthWise": 3,
    "BinaryOp": 4,
    "Pooling": 5,
}


def _align(n):
    return (n + 127) & ~127


def resident_weights(graph):
    """Lossless pair-row packing; only four odd dense rows require zero padding."""
    blob = bytearray()
    offsets = {}
    for s in specifications(graph):
        if "weight_offset" not in s:
            continue
        start = len(blob)
        raw = graph.raw[s["weight_offset"] : s["weight_offset"] + s["weight_bytes"]]
        if s["operator"] == "ConvolutionDepthWise":
            if any(0 < (h[0] & 0x7FFF) < 1024 for h in struct.iter_unpack("<H", raw)):
                raise UnsupportedOperator(
                    "pinned depthwise fast path requires normal/zero coefficients"
                )
            blob.extend(raw)
        else:
            n = s["output_shape"][0]
            k = len(raw) // (2 * n)
            words = struct.unpack(f"<{n * k}H", raw)
            packed = [
                words[row * k + t] if which == 0 else words[(row + 1) * k + t] if row + 1 < n else 0
                for row in range(0, n, 2)
                for t in range(k)
                for which in range(2)
            ]
            blob.extend(struct.pack(f"<{len(packed)}H", *packed))
        bias = len(blob)
        if s["bias_offset"] is not None:
            blob.extend(graph.raw[s["bias_offset"] : s["bias_offset"] + s["output_shape"][0] * 4])
        offsets[s["name"]] = (start, bias if s["bias_offset"] is not None else None)
    if len(blob) != WEIGHT_BYTES:
        raise InvalidModel("packed resident weight size differs from pinned model")
    return bytes(blob), offsets


def _storage_schedule(specs, shapes):
    roots, last, born, sizes = {}, {}, {}, {}
    for step, s in enumerate(specs):
        for key in s["input"]:
            last[roots[key]] = step
        for key in s["output"]:
            if s["operator"] in ALIASES:
                roots[key] = roots[s["input"][0]]
            else:
                roots[key] = key
                born[key] = step
                last[key] = step
                sizes[key] = _align(math.prod(shapes[key]) * 4)
    for key in OUTPUTS:
        if key in roots:
            last[roots[key]] = len(specs)
    active, placed = {}, {}
    high = ACTIVATIONS
    for step, s in enumerate(specs):
        active = {k: v for k, v in active.items() if last[k] >= step}
        if s["operator"] in ALIASES:
            continue
        key = s["output"][0]
        if s["name"] == "convdwclip_1":
            source = roots[s["input"][0]]
            if (
                s["input_shape"] != (64, 112, 112)
                or s["output_shape"] != (64, 56, 56)
                or s["kernel"] != (3, 3)
                or s["stride"] != (2, 2)
                or s["padding"] != (0, 1, 0, 1)
                or last[source] != step
            ):
                raise InvalidModel("in-place downsample schema/lifetime differs")
            offset = placed[source]["offset"]
            del active[source]
            producer_span = sizes[source]
        else:
            offset = ACTIVATIONS
            for start, length in sorted(active.values()):
                if offset + sizes[key] <= start:
                    break
                offset = max(offset, start + length)
            producer_span = sizes[key]
        if offset + producer_span > SCRATCH - 128:
            raise InvalidModel("resident graph exceeds verified DDR arena")
        placed[key] = {
            "offset": offset,
            "bytes": sizes[key],
            "first": born[key],
            "last": last[key],
            "producer_span": producer_span,
        }
        active[key] = (offset, sizes[key])
        high = max(high, offset + producer_span)
    return roots, placed, high


def resident_landmark_plan(graph):
    """Recompile source shapes/lifetimes, never trust edited planner offsets."""
    if (
        graph.param_sha256 != PARAM_SHA
        or graph.weights_sha256 != WEIGHTS_SHA
        or len(graph.raw) != RAW_WEIGHT_BYTES
        or graph.shapes.get("in0") != (3, 224, 224)
    ):
        raise UnsupportedOperator("resident execution requires the pinned 224x224 v20a graph")
    specs = specifications(graph)
    packed_weights, weight_offsets = resident_weights(graph)
    roots, allocations, high = _storage_schedule(specs, graph.shapes)
    records = []
    arithmetic = []
    for s in specs:
        op = s["operator"]
        if op == "Input" or op in ALIASES:
            continue
        r = [0] * 32
        kind = KINDS[op]
        if op == "Convolution" and s["kernel"] != (1, 1):
            if s["name"] != "convclip_0":
                raise UnsupportedOperator("only the pinned stem has spatial HMX lowering")
            kind = 2
        r[0] = kind
        r[1] = allocations[roots[s["input"][0]]]["offset"]
        r[2] = allocations[roots[s["input"][1]]]["offset"] if len(s["input"]) == 2 else 0
        r[3] = allocations[roots[s["output"][0]]]["offset"]
        ish = s["input_shape"]
        osh = s["output_shape"]
        r[4:7] = list(ish) if len(ish) == 3 else [math.prod(ish), 1, 1]
        r[7:10] = list(osh) if len(osh) == 3 else [math.prod(osh), 1, 1]
        r[10:12] = s.get("kernel", (1, 1))
        r[12:14] = s.get("stride", (1, 1))
        if "padding" in s:
            r[14], r[15] = s["padding"][0], s["padding"][2]
        if "weight_offset" in s:
            if s["weight_dtype"] != "f16":
                raise UnsupportedOperator("resident weights must be original FP16")
            weight, bias = weight_offsets[s["name"]]
            r[16] = WEIGHTS + weight
            r[17] = 0 if bias is None else WEIGHTS + bias
        r[18] = {"none": 0, "clip": 1, "sigmoid": 2}.get(s.get("activation", "none"), 0)
        r[19] = r[5] * r[6]
        r[20] = r[8] * r[9]
        r[21] = allocations[roots[s["input"][0]]]["bytes"]
        output = allocations[roots[s["output"][0]]]
        r[22] = output["producer_span"]
        if s["name"] == "convdwclip_1":
            r[23] = 1
            r[20] = r[19]
        if op == "Pooling":
            p = s["params"]
            if p.get(4, 0):
                r[23] = 2
                r[10] = r[5]
                r[11] = r[6]
            else:
                r[10:12] = [p.get(11, p[1]), p[1]]
                r[12:14] = [p.get(12, p.get(2, 1)), p.get(2, 1)]
        if kind == 1:
            total = ((r[7] + 31) // 32) * ((r[8] * r[9] + 31) // 32)
        elif kind == 2:
            total = ((r[7] + 31) // 32) * r[8] * ((r[9] + 31) // 32)
        elif kind == 3 or kind == 5 and not r[23]:
            total = r[7] * r[8] * ((r[9] + 31) // 32)
        elif kind == 4:
            total = (r[7] * r[8] * r[9] + 31) // 32
        else:
            total = (r[7] + 31) // 32
        r[24] = total
        r[25] = math.prod(osh) * 4 // 128 if r[23] == 1 else 0
        r[26] = ((r[4] * (9 if kind == 2 else 1) + 31) // 32) * 16 if kind in (1, 2) else 0
        records.append(r)
        arithmetic.append(s["name"])
    if len(records) != 63:
        raise InvalidModel("pinned arithmetic layer count differs")
    program = b"".join(struct.pack("<32I", *r) for r in records).ljust(PROGRAM_BYTES, b"\0")
    return {
        "format": "framenpu-resident-landmark-plan",
        "version": 1,
        "storage": "resident_uncached_fp32",
        "span_bytes": SPAN,
        "control_offset": CONTROL,
        "program_offset": PROGRAM,
        "program_bytes": PROGRAM_BYTES,
        "weight_offset": WEIGHTS,
        "weight_bytes": WEIGHT_BYTES,
        "activation_offset": ACTIVATIONS,
        "high_water_offset": high,
        "required_bytes": SPAN,
        "headroom_bytes": SCRATCH - 128 - high,
        "scratch_offset": SCRATCH,
        "scratch_bytes": 45056,
        "roots": roots,
        "allocations": allocations,
        "arithmetic_layers": arithmetic,
        "records": records,
        "program": program,
        "packed_weights": packed_weights,
    }


JOB_KINDS = {
    0: "admin",
    1: "hmx",
    3: "dw",
    4: "add",
    5: "pool",
    6: "sigmoid",
    7: "compact",
    8: "post",
    9: "pack",
    10: "stem",
    11: "avg",
}


def _kind(state, record):
    if record[0] in (1, 2):
        return (
            (9 if record[0] == 1 else 10)
            if state[24] == 0
            else 1
            if state[24] == 1
            else 8
            if state[24] == 3
            else 6
        )
    return 7 if state[24] else 11 if record[0] == 5 and record[23] == 2 else record[0]


def _done(state, record):
    state[20:24] = [0] * 4
    if not state[24] and record[23] == 1:
        state[24] = 1
    else:
        state[24] = 0
        state[19] += 1
        if state[19] == 63:
            state[0] = 2


def _next_row(state, record):
    state[23] += 32
    if state[23] >= record[9]:
        state[23] = 0
        state[22] += 1
        if state[22] == record[8]:
            state[22] = 0
            state[21] += 1


def _post_end(s, record):
    """State after the last bias/clip step of one group of positions."""
    s[18] = 0
    if s[20] == record[24]:
        if record[18] == 2:
            s[24] = 2
            s[20] = s[22] = s[23] = 0
        else:
            _done(s, record)
    else:
        s[22] += 32
        if record[0] == 2 and s[22] >= record[9]:
            s[22] = 0
            s[23] += 1
        s[24] = 0


def _unit(s, kind, record, fault, fused=False):
    """Apply one completed native work unit in place; True ends its phase.

    A fused image adds bias and clips while storing HMX rows, so its last HMX
    unit of a group does what the last separate post unit otherwise does."""
    terminal = False
    s[30] += 1
    if kind in (9, 10):
        s[18] += 1
        if not fault and s[18] == record[26]:
            s[24] = 1
            terminal = True
    elif kind == 1:
        s[20] += 1
        if not fault:
            s[21] += 32
            if s[21] >= record[7]:
                s[21] = s[18] = 0
                if fused:
                    _post_end(s, record)
                else:
                    s[24] = 3
                terminal = True
    elif kind == 8:
        s[18] += 1
        if not fault and s[18] == record[7]:
            _post_end(s, record)
            terminal = True
    elif kind in (3, 5):
        s[20] += 1
        _next_row(s, record)
        if not fault and s[20] == record[24]:
            _done(s, record)
            terminal = True
    elif kind in (4, 11):
        s[20] += 1
        if not fault and s[20] == record[24]:
            _done(s, record)
            terminal = True
    elif kind == 7:
        s[20] += 1
        s[22] += 1
        if s[22] == 98:
            s[22] = 0
            s[21] += 1
        if not fault and s[20] == record[25]:
            _done(s, record)
            terminal = True
    elif kind == 6:
        if not fault:
            _done(s, record)
            terminal = True
    else:
        raise ValueError("unknown resident work kind")
    if fault:
        s[0] = 4
        terminal = True
    return terminal


def _batch_transition(before, words, count, records, fused=False):
    """Replay a linked request that may cross phases and layers.

    Every unit's module follows from resident state alone, so the reported
    unit count determines the result. A request must end at its host stop
    point, at frame completion and after a guarded fault; it may end earlier
    only between whole units (tick budget or unit limit).
    """
    s = list(before)
    s[8:10] = words[8:10]
    s[29] = s[30] = 0
    if not 1 <= count <= words[19]:
        raise ValueError("resident batch count differs")
    if words[30] and count != 1:
        raise ValueError("fault must stop after one unit")
    for i in range(count):
        if s[0] != 3 or s[19] >= len(records):
            raise ValueError("resident batch ran beyond the frame")
        record = records[s[19]]
        terminal = _unit(s, _kind(s, record), record, words[30], fused)
        stopped = s[0] != 3 or (terminal and s[19] == words[24] and s[24] == words[25])
        if stopped and i + 1 != count:
            raise ValueError("resident batch ran past its stop point")
    return s


def _phase_end(state, records, fused=False):
    """(layer, phase) reached when the current phase finishes; the first such
    state after any unit of this phase, so it is a valid one-phase stop point."""
    s = list(state)
    while True:
        record = records[s[19]]
        if _unit(s, _kind(s, record), record, 0, fused):
            return s[19], s[24]


def load_linked(folder):
    """Load a checked `build_resident_landmark.py --linked` output directory."""
    import json
    from pathlib import Path

    folder = Path(folder)
    record = json.loads((folder / "link.json").read_text())
    loader = Job.load(folder / "resident_graph_loader.fjob")
    image = (folder / "resident_graph_linked.bin").read_bytes()
    if (
        record.get("format") != "framenpu-resident-link-build"
        or record.get("version") != 1
        or record.get("code_offset") != CODE
        or record.get("code_reservation_bytes") != CODE_LAYOUTS.get(loader.name)
        or record["loader"] != {"bytes": len(loader.code), "sha256": _sha(loader.code)}
        or record["linked"] != {"bytes": len(image), "sha256": _sha(image)}
    ):
        raise InvalidModel("linked resident build record differs")
    return LinkedJobs(loader, image, record.get("conv_fast", False) is True)


def _sha(data):
    return hashlib.sha256(data).hexdigest()


class LinkedJobs(tuple):
    """(slot loader Job, operator image bytes, fused 1x1 path) for linked execution."""

    __slots__ = ()

    def __new__(cls, loader, image, fused=False):
        return super().__new__(cls, (loader, image, bool(fused)))

    loader = property(lambda self: self[0])
    image = property(lambda self: self[1])
    # True when the image adds bias and clips inside its HMX units.
    fused = property(lambda self: self[2])


def _transition(before, words, count, record=None):
    """Independent host FSM; count is bounded actual work, never a trusted cursor."""
    s = list(before)
    op, kind = words[2:4]
    if op == 0:
        s = [0] * 32
        s[0:3] = [1, 1, MAGIC]
        s[4:18] = words[4:18]
        s[25] = PROGRAM
        return s
    s[8:10] = words[8:10]
    s[29] = s[30] = 0
    if op == 1:
        s[25] = words[21] + words[22]
    elif op == 2:
        s[26] += words[22]
        if words[21] + words[22] == ACTIVATIONS:
            s[0] = 2
            s[3] = 63
            s[19] = 63
    elif op == 3:
        s[0] = 3
        s[19:25] = [0] * 6
        s[27] = 0
        s[28] += 1
    elif op == 4:
        s[27] += words[22]
    elif op == 6:
        s[0] = 6 if words[22] else 5
        if words[22]:
            s[25] = words[21]
    elif op in (7, 8):
        s[31] = SEAL if op == 7 else 0
    elif op == 5:
        if not 1 <= count <= words[19]:
            raise ValueError("resident work count differs")
        if words[30] and count != 1:
            raise ValueError("fault must stop after one unit")
        for i in range(count):
            terminal = _unit(s, kind, record, words[30])
            if terminal and i + 1 != count:
                raise ValueError("resident work crossed a phase boundary")
    return s


class ResidentLandmark:
    """One exclusive, bounded resident session for the pinned landmark graph.

    Weights upload once. Only the next input is written during a run; operator
    dataflow, packing and aliases stay on DSP. Optional validation reads tensors
    without feeding CPU arithmetic back into execution. Confirmed faults permit
    close only; uncertain completion retains arena/code/power and refusal record.
    """

    def __init__(
        self,
        graph,
        jobs,
        *,
        cached_code=False,
        budget_ms=5.0,
        hold_power=False,
        _device_factory=Device,
        _lease_factory=_lease,
    ):
        planning_started = time.monotonic_ns()
        if type(cached_code) is not bool or type(hold_power) is not bool:
            raise InvalidModel("cached_code and hold_power must be boolean")
        # Opt-in: keep HMX powered from open to close instead of per request.
        self.hold_power, self.power = hold_power, None
        self.graph = graph
        self.plan = resident_landmark_plan(graph)
        self.linked = isinstance(jobs, LinkedJobs)
        self.image = b""
        self.code_bytes = 0
        self.fused = False
        self.budget_ticks = 96000
        if self.linked:
            loader, image, self.fused = jobs
            if (
                not isinstance(loader, Job)
                or not loader.requires_hmx
                or not loader.supports_direct
                or loader.name not in CODE_LAYOUTS
                or not isinstance(image, bytes)
                or not 0 < len(image) <= CODE_LAYOUTS[loader.name]
                or len(image) % 4
            ):
                raise InvalidModel("linked execution requires the link-v1 loader and image")
            if (
                type(budget_ms) not in (int, float)
                or not math.isfinite(budget_ms)
                or not 1 <= round(budget_ms * TICKS_PER_MS) <= BUDGET_TICKS_MAX
            ):
                raise InvalidModel("linked request budget must be within (0, 25] ms")
            self.code_bytes = CODE_LAYOUTS[loader.name]
            if self.plan["high_water_offset"] > CODE or CODE + self.code_bytes > SCRATCH:
                raise InvalidModel("resident tensors overlap the linked image reservation")
            self.budget_ticks = round(budget_ms * TICKS_PER_MS)
            self.image = image.ljust(self.code_bytes, b"\0")
            self.image_bytes = len(image)
            jobs = {"loader": loader}
        elif budget_ms != 5.0:
            raise InvalidModel("only linked execution has a selectable request budget")
        elif set(jobs) != set(JOB_KINDS.values()) or any(
            not isinstance(j, Job) or not j.requires_hmx or not j.supports_direct
            for j in jobs.values()
        ):
            raise InvalidModel("all resident HMX/direct modules are required")
        elif cached_code and any(
            job.name != f"resident_graph_{name}_cache_v1" for name, job in jobs.items()
        ):
            raise InvalidModel("cached execution requires the resident cache-v1 entry modules")
        self._cached_code = cached_code
        self.jobs = dict(jobs)
        self.code_live = False
        self.specs = specifications(graph)
        self.records = tuple(tuple(r) for r in self.plan["records"])
        self.program = self.plan["program"]
        self.weights = self.plan["packed_weights"]
        self.immutable = self.program + self.weights.ljust(WEIGHT_SPAN + 128, b"\0")
        self.roots = dict(self.plan["roots"])
        self.allocations = {k: dict(v) for k, v in self.plan["allocations"].items()}
        self.identity = hashlib.sha256(
            self.immutable
            + b"".join(jobs[k].code for k in sorted(jobs))
            + bytes([cached_code])
            + (b"link-v1" + self.image if self.linked else b"")
        ).digest()
        self.nonce = secrets.token_bytes(16)
        if not any(self.nonce):
            raise ValueError("zero resident nonce")
        self.guard = self.nonce * 256
        self.native = [0] * 32
        self.sequence = 0
        self.device = self.lease = None
        self.saved = bytearray(SPAN)
        self.saved_bytes = 0
        self.owned = self.closed = self.invalid = self.sealed = self.guard_ready = False
        self._factory, self._lease_factory = _device_factory, _lease_factory
        self._lock = threading.RLock()
        self.metrics = {
            "cached_code": cached_code,
            "linked": self.linked,
            "budget_ticks": self.budget_ticks,
            "plan_and_weight_pack_ms": (time.monotonic_ns() - planning_started) / 1e6,
            "commands": [],
            "runs": [],
            "weight_upload_bytes": 0,
            "immutable_upload_bytes": 0,
            "input_upload_bytes": 0,
            "weight_loads": 0,
            "input_loads": 0,
            "qualification_readback_bytes": 0,
            "head_readback_bytes": 0,
        }

    @property
    def cached_code(self):
        return self._cached_code

    def _owner(self, t):
        if bytes(t.memory[CONTROL : CONTROL + 56]) != RECORD + self.nonce + self.identity:
            raise RecoveryRequired("resident graph arena ownership changed")

    def _descriptor(self, op, kind=0, *, units=0, offset=0, length=0, fault=False, stop=(63, 0)):
        seq = self.sequence + 1
        if not 0 < seq < (1 << 64) - 1:
            raise ValueError("resident sequence exhausted")
        words = [
            MAGIC,
            1,
            op,
            kind,
            *struct.unpack("<4I", self.nonce),
            seq & 0xFFFFFFFF,
            seq >> 32,
            *struct.unpack("<8I", self.identity),
        ]
        first = kind
        if kind == BATCH and self.native[19] < len(self.records):
            first = _kind(self.native, self.records[self.native[19]])
        progress = self.native[18] if first in (8, 9, 10) else self.native[20]
        words += [self.native[19], units, progress, offset, length] + [0] * 9
        if kind == BATCH:
            words[23:26] = [self.budget_ticks, *stop]
        words[30] = int(fault)
        return words

    def _command(
        self,
        op,
        kind=0,
        *,
        offset=0,
        data=None,
        length=0,
        units=32,
        check=None,
        fault=False,
        descriptor=None,
        reject=False,
        stop=(63, 0),
    ):
        if data is not None:
            if op not in (2, 4, 7) or (op == 7 and data != self.image):
                raise ValueError("only immutable/input/code phases may upload resident data")
            expected = (
                PROGRAM + self.native[26]
                if op == 2
                else ACTIVATIONS + self.native[27]
                if op == 4
                else CODE
            )
            limit = (
                ACTIVATIONS
                if op == 2
                else ACTIVATIONS + INPUT_BYTES
                if op == 4
                else CODE + self.code_bytes
            )
            chunk = INPUT_BYTES if op == 4 and self.linked else 32768
            if offset != expected or not 0 < len(data) <= chunk or offset + len(data) > limit:
                raise ValueError("resident upload is outside its phase/range")
        words = (
            self._descriptor(
                op,
                kind,
                units=units if op == 5 else 0,
                offset=offset,
                length=len(data) if data is not None else length,
                fault=fault,
                stop=stop,
            )
            if descriptor is None
            else list(descriptor)
        )
        before = list(self.native)
        record = self.records[before[19]] if op == 5 and before[19] < len(self.records) else None
        phases = {}
        captured = []
        completions = []
        if self.linked:
            # The slot loader always runs uncached; only the operator image
            # executes through the selected alias, with no per-request cleanup.
            job = self.jobs["loader"]
            in_image = op == 5
            code_base = (
                (0x87000000 if self.cached_code else 0xF0000000) + CODE if in_image else 0xF0A92000
            )
            code_end = code_base + (self.image_bytes if in_image else len(job.code))
            markers = (0, 0)
        else:
            job = self.jobs[JOB_KINDS[kind]]
            code_base = 0x87A92000 if self.cached_code else 0xF0A92000
            code_end = code_base + len(job.code)
            markers = (1, 1) if self.cached_code else (0, 0)
        code_live = self.code_live
        started = time.monotonic_ns()
        program_live = self.sealed and not (before[0] == 6 and before[25] < PROGRAM + PROGRAM_BYTES)
        expected_guard = bytes(self.saved[GUARD - ARENA_START :]) if before[0] == 6 else self.guard

        def prepare(t):
            if not self.owned:
                if bytes(t.memory[CONTROL : CONTROL + 8]) == RECORD:
                    raise RecoveryRequired("resident ownership appeared")
                self.saved[:8192] = bytes(t.memory[ARENA_START:PROGRAM])
                self.saved_bytes = 8192
                t.memory[CONTROL : CONTROL + 128] = (RECORD + self.nonce + self.identity).ljust(
                    128, b"\0"
                )
                self.owned = True
                t.memory[STATE : STATE + 128] = bytes(128)
            self._owner(t)
            if bytes(t.memory[STATE : STATE + 128]) != struct.pack("<32I", *before):
                raise RecoveryRequired("resident native state changed before publication")
            if program_live and bytes(t.memory[PROGRAM : PROGRAM + PROGRAM_BYTES]) != self.program:
                raise RecoveryRequired("resident program changed")
            if self.guard_ready and bytes(t.memory[GUARD:ARENA_END]) != expected_guard:
                raise RecoveryRequired("resident end guard changed")
            if code_live and bytes(t.memory[CODE : CODE + self.code_bytes]) != self.image:
                raise RecoveryRequired("resident linked image changed")
            if data is not None:
                stamp = time.monotonic_ns()
                t.memory[offset : offset + len(data)] = data
                phases["resident_upload_ms"] = (time.monotonic_ns() - stamp) / 1e6
                if op == 2:
                    self.metrics["immutable_upload_bytes"] += len(data)
                    self.metrics["weight_upload_bytes"] += max(
                        0, min(offset + len(data), WEIGHTS + WEIGHT_BYTES) - max(offset, WEIGHTS)
                    )
                    if not self.guard_ready:
                        t.memory[GUARD:ARENA_END] = self.guard
                        self.guard_ready = True
                elif op == 4:
                    self.metrics["input_upload_bytes"] += len(data)

        def validate(result):
            try:
                t = self.device._transport
                self._owner(t)
                if len(result.output) != 256 or len(result.workers) != 3:
                    raise ValueError("incomplete resident completion")
                for i, row in enumerate(result.workers):
                    error = (1 if reject else 2 if fault else 0) if i == 0 else 0
                    if (
                        len(row) != 16
                        or not row[0]
                        or row[0] != result.workers[0][0]
                        or row[1] != i
                        or row[2] != error
                        or row[7] != SSR[i]
                    ):
                        raise ValueError("resident worker identity/SSR/error differs")
                    if tuple(row[14:16]) != (markers if i == 0 else (0, 0)):
                        raise ValueError("resident cache cleanup was not confirmed")
                    if i == 0 and fault and not reject:
                        if row[3] & 255 != 37 or not code_base <= row[4] < code_end:
                            raise ValueError("resident guarded fault differs")
                    elif any(row[3:6]):
                        raise ValueError("unexpected resident fault")
                row = result.workers[0]
                if row[8] != 0x87000000:
                    raise ValueError("resident EVB differs")
                if result.kernel_reference_ticks is None or result.kernel_reference_ticks >= 768000:
                    raise ValueError("resident wrapper exceeds 40ms")
                meta = list(struct.unpack("<64I", result.output))
                if meta[:31] != words[:31] or not code_base <= meta[31] < code_end:
                    raise ValueError("resident descriptor echo/PC differs")
                actual = meta[32:]
                expected = (
                    before
                    if reject
                    else _batch_transition(before, words, actual[30], self.records, self.fused)
                    if op == 5 and kind == BATCH
                    else _transition(before, words, actual[30] if op == 5 else 0, record)
                )
                if actual != expected or struct.unpack_from("<32I", t.memory, STATE) != tuple(
                    expected
                ):
                    raise ValueError(f"resident state transition differs: {actual} != {expected}")
                if actual[29]:
                    raise ValueError("resident raw arithmetic/conversion was nonfinite")
                if self.guard_ready and bytes(t.memory[GUARD:ARENA_END]) != expected_guard:
                    raise ValueError("resident guard changed")
                if (
                    bytes(t.memory[ARENA_START:CONTROL]) != self.saved[:4096]
                    or bytes(t.memory[CONTROL + 256 : PROGRAM]) != self.saved[4352:8192]
                ):
                    raise ValueError("resident control boundary changed")
                if (
                    program_live
                    and bytes(t.memory[PROGRAM : PROGRAM + PROGRAM_BYTES]) != self.program
                ):
                    raise ValueError("resident immutable program changed")
                if (code_live or op == 7 and not reject) and (
                    bytes(t.memory[CODE : CODE + self.code_bytes]) != self.image
                ):
                    raise ValueError("resident linked image changed")
                if (
                    op == 6
                    and before[0] != 6
                    and self.sealed
                    and bytes(t.memory[WEIGHTS:ACTIVATIONS]) != self.immutable[PROGRAM_BYTES:]
                ):
                    raise ValueError("resident immutable weights changed before close")
                if op == 1 and not reject:
                    end = words[21] + words[22]
                    at = words[21] - ARENA_START
                    if at != self.saved_bytes:
                        raise ValueError("resident cache reconciliation skipped a span")
                    self.saved[at : at + words[22]] = t.memory[words[21] : end]
                    self.saved_bytes += words[22]
                if fault and not reject:
                    if struct.unpack_from("<I", t.memory, record[3])[0] != 0xDECAFBAD:
                        raise ValueError("resident trap sentinel/continuation differs")
                elif check is not None:
                    check(before, actual, actual[30], t)
                if getattr(self, "_tearing_down", False) and op == 6:
                    # Bound each write/readback to one completed submission;
                    # retire the refusal prefix after dispatch removal.
                    if words[22]:
                        off, size = words[21:23]
                        original = self.saved[off - ARENA_START : off - ARENA_START + size]
                        t.memory[off : off + size] = original
                        if bytes(t.memory[off : off + size]) != original:
                            raise ValueError("resident teardown chunk readback differs")
                    else:
                        t.memory[ARENA_START:CONTROL] = self.saved[:4096]
                        t.memory[CONTROL + 128 : PROGRAM] = self.saved[4224:8192]
                        if (
                            bytes(t.memory[ARENA_START:CONTROL]) != self.saved[:4096]
                            or bytes(t.memory[CONTROL + 128 : PROGRAM]) != self.saved[4224:8192]
                        ):
                            raise ValueError("resident teardown control readback differs")
                captured.append(actual)
                completions.append((result, meta[31], list(row[14:16])))
            except BaseException as exc:
                raise RecoveryRequired(
                    "resident graph completion invalid; retain arena/code/power"
                ) from exc

        try:
            self.device._submit(
                job,
                struct.pack("<32I", *words),
                opcode=32 if self.linked and op == 5 else 31,
                params=(int(self.cached_code),),
                output_size=256,
                use_direct=True,
                _prepare=prepare,
                _phase_times=phases,
                _completion_validator=validate,
                _held_power=self.power,
            )
            if fault or reject:
                raise RecoveryRequired("resident expected error did not fail")
        except JobFailed:
            if not (fault or reject) or len(captured) != 1:
                raise
        except RecoveryRequired:
            self.device._transport.poisoned = True
            raise
        if not reject:
            self.native = captured[0]
            self.sequence = words[8] | words[9] << 32
            if op in (7, 8):
                self.code_live = op == 7
            if fault:
                self.invalid = True
        returned, pc, cleanup = completions[0]
        self.metrics["commands"].append(
            {
                "op": op,
                "kind": kind,
                "layer": before[19],
                "count": captured[0][30],
                "sequence": self.sequence,
                "rejected": reject,
                "fault": fault,
                "command_ms": (time.monotonic_ns() - started) / 1e6,
                "host_phase_ms": phases,
                "wrapper_ms": returned.kernel_reference_ticks / 19200,
                "publication_ms": returned.elapsed_ms,
                "pc": pc,
                "cache_cleanup": cleanup,
            }
        )
        return self.native

    def __enter__(self):
        with self._lock:
            if self.device is not None or self.closed:
                raise RuntimeError("resident session cannot reopen")
            started = time.monotonic_ns()
            self.lease = self._lease_factory()
            try:
                self.device = self._factory(dispatch="direct")
                self.device.__enter__()
                if bytes(self.device._transport.memory[CONTROL : CONTROL + 8]) == RECORD:
                    raise RecoveryRequired(
                        "resident refusal record remains; establish reset/recovery before reuse"
                    )
                if self.hold_power:
                    self.power = self.device._hold_power()
                self._command(0)
                for at in range(PROGRAM, ARENA_END, 32768):
                    self._command(1, offset=at, length=min(32768, ARENA_END - at))
                for i in range(0, len(self.immutable), 32768):
                    self._command(2, offset=PROGRAM + i, data=self.immutable[i : i + 32768])
                if bytes(self.device._transport.memory[PROGRAM:ACTIVATIONS]) != self.immutable:
                    raise RecoveryRequired("resident immutable upload readback differs")
                self.sealed = True
                if self.linked:
                    self._command(7, offset=CODE, data=self.image)
                self.metrics["weight_loads"] = 1
                self.metrics["open_ms"] = (time.monotonic_ns() - started) / 1e6
                return self
            except BaseException:
                if self.owned:
                    self.device._transport.poisoned = True
                self.close()
                raise

    def __exit__(self, *exc):
        self.close()

    def close(self):
        with self._lock:
            if self.closed:
                return
            started = time.monotonic_ns()
            try:
                if self.device is not None and self.owned and not self.device._transport.poisoned:
                    if self.native[31] == SEAL:
                        # Retire cached instructions before any byte is restored.
                        self._command(8, offset=CODE, length=self.code_bytes)
                    self._tearing_down = True
                    cursor = ARENA_START + self.saved_bytes
                    while cursor > PROGRAM:
                        size = min(32768, cursor - PROGRAM)
                        self._command(6, offset=cursor - size, length=size)
                        cursor -= size
                    self._command(6)
                    t = self.device._transport
                    if self.device._direct is not None:
                        self.device._direct.remove(self.device)
                        self.device._direct = None
                    self._owner(t)
                    t.memory[CONTROL : CONTROL + 128] = self.saved[4096:4224]
                    if (
                        bytes(t.memory[ARENA_START : ARENA_START + self.saved_bytes])
                        != self.saved[: self.saved_bytes]
                    ):
                        raise RecoveryRequired("resident final arena restore differs")
                if self.power is not None and not self.device._transport.poisoned:
                    # Also after an open that failed cleanly before ownership.
                    held, self.power = self.power, None
                    self.device._release_power(held)
            except BaseException:
                if self.owned and self.device is not None:
                    self.device._transport.poisoned = True
                    self.device._transport.memory[CONTROL : CONTROL + 24] = RECORD + self.nonce
                raise
            finally:
                if self.power is not None:
                    # Uncertain state: unmap only, never gate clocks.
                    self.power.abandon()
                    self.power = None
                if self.device is not None:
                    self.device.close()
                if self.lease is not None:
                    self.lease.close()
                self.closed = True
                self.metrics["close_ms"] = (time.monotonic_ns() - started) / 1e6

    def _read_tensor(self, key, *, strided=False):
        allocation = self.allocations[self.roots[key]]
        shape = self.graph.shapes[key]
        t = self.device._transport
        out = array("f")
        if strided:
            c, h, w = shape
            for channel in range(c):
                at = allocation["offset"] + channel * 12544 * 4
                out.frombytes(bytes(t.memory[at : at + h * w * 4]))
        else:
            n = math.prod(shape) * 4
            at = allocation["offset"]
            out.frombytes(bytes(t.memory[at : at + n]))
        return out

    def _output_coordinates(self, record, kind, before, count):
        """Return (flat model index, resident byte offset) in completed work order."""
        c, y, x = before[21:24]
        spatial = record[8] * record[9]
        if kind == 8:
            c = before[18]
            p = before[22] + (before[23] * record[9] if record[0] == 2 else 0)
            n = min(32, record[9] - before[22] if record[0] == 2 else spatial - p)
            return [
                (ch * spatial + p + i, record[3] + (ch * record[20] + p + i) * 4)
                for ch in range(c, c + count)
                for i in range(n)
            ]
        if kind in (3, 5):
            pairs = []
            state = list(before)
            for u in range(count):
                c, y, x = state[21:24]
                n = min(32, record[9] - x)
                pairs.extend(
                    (
                        c * spatial + y * record[9] + x + i,
                        record[3] + (c * record[20] + y * record[9] + x + i) * 4,
                    )
                    for i in range(n)
                )
                _next_row(state, record)
            return pairs
        if kind in (4, 11):
            start = before[20] * 32
            end = min(start + count * 32, math.prod(record[7:10]))
            return [(i, record[3] + i * 4) for i in range(start, end)]
        if kind == 6:
            return [(0, record[3])]
        return []

    def _drive(self, stop, fault_kind=None):
        """Run linked requests until the host stop point; replay bounds the units."""
        s = list(self.native)
        remaining = 0
        while True:
            record = self.records[s[19]]
            remaining += 1
            terminal = _unit(s, _kind(s, record), record, 0, self.fused)
            if s[0] != 3 or terminal and (s[19], s[24]) == tuple(stop):
                break
        while remaining:
            target, fault = stop, False
            if fault_kind is not None:
                # Step one phase at a time so the trap lands in the named module.
                fault = _kind(self.native, self.records[self.native[19]]) == fault_kind
                target = _phase_end(self.native, self.records, self.fused)
            self._command(5, BATCH, units=remaining, stop=target, fault=fault)
            if fault:
                raise JobFailed("confirmed resident graph trap; close session")
            remaining -= self.native[30]

    def _linked_layer(self, spec, record, index, checker, fault_at):
        """One layer in as few requests as the budget allows.

        Extra stops exist only where a later phase overwrites what qualification
        must read: the strided in-place downsample and the pre-sigmoid logit.
        """
        key = spec["output"][0]
        fault_kind = fault_at[1] if fault_at and fault_at[0] == spec["name"] else None

        def read(**layout):
            values = self._read_tensor(key, **layout)
            self.metrics["qualification_readback_bytes"] += len(values) * 4
            return values

        shadow = None
        if record[23] == 1:
            self._drive((index, 1), fault_kind)
            if checker:
                shadow = read(strided=True)
                checker(range(len(shadow)), shadow, True, "output")
        elif record[18] == 2:
            self._drive((index, 2), fault_kind)
            if checker:
                checker([0], read(), False, "logits")
        self._drive((index + 1, 0), fault_kind)
        if checker:
            output = read()
            if shadow is None:
                checker(range(len(output)), output, True, "output")
            elif output.tobytes() != shadow.tobytes():
                raise ValueError("resident in-place compaction changed values")

    def run(self, chw, *, validator_factory=None, stop_after=None, _fault_at=None):
        with self._lock:
            try:
                return self._run(
                    chw,
                    validator_factory=validator_factory,
                    stop_after=stop_after,
                    _fault_at=_fault_at,
                )
            except RecoveryRequired:
                self.invalid = True
                if self.device is not None:
                    self.device._transport.poisoned = True
                raise
            except BaseException:
                if self.native[0] != 2:
                    self.invalid = True
                raise

    def _run(self, chw, *, validator_factory=None, stop_after=None, _fault_at=None):
        with self._lock:
            if self.closed or self.invalid or self.device is None:
                raise RuntimeError("resident graph is not ready")
            if len(chw) != 150528 or any(not math.isfinite(v) or abs(v) > 65504 for v in chw):
                raise InvalidModel("resident input shape/range differs")
            if validator_factory is not None and not callable(validator_factory):
                raise InvalidModel("validator factory must be callable")
            if stop_after is not None and stop_after not in {s["name"] for s in self.specs}:
                raise InvalidModel("unknown diagnostic layer")
            if self.native[0] != 2:
                raise RuntimeError("previous resident frame is incomplete")
            raw = _fp32(chw)
            started = time.monotonic_ns()
            first = len(self.metrics["commands"])
            checks = []
            self._command(3)
            # The linked loader accepts the whole input under one request's gate.
            chunk = INPUT_BYTES if self.linked else 32768
            for at in range(0, len(raw), chunk):
                self._command(4, offset=ACTIVATIONS + at, data=raw[at : at + chunk])
            self.metrics["input_loads"] += 1
            arithmetic = 0
            specs = self.specs
            if self.linked and validator_factory is None and not stop_after and not _fault_at:
                # Unqualified execution: each request runs until its tick budget.
                while self.native[0] == 3:
                    self._command(5, BATCH, units=UNIT_LIMIT)
                specs = ()
            for spec in specs:
                op = spec["operator"]
                key = spec["output"][0]
                qualification_started = time.monotonic_ns()
                inputs = [self._read_tensor(k) for k in spec["input"]] if validator_factory else []
                if validator_factory:
                    self.metrics["qualification_readback_bytes"] += sum(len(a) * 4 for a in inputs)
                checker = validator_factory(spec, inputs) if validator_factory else None
                shadow = array("f", [0.0]) * math.prod(self.graph.shapes[key]) if checker else None
                qualification_setup_ms = (time.monotonic_ns() - qualification_started) / 1e6
                layer_start = time.monotonic_ns()
                command_start = len(self.metrics["commands"])
                if op == "Input" or op in ALIASES:
                    if checker:
                        output = self._read_tensor(key)
                        self.metrics["qualification_readback_bytes"] += len(output) * 4
                        checker(range(len(output)), output, True, "output")
                else:
                    record = self.records[arithmetic]
                    if self.native[19] != arithmetic:
                        raise RecoveryRequired("resident layer ordering differs")
                    if self.linked:
                        try:
                            self._linked_layer(spec, record, arithmetic, checker, _fault_at)
                        except ValueError as exc:
                            raise RecoveryRequired("resident layer qualification failed") from exc
                    while not self.linked and self.native[19] == arithmetic and self.native[0] == 3:
                        kind = _kind(self.native, record)

                        def validate_chunk(old, new, count, t):
                            if kind == 7:
                                if shadow is not None:
                                    channel, vec = old[21:23]
                                    for u in range(count):
                                        begin = channel * 3136 + vec * 32
                                        actual = bytes(
                                            t.memory[
                                                record[3] + begin * 4 : record[3] + (begin + 32) * 4
                                            ]
                                        )
                                        self.metrics["qualification_readback_bytes"] += len(actual)
                                        if actual != shadow[begin : begin + 32].tobytes():
                                            raise ValueError(
                                                "resident in-place compaction changed values"
                                            )
                                        vec += 1
                                        if vec == 98:
                                            channel += 1
                                            vec = 0
                                return
                            coordinates = self._output_coordinates(record, kind, old, count)
                            if checker and coordinates:
                                self.metrics["qualification_readback_bytes"] += len(coordinates) * 4
                                indices = [i for i, off in coordinates]
                                values = [
                                    struct.unpack_from("<f", t.memory, off)[0]
                                    for i, off in coordinates
                                ]
                                phase = "logits" if record[18] == 2 and kind == 8 else "output"
                                final = new[19] != arithmetic or kind == 3 and new[24] == 1
                                checker(indices, values, final, phase)
                                if phase == "output":
                                    for i, value in zip(indices, values):
                                        shadow[i] = value

                        fault = _fault_at == (spec["name"], kind)
                        self._command(
                            5,
                            kind,
                            units=256 if kind in (8, 9, 10) else 32,
                            check=validate_chunk,
                            fault=fault,
                        )
                        if fault:
                            raise JobFailed("confirmed resident graph trap; close session")
                    arithmetic += 1
                checks.append(
                    {
                        "name": spec["name"],
                        "operator": op,
                        "commands": len(self.metrics["commands"]) - command_start,
                        "qualification_setup_ms": qualification_setup_ms,
                        "layer_ms": (time.monotonic_ns() - layer_start) / 1e6,
                    }
                )
                if stop_after == spec["name"]:
                    break
            outputs = {k: self._read_tensor(k) for k in OUTPUTS} if self.native[0] == 2 else {}
            self.metrics["head_readback_bytes"] += sum(len(a) * 4 for a in outputs.values())
            if (
                bytes(self.device._transport.memory[WEIGHTS:ACTIVATIONS])
                != self.immutable[PROGRAM_BYTES:]
            ):
                raise RecoveryRequired("resident weights changed during inference")
            self.metrics["runs"].append(
                {
                    "layers": checks,
                    "complete": self.native[0] == 2,
                    "body_ms": (time.monotonic_ns() - started) / 1e6,
                    "command_count": len(self.metrics["commands"]) - first,
                }
            )
            return outputs
