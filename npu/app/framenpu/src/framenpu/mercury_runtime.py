"""Exclusive Mercury replay session for the qualified Steam Frame image.

Accepts already-normalized ONNX feeds for 1..3 crops. Convolutions run on the
DSP; the hint and heads use the qualified host contract. No crop queue,
background batching, retry, firmware recovery or service restart is used.
"""

import hashlib
import json
from pathlib import Path
import secrets
import struct
import threading
import time

from . import mercury
from . import _mercury_common as C
from . import _mercury_layout as L
from . import _native
from ._mercury_host import NativeDetector, NativeRequest
from .device import Device
from .errors import InvalidModel, RecoveryRequired
from .job import Job
from .resident_gemm import _lease, RECORD, SSR
from .resident_landmark import CONTROL

IMAGE_SHA256 = "d7a78a81469c10acaf193b0d99cc0415f92d1de845a7a9aa292a7a1ebe691819"
# The same build with --mheads: the heatmap head (step 2c) runs on the DSP too.
HEADS_IMAGE_SHA256 = "582d79ce821304ab90b48d18c5e9d7c5e4396614b9c6eb70bf2e7b6febf114d3"
# The heads build with --resident: parameter images stay in the FeatureNN blob region.
# Built with --omit-fp --pw-pipe: no leaf frames on the DDR stack for the group barriers
# to wait on, overlapped 1x1 loops (same outputs bit for bit; 3.35 -> 2.96 ms DSP per crop).
# 9160e4c7 (16 KiB) was the --omit-fp --pw-pipe build. This one adds --hmx-inline --pool
# --tiles --post-batch --pw-deep --pw-lg 3 --code-kib 20: three workers, tile-layout 1x1
# inputs, 8-block 1x1 groups; same outputs bit for bit, 2.96 -> 1.63 ms DSP per crop.
RESIDENT_IMAGE_SHA256 = "5d15e715948564ee0546cfb05d5cabb5c767f6f302936dad99716c0b15637ee4"
# The same build with --res-slots: four residency tag slots, so several models (ft7 and the
# stereo 3D student) keep their parameter images resident side by side. Without the option the
# sources still build RESIDENT_IMAGE_SHA256 byte for byte.
SLOTS_IMAGE_SHA256 = "320ae5c3f856b91ea9cb8b99e0fe04159309624eba13a8fedd8d6d2c8cb1ae6c"
RES_SLOTS = 4
# The slots build with --cbias --win-wide --s2-rev: biases packed one word per channel pair
# (expanded per op at the top of the blob region), the window up to the image reservation
# (0x1ff0000), so the hand detector fits resident beside ft7, and the stride-2 column blocks
# stored last to first (the detector's 80 -> 40 layer is wider than one block; the post-batch
# order clobbered the next row's first columns, 382fe1e6 on device). Same loader.
DET_IMAGE_SHA256 = "bebc1ea8d758b3b77e46a820344983c8981d4ba288426b527a2b39bba686a705"
# The same build with --multi: a request may carry two segments (two models' tables, crop counts
# and residency slots, e.g. the detector and ft7 of one frame), run back to back in one DSP job
# under one gate hold and one 20 ms budget (infer_frame). One-segment requests run as before.
MULTI_IMAGE_SHA256 = "ffe783131158f302d28c68812c9d5c4855892dee01e3e4abb0ef18e5c145ec94"
MULTI_SEGMENTS = 2
# The --multi build with --stream: in a streamed request segment 1 starts once the host has
# staged it (request word 12 = the nonce in word 11), and each segment publishes meta word 13 =
# nonce after its outputs. infer_frame then prepares and stages segment 1 and reads and finishes
# segment 0 while the DSP runs; only segment 0's prepare and segment 1's finish stay serial.
STREAM_IMAGE_SHA256 = "cd656b5d4641ac37b1820549dacef93859412f42fb60fe093641907f4630e5de"
STREAM_LATE = 0x7D  # the kernel's error when segment 1 was not staged within 5 ms
# Straight-line VTCM backup/restore (the uncached slot refetched each loop packet: 385 -> 42 us).
# The pool loader (workers 1 and 2 enter the image too) for the 20 KiB reservation.
LOADER_SHA256 = "cc81fb7ec3ad340da8fe3ba7f3c930689cd9fd81861a5fe8cab29babc7247524"
CODE, CODE_BYTES, SEAL_AT, SEAL = 0x1FF0000, 0x4000, 0x1FF3F80, 0x544C4E4B
IMAGE_CACHED, SLOT_UNCACHED = 0x88FF0000, 0xF0A92000
ADMIN_MAGIC, ADMIN_OPCODE = 0x544C4144, 112


def image_kind(record):
    """(heads, resident) of a link record."""
    return bool(record.get("mheads")), bool(record.get("resident"))


def reservation(record):
    """(bytes, seal vector) of a link record's image reservation at CODE: the
    16 KiB default or the 20 KiB --code-kib 20 build. The loader's admin command
    rejects any other size, so a misstated record fails before sealing."""
    r = record.get("reservation", {})
    size = r.get("bytes")
    if r.get("code") != f"{CODE:#x}" or size not in (0x4000, 0x5000) or (
        r.get("seal_vector") != f"{CODE + size - 128:#x}"
    ):
        raise InvalidModel("Mercury image reservation differs")
    return size, CODE + size - 128


def load_jobs(folder):
    """Accept the qualified replay build only (with or without the DSP heatmap
    head, and the heads build with resident parameters, with or without
    residency slots), including its existing
    loader. Returns (loader, image, heads); resident_image(folder) tells the rest."""
    folder = Path(folder)
    record = json.loads((folder / "link.json").read_text())
    loader = Job.load(folder / "teacher_link_loader.fjob")
    image = (folder / "teacher_link.bin").read_bytes()
    heads, resident = image_kind(record)
    if resident and not heads:
        raise InvalidModel("Mercury resident parameters need the heads image")
    slots = record.get("res_slots") == RES_SLOTS
    if record.get("res_slots") not in (None, RES_SLOTS) or (slots and not resident):
        raise InvalidModel("Mercury residency slots differ")
    wide = bool(record.get("cbias")) or record.get("win_hi") is not None or bool(record.get("s2_rev"))
    if wide and (not slots or not record.get("cbias") or record.get("win_hi") != "0x1ff0000"
                 or not record.get("s2_rev")):
        raise InvalidModel("Mercury packed-bias / wide-window build differs")
    multi, stream = record.get("multi"), record.get("stream")
    if multi not in (None, MULTI_SEGMENTS) or (multi and not wide) or stream not in (None, True) or (
            stream and not multi):
        raise InvalidModel("Mercury multi-segment build differs")
    want = (STREAM_IMAGE_SHA256 if stream else MULTI_IMAGE_SHA256 if multi else DET_IMAGE_SHA256 if wide else SLOTS_IMAGE_SHA256 if slots else RESIDENT_IMAGE_SHA256) if resident else \
        HEADS_IMAGE_SHA256 if heads else IMAGE_SHA256
    if (
        hashlib.sha256(image).hexdigest() != want
        or hashlib.sha256(loader.code).hexdigest() != LOADER_SHA256
        or record["linked"]["sha256"] != want
        or record["loader"]["sha256"] != LOADER_SHA256
        or loader.name != "teacher_link_loader_v1"
        or not loader.requires_hmx
        or not loader.supports_direct
        or record.get("pw_group") != 8
        or any(
            not record.get(k)
            for k in ("stride2", "s2_even", "s2_direct", "pw_fast", "dw_fast", "replay")
        )
        or any(record.get(k) for k in ("heads", "stem", "pw_profile"))
    ):
        raise InvalidModel("Mercury requires the qualified replay image and loader")
    return loader, image, heads


def _completion(result, output_bytes):
    """Check worker identity, code/EVB, status and fault fields under the gate."""
    if len(result.workers) != 3 or len(result.output) != output_bytes:
        raise RecoveryRequired("Mercury completion is incomplete")
    for i, row in enumerate(result.workers):
        if (
            len(row) != 16
            or not row[0]
            or row[0] != result.workers[0][0]
            or row[1] != i
            or row[7] != SSR[i]
            or any(row[2:6])
            or any(row[14:16])
        ):
            raise RecoveryRequired("Mercury worker completion differs")
    if (
        result.workers[0][8] != 0x87000000
        or result.kernel_reference_ticks is None
        or not 0 < result.kernel_reference_ticks < 768000
    ):
        raise RecoveryRequired("Mercury EVB or wrapper time differs")


def _completion_guard(check):
    """Any interrupted or failed integrity check retains the DSP resources."""

    def validate(result):
        try:
            return check(result)
        except RecoveryRequired:
            raise
        except BaseException as exc:
            raise RecoveryRequired("Mercury completion validation failed") from exc

    return validate


def replay_metadata(result, count, descriptors, image_bytes, resident=None):
    """resident: None for a non-resident image; else this request's mode (bool).
    A resident image reports meta[12] = mode | copied << 1 | tagged << 2, and a
    complete request tags exactly when it copied."""
    _completion(result, C.META_BYTES)
    return _segment_metadata(result.output, count, descriptors, image_bytes, resident,
                             result.kernel_reference_ticks)


def frame_metadata(result, segments, image_bytes, resident, done=None):
    """A --multi request's metadata: segments = [(crops, descriptors), ...], one
    META_BYTES block each (segment k's in block k), the segments' bodies together
    within the request's one 20 ms budget and the wrapper's time."""
    _completion(result, C.META_BYTES * len(segments))
    out = [
        _segment_metadata(result.output[k * C.META_BYTES : (k + 1) * C.META_BYTES], count, n,
                          image_bytes, resident, result.kernel_reference_ticks, done)
        for k, (count, n) in enumerate(segments)
    ]
    if sum(t["crop_end_ticks"][-1] for t in out) > min(384000, result.kernel_reference_ticks):
        raise RecoveryRequired("Mercury segment bodies exceed the request budget")
    return out


def _segment_metadata(output, count, descriptors, image_bytes, resident, kernel_ticks, done=None):
    """done: a streamed request's nonce, which the segment publishes in word 13."""
    m = list(struct.unpack("<256I", output))
    if done is not None:
        if m[13] != done:
            raise RecoveryRequired("Mercury segment completion word differs")
        m[13] = 0
    status = m[12]
    if resident is not None:
        if status not in ((3 | 4, 1) if resident else (0,)):
            raise RecoveryRequired("Mercury residency status differs")
        m[12] = 0
    ticks = list(m[96 : 96 + descriptors])
    ends = list(m[48 : 48 + count]) if count > 1 else [ticks[-1]]
    base = ends[-2] if count > 1 else 0
    if (
        m[0] != C.RS_MAGIC
        or m[2] != descriptors * count
        or not IMAGE_CACHED <= m[1] < IMAGE_CACHED + image_bytes
        or any(m[3:48])
        or any(m[48 + count : 96] if count > 1 else m[48:96])
        or any(m[96 + descriptors :])
        or any(b <= a for a, b in zip([0] + ends, ends))
        or any(b < a for a, b in zip(ticks, ticks[1:]))
        or not base
        < ticks[0]
        <= ticks[-1]
        == ends[-1]
        <= min(384000, kernel_ticks)
    ):
        raise RecoveryRequired("Mercury replay metadata or budget differs")
    return {
        "crops": count,
        "params_copied": bool(status & 2) if resident else None,
        "crop_end_ticks": ends,
        "crop_body_ms": [(b - a) / 19200 for a, b in zip([0] + ends, ends)],
        "body_ms": ends[-1] / 19200,
    }


class _Model:
    """One model a session serves: its lowered graph, plan, residency tag and tag slot."""

    def __init__(self, model, plan, slot, outputs):
        self.model, self.plan, self.slot, self.outputs = model, plan, slot, outputs
        self.native = None
        self.crop_ms = 0.0  # last measured DSP body per crop: the next request sleeps most of it
        self.tag = (0, 0, 0, 0)  # the kernel copies the images unless the slot holds it
        while not self.tag[0]:
            self.tag = struct.unpack("<4I", secrets.token_bytes(16))


class _Pending:
    """One request on its way through host prepare -> device run -> host finish."""

    def __init__(self, sl, count, timed):
        self.sl, self.count = sl, count
        self.prepared, self.extra, self.maps, self.timing = [], [], [], None
        self.marks = {} if timed else None
        self.restart()

    def restart(self):
        """start timing on the calling thread (a phase runs on one thread)"""
        if self.marks is not None:
            self._t, self._c = time.perf_counter(), time.thread_time()

    def mark(self, name):
        """wall ms of the phase since the last mark, plus its thread CPU ms and core (profiling)"""
        if self.marks is not None:
            now, cpu = time.perf_counter(), time.thread_time()
            self.marks[name] = (now - self._t) * 1e3
            self.marks[name + ".cpu"] = (cpu - self._c) * 1e3
            self.marks[name + ".core"] = _core()
            self._t, self._c = now, cpu


def _core():
    """the core the calling thread runs on (profiling; -1 if unknown)"""
    try:
        with open("/proc/thread-self/stat") as f:
            return int(f.read().rsplit(")", 1)[1].split()[36])
    except (OSError, ValueError, IndexError):
        return -1


def _outputs(model):
    """per-crop outputs: all of Mercury's; for the stereo student only what its 3D hand
    needs (joints3d, is-hand logit)"""
    if model.detector:
        return tuple(model.outputs)
    return tuple(model.outputs) if model.views == 1 else ("joints3d", "scalar_extras")


class MercurySession:
    """One process owns the shared window until close; explicit idle releases HMX.

    A session opens lazily on the first infer(), holds power between calls, and
    releases it at idle(). A poisoned session retains its refusal record and
    device bytes and cannot reopen. The caller may use an independent CPU path.

    extra_models (a --res-slots image): further (path, crops per request) models served
    by the same session, infer(feeds, model=k) with k = 1, 2, ...; each gets its own part
    of the window and of the blob region and its own residency tag slot, so switching
    models copies no parameters. An extra model may be Mercury's hand detector
    (inputImg 160x160 -> hand_exists, cx, cy, size); it needs the packed-bias wide-window
    image (DET_IMAGE_SHA256) to fit beside a keypoint model.
    """

    def __init__(
        self,
        model_path,
        jobs,
        *,
        require_pin=True,
        native=None,
        resident=None,
        extra_models=(),
        _device_factory=Device,
        _lease_factory=_lease,
    ):
        """native: None uses the bundled host kernels when this platform and image allow
        (else NumPy), False always uses NumPy, True requires the kernels, and a
        _native.Host instance uses that library (tests). resident: None keeps the
        parameter images in the FeatureNN blob region when the image supports it, True
        requires that, False copies them for every crop (legacy plan, same image)."""
        # require_pin=False accepts retrained weights for the same graph: lower() still validates
        # the structure, shapes and operators, and every plan row is bounds-checked below.
        model = mercury.load_keypoint(model_path, require_pin=require_pin)
        self.loader, self.image, self.heads = load_jobs(jobs)
        self.image_sha256 = hashlib.sha256(self.image).hexdigest()
        self.res_slots = self.image_sha256 in (SLOTS_IMAGE_SHA256, DET_IMAGE_SHA256, MULTI_IMAGE_SHA256,
                                               STREAM_IMAGE_SHA256)
        self.stream = self.image_sha256 == STREAM_IMAGE_SHA256
        self.multi = self.stream or self.image_sha256 == MULTI_IMAGE_SHA256
        self.resident_image = self.res_slots or self.image_sha256 == RESIDENT_IMAGE_SHA256
        # RS_CBIAS + RS_WIN_WIDE (load_jobs checked the record against the hash)
        self.cbias = self.image_sha256 in (DET_IMAGE_SHA256, MULTI_IMAGE_SHA256, STREAM_IMAGE_SHA256)
        self.win_hi = C.WIN_HI_WIDE if self.cbias else C.WIN_HI
        if extra_models and not self.res_slots:
            raise InvalidModel("several models need the --res-slots image")
        if len(extra_models) >= RES_SLOTS:
            raise InvalidModel(f"at most {RES_SLOTS} models")
        if resident and not self.resident_image:
            raise InvalidModel("this Mercury image has no resident parameter support")
        self.resident = self.resident_image if resident is None else bool(resident)
        if self.cbias and not self.resident:
            raise InvalidModel("the packed-bias image runs resident requests only")
        # an RS_TILES build keeps 1x1-only tensors in B-tile layout (no gather). A record
        # that misstates it is harmless: a build without RS_TILES ignores the layout word
        # (every tensor stays planar), and one with it runs flag-free rows planar too.
        record = json.loads((Path(jobs) / "link.json").read_text())
        self.tiles = self.resident and bool(record.get("tiles"))
        # 8-block 1x1 groups (--pw-deep --pw-lg 3): flagged ops get a larger scratch. A build
        # without them refuses flag 8; one with them runs unflagged ops 4-block.
        self.deep8 = self.tiles and bool(record.get("pw_deep")) and record.get("pw_lg") == 3
        self.code_bytes, self.seal_at = reservation(record)
        if len(self.image) > self.seal_at - CODE:
            raise InvalidModel("Mercury image overlaps its seal vector")
        self.models = []
        res_base = win_base = None
        for slot, (m, batch) in enumerate([(model, 3)] + [
            (mercury.load_model(path, require_pin=False), batch) for path, batch in extra_models
        ]):
            if not 1 <= batch <= 3:
                raise InvalidModel("a model's requests carry one to three crops")
            try:
                plan = L.build_plan(
                    m, prefix=not m.detector, batch=batch, replay=True, pw_group=8,
                    heads=self.heads and not m.detector, resident=self.resident, tiles=self.tiles,
                    deep8=self.deep8, res_base=res_base, win_base=win_base, cbias=self.cbias,
                    win_hi=self.win_hi,
                )
            except ValueError as exc:
                raise InvalidModel(f"model {slot}: {exc}") from exc
            if extra_models and not self.resident:
                raise InvalidModel("several models need resident parameters")
            res_base, win_base = plan.res_end, -(-plan.win_end // 4096) * 4096
            for k in range(batch):
                for row in struct.iter_unpack("<32I", plan.plan.table):
                    if row[0] == L.KIND_RG and self.heads:
                        ok = C.regrid_ok(row)
                    else:
                        ok = C.native_ok(
                            C.replay_row(row, k, plan.plan.crop_stride, self.win_hi),
                            s2_direct=0,
                            pw_group=8,
                            mheads=self.heads,
                            resident=self.resident_image,
                            tiles=self.tiles,
                            deep8=self.deep8,
                            cbias=self.cbias,
                            win_hi=self.win_hi,
                        )
                    if not ok:
                        raise InvalidModel("Mercury plan exceeds native bounds")
            self.models.append(_Model(m, plan, slot, _outputs(m)))
        if native is not False:
            host = native if isinstance(native, _native.Host) else _native.bundled()
            if host is None:
                if native is True:
                    raise InvalidModel("the native host kernels are not available here")
            else:
                for sl in self.models:
                    try:
                        sl.native = (
                            NativeDetector(host, sl.model, sl.plan, sl.outputs)
                            if sl.model.detector
                            else NativeRequest(host, sl.model, sl.plan, self.heads, sl.outputs)
                        )
                    except ValueError as exc:
                        # an explicit request covers the primary model; extra models (other
                        # graphs) fall back to NumPy where the kernels do not cover them
                        if native is not None and sl.slot == 0:
                            raise InvalidModel(f"native host path: {exc}") from exc
        self._device_factory, self._lease_factory = _device_factory, _lease_factory
        self.device = self.lease = self.power = None
        self.closed = self.owned = self.sealed = False
        self.marker = RECORD + secrets.token_bytes(16) + bytes.fromhex(self.image_sha256)
        self.saved = []
        self.lock = threading.RLock()
        self.last_timing = None
        self.phase_times = False  # True: last_timing["phases_ms"] times each host step (profiling)
        self._host_cost = {}  # (prepare|finish, model, crops) -> seconds, a running estimate

    # the primary model (model 0), as single-model callers use it
    model = property(lambda self: self.models[0].model)
    plan = property(lambda self: self.models[0].plan)
    tag = property(lambda self: self.models[0].tag)
    native = property(lambda self: self.models[0].native)
    outputs = property(lambda self: self.models[0].outputs)
    crop_ms = property(lambda self: self.models[0].crop_ms)

    def _owner(self):
        if (
            bytes(self.device._transport.memory[CONTROL : CONTROL + len(self.marker)])
            != self.marker
        ):
            raise RecoveryRequired("Mercury arena ownership changed")

    def _reservation(self, sealed):
        mem = self.device._transport.memory
        expect = bytearray(self.image.ljust(self.code_bytes, b"\0"))
        struct.pack_into("<I", expect, self.seal_at - CODE, SEAL if sealed else 0)
        if bytes(mem[CODE : CODE + self.code_bytes]) != expect:
            raise RecoveryRequired("Mercury image or seal changed")
        self._owner()

    def _gate(self, action):
        t = self.device._transport
        t.claim(self.device.claim_timeout)
        try:
            action(t.memory)
        except RecoveryRequired:
            t.poisoned = True
            raise
        finally:
            if not t.poisoned:
                t.release_unsubmitted()

    def _admin(self, command):
        before, after = self.sealed, command == 7

        def validate(result):
            _completion(result, 256)
            m = struct.unpack("<64I", result.output)
            if (
                m[0] != ADMIN_MAGIC
                or not SLOT_UNCACHED <= m[1] < SLOT_UNCACHED + len(self.loader.code)
                or m[2] != (SEAL if after else 0)
                or any(m[3:])
            ):
                raise RecoveryRequired("Mercury seal/retire completion differs")
            self._reservation(after)
            self.sealed = after

        self.device._submit(
            self.loader,
            struct.pack("<4I", ADMIN_MAGIC, command, CODE, self.code_bytes).ljust(128, b"\0"),
            opcode=ADMIN_OPCODE,
            params=(0,),
            output_size=256,
            use_direct=True,
            _prepare=lambda t: self._reservation(before),
            _completion_validator=_completion_guard(validate),
            _held_power=self.power,
        )

    def _open(self):
        if self.closed:
            raise RuntimeError("Mercury session is closed")
        if self.device is not None:
            return
        self.lease = self._lease_factory()
        try:
            self.device = self._device_factory(dispatch="direct")
            self.device.__enter__()

            def install(mem):
                if bytes(mem[CONTROL : CONTROL + len(RECORD)]) == RECORD:
                    raise RecoveryRequired("resident refusal record present")
                self.saved = [
                    (at, bytes(mem[at : at + size]))
                    for at, size in (
                        (C.WIN_LO, self.win_hi - C.WIN_LO),
                        (CODE, self.code_bytes),
                        (CONTROL, 128),
                    )
                ]
                self.owned = True
                mem[CONTROL : CONTROL + len(self.marker)] = self.marker
                image = self.image.ljust(self.code_bytes, b"\0")
                mem[CODE : CODE + self.code_bytes] = image
                for sl in self.models:
                    for off, data in C.setup_writes(sl.plan.plan).items():
                        mem[off : off + len(data)] = data
                        if bytes(mem[off : off + len(data)]) != data:
                            raise RecoveryRequired("Mercury parameter upload differs")
                self._reservation(False)

            self._gate(install)
            self._admin(7)
        except BaseException:
            self.close()
            raise

    def __enter__(self):
        with self.lock:
            self._open()
        return self

    def __exit__(self, *exc):
        self.close()

    def infer(self, feeds, model=0):
        """Run the available 1..3 normalized crops (of extra model k: 1..its crops per
        request) now, preserving their order."""
        sl = self._model_for(model, feeds)
        with self.lock:
            p = self._host_prepare(sl, feeds)
            self._device_run(p)
            outs = self._host_finish(p)
            self.last_timing = p.timing
            return outs

    def infer_many(self, requests):
        """Run several requests [(model, feeds), ...] back to back, in order, overlapping the
        host side with the DSP: while request i runs on the DSP, the waiting thread prepares
        request i+1 and finishes request i-1 in the time it would otherwise sleep (only work
        whose measured cost fits that sleep runs there, so completion is never noticed later
        than by infer; the rest runs right after). One thread: no hand-off or lock contention.
        Each request is its own DSP job (the gate is held per job, as with infer). Returns each
        request's outputs; last_timing holds the per-request timings under "requests". Any
        failure stops the sequence (no retries)."""
        reqs = [(self._model_for(m, f), f) for m, f in requests]
        if not reqs:
            raise ValueError("infer_many needs at least one request")
        lanes, seen = [], {}
        for sl, f in reqs:  # three requests in flight never share a workspace
            key = (sl.slot, len(f))
            lanes.append(seen.get(key, 0) % 3)
            seen[key] = seen.get(key, 0) + 1
        n = len(reqs)
        pend, outs, overlapped = [None] * n, [None] * n, [0]

        def step(kind, j):
            key = (kind, reqs[j][0].slot, len(reqs[j][1]))
            t = time.perf_counter()
            if kind == "prepare":
                pend[j] = self._host_prepare(reqs[j][0], reqs[j][1], lanes[j])
            else:
                outs[j] = self._host_finish(pend[j])
            dt = time.perf_counter() - t
            old = self._host_cost.get(key)
            self._host_cost[key] = dt if old is None else 0.8 * old + 0.2 * dt

        with self.lock:
            step("prepare", 0)
            for i in range(n):
                todo = ([("prepare", i + 1)] if i + 1 < n else []) + ([("finish", i - 1)] if i >= 1 else [])
                sl, count = pend[i].sl, pend[i].count
                budget, inside = 0.8e-3 * sl.crop_ms * count, []  # infer's expected-time sleep
                for kind, j in todo:
                    est = self._host_cost.get((kind, reqs[j][0].slot, len(reqs[j][1])))
                    if est is None or sum(e for _, _, e in inside) + est * 1.25 + 1e-4 > budget:
                        break
                    inside.append((kind, j, est))
                during = (lambda: [step(k, j) for k, j, _ in inside]) if inside else None
                overlapped[0] += len(inside)
                self._device_run(pend[i], during)
                for kind, j in todo[len(inside):]:
                    step(kind, j)
            step("finish", n - 1)
            self.last_timing = {"requests": [p.timing for p in pend],
                                "body_ms": sum(p.timing["body_ms"] for p in pend),
                                "overlapped": overlapped[0], "host_steps": 2 * n}
            return outs

    def infer_frame(self, requests):
        """Run one frame's requests [(model, feeds), ...] (distinct models, each 1..its crops
        per request), in order, as ONE DSP job on a --multi image: each request is a segment of
        the same submission (one gate hold, one wake and completion, one 20 ms budget), so a
        frame that runs the detector and ft7 pays one round trip. Up to MULTI_SEGMENTS requests.
        Without the --multi image the requests run as infer_many. Returns each request's
        outputs; last_timing holds the per-request timings under "requests". Any failure fails
        the whole frame (no retries)."""
        reqs = [(self._model_for(m, f), f) for m, f in requests]
        if not reqs:
            raise ValueError("infer_frame needs at least one request")
        if not self.multi:
            return self.infer_many(requests)
        if len(reqs) > MULTI_SEGMENTS:
            raise ValueError(f"a frame carries at most {MULTI_SEGMENTS} requests")
        if len({sl.slot for sl, _ in reqs}) != len(reqs):
            raise ValueError("a frame's requests are of distinct models")  # one workspace each
        with self.lock:
            if self.stream and len(reqs) == 2:
                p0 = self._host_prepare(*reqs[0])
                p1, early = self._device_run_streamed(p0, *reqs[1])
                pend = [p0, p1]
                outs = [early if early is not None else self._host_finish(p0), self._host_finish(p1)]
            else:
                early = None
                pend = [self._host_prepare(sl, f) for sl, f in reqs]
                self._device_run_frame(pend)
                outs = [self._host_finish(p) for p in pend]
            self.last_timing = {"requests": [p.timing for p in pend],
                                "body_ms": sum(p.timing["body_ms"] for p in pend), "segments": len(pend),
                                "streamed": self.stream and len(reqs) == 2, "early_finish": early is not None}
            return outs

    def _model_for(self, model, feeds):
        sl = self.models[model]
        if not 1 <= len(feeds) <= sl.plan.batch:
            raise ValueError(f"requests of this model carry one to {sl.plan.batch} crops")
        return sl

    def _host_prepare(self, sl, feeds, lane=0):
        """Check and convert one request's inputs (no device access)."""
        import numpy as np

        p = _Pending(sl, len(feeds), self.phase_times)
        if sl.native is not None:
            p.prepared = sl.native.prepare(feeds, lane)
            p.mark("host_prepare_ms")
            return p
        for feed in feeds:
            ts = {}
            for name, shape in sl.model.inputs.items():
                v = np.asarray(feed[name], np.float32)
                if v.shape != shape or not np.isfinite(v).all():
                    raise ValueError(f"{name}: finite values of shape {shape} required")
                ts[name] = v.reshape(shape[1:] if len(shape) > 1 else shape)
            if not np.isfinite(ts["inputImg"].astype(np.float16)).all():
                raise ValueError("image exceeds FP16 storage")
            if sl.model.detector:
                p.extra.append({})
                p.prepared.append({L.STEM_COLS: L.stem_columns(ts["inputImg"], mercury.DET_SIZE)})
                continue
            p.extra.append({k: ts[k] for k in ts.keys() - mercury.INPUTS.keys()})
            for name in L.HOST_HINT_OPS[:3]:
                op = sl.model.op(name)
                ts[op.output] = mercury._step(op, ts, "fp16", "fp16")
            if not np.isfinite(ts[L.HINT]).all():
                raise ValueError("hint exceeds FP16 storage")
            p.prepared.append({L.STEM_COLS: L.stem_columns(ts["inputImg"]), L.HINT: ts[L.HINT]})
        p.mark("host_prepare_ms")
        return p

    def _device_run(self, p, during=None):
        """Stage, run and read back one prepared request (caller holds the lock). during:
        host-only work to run while the DSP computes (Device._submit's wait hook)."""
        self._device_run_frame([p], during)

    def _device_run_frame(self, ps, during=None):
        """Stage, run and read back prepared requests as one DSP job: one request, or on a
        --multi image up to MULTI_SEGMENTS of distinct models (segment k = ps[k])."""
        import numpy as np

        if len(ps) > 1 and not (self.multi and len(ps) <= MULTI_SEGMENTS):
            raise ValueError("this image runs one request per job")
        p = ps[0]
        p.restart()
        self._open()
        if self.power is None:
            self.power = self.device._hold_power()
        p.mark("open_power_ms")

        def stage(mem):
            self._reservation(True)
            for q in ps:
                self._stage(mem, q)

        self._gate(stage)
        p.mark("gate_stage_ms")
        jobs = [q.sl.plan.plan.jobs[0] for q in ps]
        if len(ps) > 1 and any(j.first or j.end != len(q.sl.plan.plan.table) // 128 for j, q in zip(jobs, ps)):
            raise ValueError("a segment runs its whole table")  # the kernel runs segment 1 whole
        job = jobs[0]
        phases = {} if p.marks is not None else None
        resident = self.resident if self.resident_image else None

        def validate(result):
            self._reservation(True)
            if len(ps) == 1:
                p.timing = replay_metadata(result, p.count, job.end - job.first, len(self.image), resident=resident)
            else:
                for q, t in zip(ps, frame_metadata(result, [(q.count, j.end - j.first) for q, j in zip(ps, jobs)],
                                                   len(self.image), resident)):
                    q.timing = t
            for q in ps:
                self._read_outputs(q)

        self.device._submit(
            self.loader,
            b"".join(self._payload(q.count, q.sl) for q in ps),
            opcode=C.RS_OPCODE,
            params=(1, job.first, job.end, 0, 0, 0),
            output_size=C.META_BYTES * len(ps),
            use_direct=True,
            _prepare=lambda t: self._reservation(True),
            _completion_validator=_completion_guard(validate),
            _held_power=self.power,
            _expect_s=0.8e-3 * sum(q.sl.crop_ms * q.count for q in ps),
            _phase_times=phases,
            _while_waiting=during,
        )
        p.mark("submit_ms")
        if phases is not None:
            p.marks["submit"] = phases
        for q in ps:
            q.sl.crop_ms = q.timing["body_ms"] / q.count

    def _stage(self, mem, q):
        """Write one prepared request's inputs into its window part."""
        if q.sl.native is not None:
            q.sl.native.stage(mem, q.prepared)
            return
        for k, ts in enumerate(q.prepared):
            L.stage_inputs(q.sl.plan.crop(k), lambda at, data: mem.__setitem__(slice(at, at + len(data)), data), ts)

    def _read_outputs(self, p):
        """Read and check one request's outputs from the window (into its workspace or p.maps)."""
        import numpy as np

        sl, count, prepared = p.sl, p.count, p.prepared
        get = lambda at, n: bytes(self.device._transport.memory[at : at + n])  # noqa: E731
        p.maps = []
        for k in range(count):
            if sl.native is not None:
                if not sl.native.read(prepared, get, k):
                    raise RecoveryRequired("Mercury output has invalid padding or values")
                continue
            maps = {}
            names = (sl.plan.final,) if sl.model.detector else (L.OUTPUT, L.HEATMAP) if self.heads else (L.OUTPUT,)
            for name in names:
                out, stray = L.read_tensor(sl.plan.crop(k), get, name)
                if stray or not np.isfinite(out).all():
                    raise RecoveryRequired("Mercury output has invalid padding or values")
                maps[name] = out.astype(np.float32)
            p.maps.append(maps)

    def _device_run_streamed(self, p0, sl1, feeds1):
        """A --stream frame of two segments: p0 (prepared) is staged before the job; while the DSP
        runs it, the waiting thread prepares and stages segment 1 (sl1, feeds1), releases it (word
        12 = nonce, a seq-cst store after the staging), then waits for segment 0's completion word
        and reads and finishes its outputs. Returns (segment 1's pending request, segment 0's
        outputs or None when segment 0 had not published in time: then they are read after the
        job like any other). A failure of the early read or finish is left to the normal path."""
        from .profile import PROFILE

        if sl1.slot == p0.sl.slot:
            raise ValueError("a frame's requests are of distinct models")
        t = self.device._transport
        nonce = 0
        while not nonce:
            nonce = struct.unpack("<I", secrets.token_bytes(4))[0]
        ready_at = PROFILE.arena + 128 + 48  # segment 1's request word 12
        done_at = PROFILE.arena + 256 + 13 * 4  # segment 0's meta word 13 (output at 256)
        box = {"p1": None, "early": None, "read0": False, "error": None}
        p0.restart()
        self._open()
        if self.power is None:
            self.power = self.device._hold_power()
        p0.mark("open_power_ms")

        def stage0(mem):
            self._reservation(True)
            self._stage(mem, p0)

        self._gate(stage0)
        p0.mark("gate_stage_ms")
        job0, job1 = p0.sl.plan.plan.jobs[0], sl1.plan.plan.jobs[0]
        for j, sl in ((job0, p0.sl), (job1, sl1)):
            if j.first or j.end != len(sl.plan.plan.table) // 128:
                raise ValueError("a segment runs its whole table")
        phases = {} if p0.marks is not None else None
        resident = self.resident if self.resident_image else None

        def during():
            try:
                p1 = self._host_prepare(sl1, feeds1)
                box["p1"] = p1
                self._reservation(True)
                self._stage(t.memory, p1)
            except BaseException as exc:  # raised after the job, which still completes cleanly
                box["error"] = exc
            # after the staging (seq-cst); on a failure too, so segment 1 runs (on whatever the
            # window holds: its outputs are discarded) instead of stalling the gate for 5 ms
            t.put(ready_at, nonce)
            if box["error"] is not None:
                return
            # segment 0 publishes after its outputs; bounded: else read after the job
            end = time.monotonic() + 2e-3 + 3e-3 * p0.sl.crop_ms * p0.count
            while t.get(done_at) != nonce:
                if time.monotonic() > end:
                    return
                time.sleep(5e-5)
            try:
                self._read_outputs(p0)
                box["read0"] = True
                box["early"] = self._host_finish(p0)
            except RecoveryRequired:
                box["read0"] = False  # validate reads it again and decides
                box["early"] = None

        def validate(result):
            self._reservation(True)
            if box["error"] is not None:
                _completion(result, 2 * C.META_BYTES)  # the job itself completed cleanly
                if isinstance(box["error"], RecoveryRequired):
                    raise box["error"]
                return
            p1 = box["p1"]
            t0, t1 = frame_metadata(result, [(p0.count, job0.end), (p1.count, job1.end)], len(self.image),
                                    resident, done=nonce)
            p0.timing, p1.timing = t0, t1
            if not box["read0"]:
                box["early"] = None
                self._read_outputs(p0)
            self._read_outputs(p1)

        def payload(q_count, sl):
            w = bytearray(self._payload(q_count, sl))
            struct.pack_into("<3I", w, 40, 1, nonce, 0)
            return bytes(w)

        count1 = len(feeds1)
        self.device._submit(
            self.loader,
            payload(p0.count, p0.sl) + payload(count1, sl1),
            opcode=C.RS_OPCODE,
            params=(1, job0.first, job0.end, 0, 0, 0),
            output_size=2 * C.META_BYTES,
            use_direct=True,
            _prepare=lambda t: self._reservation(True),
            _completion_validator=_completion_guard(validate),
            _held_power=self.power,
            _expect_s=0.8e-3 * (p0.sl.crop_ms * p0.count + sl1.crop_ms * count1),
            _phase_times=phases,
            _while_waiting=during,
        )
        if box["error"] is not None:
            raise box["error"]
        p0.mark("submit_ms")
        if phases is not None:
            p0.marks["submit"] = phases
        p1 = box["p1"]
        for q in (p0, p1):
            q.sl.crop_ms = q.timing["body_ms"] / q.count
        if box["early"] is not None and phases is not None:
            p0.timing["phases_ms"] = p0.marks
        return p1, box["early"]

    def _host_finish(self, p):
        """The request's outputs from its read-back maps (no device access)."""
        sl = p.sl
        p.restart()
        if sl.native is not None:
            outs = sl.native.finish(p.prepared)
        elif sl.model.detector:
            # the device ran the convolutions; the dense tail is 8 outputs per crop
            outs = [
                mercury.run_tail(sl.model, m, sl.plan.ops[-1].name, outputs=sl.outputs)
                for m in p.maps
            ]
        elif not self.heads:
            outs = [
                mercury.run_tail(sl.model, m | x, L.LAST, outputs=sl.outputs)
                for m, x in zip(p.maps, p.extra)
            ]
        else:
            # the heatmap head ran on the DSP: only the pooled vector heads remain
            names = sl.outputs if sl.model.views == 1 else sl.outputs + ("heatmap_xy",)
            outs = [
                mercury.run_tail(
                    sl.model,
                    {L.OUTPUT: m[L.OUTPUT], "heatmap_xy": m[L.HEATMAP][:21]} | x,
                    L.LAST,
                    skip=L.HEAD_OPS,
                    outputs=names,
                )
                for m, x in zip(p.maps, p.extra)
            ]
        p.mark("host_finish_ms")
        if p.marks is not None and p.timing is not None:  # a streamed early finish: set after the job
            p.timing["phases_ms"] = p.marks
        return outs

    def _payload(self, count, sl=None):
        """Request words; a resident image adds the tag (words 5..8) and mode (word 9,
        1 | slot << 1 on a --res-slots image)."""
        sl = self.models[0] if sl is None else sl
        words = bytearray(C.payload(sl.plan.plan, count))
        if self.resident_image:
            mode = int(self.resident) | (sl.slot << 1 if self.resident else 0)
            struct.pack_into("<5I", words, 20, *sl.tag, mode)
        return bytes(words)

    def idle(self):
        """Release held clocks when tracking has no observations; keep the plan."""
        with self.lock:
            if self.power is not None:
                power, self.power = self.power, None
                if self.device._transport.poisoned:
                    power.abandon()
                else:
                    self.device._release_power(power)

    def close(self):
        with self.lock:
            if self.closed:
                return
            try:
                self.idle()
                if self.device is not None and self.owned and not self.device._transport.poisoned:
                    if self.sealed:
                        self._admin(8)

                    def restore(mem):
                        self._reservation(False)
                        for off, data in self.saved:
                            mem[off : off + len(data)] = data
                            if bytes(mem[off : off + len(data)]) != data:
                                raise RecoveryRequired("Mercury arena restoration differs")
                        self.owned = False

                    self._gate(restore)
            except BaseException:
                if self.device is not None and self.owned:
                    self.device._transport.poisoned = True
                raise
            finally:
                try:
                    if self.device is not None:
                        self.device.close()
                finally:
                    if self.lease is not None:
                        self.lease.close()
                    self.closed = True
