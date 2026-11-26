"""Offline storage planning for a proposed resident executor; no device access."""

import json
from pathlib import Path
import re

from .profile import PROFILE

# Firmware-profile-specific DDR offsets, qualified in docs/MEMMAP.md. These
# are separate from the borrowed per-job code/header/arena in PROFILE.
ARENA_START = 0x1A22000
ARENA_END = 0x2000000
ARENA_BYTES = ARENA_END - ARENA_START
CACHE_ALIGNMENT = 128
DTYPES = {"u8": 1, "f16": 2, "f32": 4}
MAX_ITEMS = 1024


def _integer(value, label, low, high):
    if type(value) is not int or not low <= value <= high:
        raise ValueError(f"{label} must be an integer in {low}..{high}")
    return value


def _name(value, label):
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9.-]{0,63}", value):
        raise ValueError(f"{label} must be a name of 1..64 ASCII letters/digits/_.-")
    return value


def _object(value, required, optional, label):
    if not isinstance(value, dict) or not required <= value.keys():
        raise ValueError(f"{label} is missing required fields: {', '.join(sorted(required))}")
    if value.keys() - required - optional:
        raise ValueError(f"{label} has unknown fields")


def _names(value, label, *, empty=True):
    if not isinstance(value, list) or not (0 if empty else 1) <= len(value) <= MAX_ITEMS:
        raise ValueError(f"{label} must be a list of at most {MAX_ITEMS} names")
    names = [_name(item, label) for item in value]
    if len(set(names)) != len(names):
        raise ValueError(f"{label} contains duplicate names")
    return names


def _alignment(value):
    value = _integer(value, "alignment", CACHE_ALIGNMENT, 4096)
    if value & (value - 1):
        raise ValueError("alignment must be a power of two")
    return value


def _align(value, alignment):
    return (value + alignment - 1) & -alignment


def _tensor(value):
    _object(
        value,
        {"name", "kind", "shape", "dtype", "layout"},
        {"strides", "alignment", "alias_of"},
        "tensor",
    )
    name = _name(value["name"], "tensor name")
    if value["kind"] not in ("input", "weight", "activation"):
        raise ValueError(f"{name}: kind must be input, weight or activation")
    dtype = value["dtype"]
    if not isinstance(dtype, str) or dtype not in DTYPES:
        raise ValueError(f"{name}: dtype must be u8, f16 or f32")
    layout = _name(value["layout"], f"{name} layout")
    shape = value["shape"]
    if not isinstance(shape, list) or not 1 <= len(shape) <= 8:
        raise ValueError(f"{name}: shape must have 1..8 dimensions")
    shape = [_integer(size, f"{name} dimension", 1, ARENA_BYTES) for size in shape]
    item_bytes = DTYPES[dtype]
    contiguous = [item_bytes]
    for size in reversed(shape[1:]):
        contiguous.insert(0, contiguous[0] * size)
    strides = value.get("strides", contiguous)
    if not isinstance(strides, list) or len(strides) != len(shape):
        raise ValueError(f"{name}: strides must match rank, in bytes")
    strides = [_integer(s, f"{name} stride", item_bytes, ARENA_BYTES) for s in strides]
    if any(s % item_bytes for s in strides) or any(
        strides[i] < strides[i + 1] * shape[i + 1] for i in range(len(shape) - 1)
    ):
        raise ValueError(f"{name}: strides must be aligned and nonoverlapping in row-major order")
    size = item_bytes + sum((dim - 1) * stride for dim, stride in zip(shape, strides))
    if size > ARENA_BYTES:
        raise ValueError(f"{name}: tensor exceeds the resident arena")
    alias = value.get("alias_of")
    if "alias_of" in value:
        _name(alias, f"{name} alias_of")
        if value["kind"] != "activation":
            raise ValueError(f"{name}: only activation descriptors may alias storage")
    return {
        "name": name,
        "kind": value["kind"],
        "shape": shape,
        "dtype": dtype,
        "layout": layout,
        "strides": strides,
        "bytes": size,
        "alignment": _alignment(value.get("alignment", CACHE_ALIGNMENT)),
        "alias_of": alias,
        "contiguous": strides == contiguous,
    }


def plan_resident(spec):
    """Validate a declarative storage schedule and return a deterministic map.

    This does not infer shapes, validate operator math, acquire ownership, load
    weights, or make a plan executable. Lifetimes are half-open step intervals;
    a step's inputs, outputs and scratch overlap. Whole-buffer aliases extend
    their root's lifetime and never authorize in-place mutation.
    """
    _object(
        spec,
        {"format", "version", "name", "tensors", "steps", "outputs"},
        {"arena_bytes", "code_bytes", "control_bytes"},
        "resident plan",
    )
    if (
        spec["format"] != "framenpu-resident-plan"
        or type(spec["version"]) is not int
        or spec["version"] != 1
    ):
        raise ValueError("unsupported resident plan format/version")
    name = _name(spec["name"], "plan name")
    capacity = _integer(spec.get("arena_bytes", ARENA_BYTES), "arena_bytes", 128, ARENA_BYTES)
    code_bytes = _integer(
        spec.get("code_bytes", PROFILE.code_size), "code_bytes", 1, PROFILE.code_size
    )
    control_bytes = _integer(spec.get("control_bytes", 4096), "control_bytes", 0, ARENA_BYTES)
    if not isinstance(spec["tensors"], list) or not 1 <= len(spec["tensors"]) <= MAX_ITEMS:
        raise ValueError(f"require 1..{MAX_ITEMS} tensors")
    if not isinstance(spec["steps"], list) or not 1 <= len(spec["steps"]) <= MAX_ITEMS:
        raise ValueError(f"require 1..{MAX_ITEMS} ordered steps")
    tensors = {}
    for entry in spec["tensors"]:
        tensor = _tensor(entry)
        if tensor["name"] in tensors:
            raise ValueError(f"duplicate tensor: {tensor['name']}")
        tensors[tensor["name"]] = tensor

    # Resolve iteratively so valid long alias chains cannot exhaust Python's
    # call stack. Forward declarations are allowed; cycles are rejected.
    roots = {}
    for tensor in tensors.values():
        path, seen, current = [], set(), tensor["name"]
        while current not in roots:
            if current not in tensors:
                raise ValueError(f"unknown alias target: {current}")
            if current in seen:
                raise ValueError("alias cycle")
            seen.add(current)
            path.append(current)
            parent = tensors[current]["alias_of"]
            if parent is None:
                roots[current] = current
                break
            current = parent
        for item in path:
            roots[item] = roots[current]
    for tensor in tensors.values():
        root = tensors[roots[tensor["name"]]]
        if tensor["alias_of"] is None:
            continue
        same_view = (tensor["shape"], tensor["strides"]) == (root["shape"], root["strides"])
        if (
            tensor["dtype"] != root["dtype"]
            or tensor["bytes"] != root["bytes"]
            or not (same_view or tensor["contiguous"] and root["contiguous"])
        ):
            raise ValueError(
                f"{tensor['name']}: aliases must be whole-buffer views of the same dtype"
            )
        root["alignment"] = max(root["alignment"], tensor["alignment"])

    stop = len(spec["steps"]) + 2  # setup=0, operations=1..N, readback=N+1
    births = {key: 0 for key, t in tensors.items() if t["kind"] in ("input", "weight")}
    last_use = dict(births)
    steps, step_names = [], set()
    for index, entry in enumerate(spec["steps"], 1):
        _object(entry, {"name", "inputs", "outputs"}, {"scratch_bytes"}, "step")
        step_name = _name(entry["name"], "step name")
        if step_name in step_names:
            raise ValueError(f"duplicate step: {step_name}")
        step_names.add(step_name)
        inputs = _names(entry["inputs"], f"{step_name} inputs")
        outputs = _names(entry["outputs"], f"{step_name} outputs", empty=False)
        for key in inputs:
            if key not in roots or roots[key] not in births:
                raise ValueError(f"{step_name}: read before definition: {key}")
            last_use[roots[key]] = index
        for key in outputs:
            if key not in tensors:
                raise ValueError(f"{step_name}: unknown output: {key}")
            if tensors[key]["alias_of"] is not None or key in births:
                raise ValueError(f"{step_name}: output must define fresh activation storage: {key}")
            births[key] = last_use[key] = index
        scratch = _integer(
            entry.get("scratch_bytes", 0), f"{step_name} scratch_bytes", 0, ARENA_BYTES
        )
        steps.append(
            {
                "index": index,
                "name": step_name,
                "inputs": inputs,
                "outputs": outputs,
                "scratch_bytes": scratch,
            }
        )
    for key in set(roots.values()):
        if key not in births:
            raise ValueError(f"activation has no producer: {key}")
    outputs = _names(spec["outputs"], "plan outputs", empty=False)
    for key in outputs:
        if key not in roots:
            raise ValueError(f"unknown plan output: {key}")
        last_use[roots[key]] = stop - 1

    requests = []

    def request(key, kind, size, start, end, alignment=CACHE_ALIGNMENT):
        if size:
            requests.append(
                {
                    "name": key,
                    "kind": kind,
                    "bytes": size,
                    "reserved_bytes": _align(size, CACHE_ALIGNMENT),
                    "alignment": alignment,
                    "lifetime": [start, end],
                }
            )

    request("@code", "code_image", code_bytes, 0, stop)
    request("@control", "control_reserve", control_bytes, 0, stop)
    # Place session-long objects first, then schedule-ordered transient storage.
    for tensor in sorted(tensors.values(), key=lambda t: t["kind"] != "weight"):
        key = tensor["name"]
        if roots[key] != key:
            continue
        end = stop if tensor["kind"] == "weight" else last_use[key] + 1
        request(key, tensor["kind"], tensor["bytes"], births[key], end, tensor["alignment"])
    for step in steps:
        request(
            f"@scratch:{step['name']}",
            "scratch",
            step["scratch_bytes"],
            step["index"],
            step["index"] + 1,
        )

    active, allocations, required, peak = [], [], 0, 0
    for entry in sorted(requests, key=lambda r: r["lifetime"][0]):
        start = entry["lifetime"][0]
        active = [item for item in active if item["lifetime"][1] > start]
        offset = _align(ARENA_START, entry["alignment"])
        for item in sorted(active, key=lambda a: a["offset"]):
            if offset + entry["reserved_bytes"] <= item["offset"]:
                break
            offset = _align(item["offset"] + item["reserved_bytes"], entry["alignment"])
        end = offset + entry["reserved_bytes"]
        if end > ARENA_START + capacity:
            raise ValueError(
                f"resident placement exceeds {capacity} bytes at {entry['name']}; reduce storage or change the schedule"
            )
        allocation = dict(entry, offset=offset)
        active.append(allocation)
        allocations.append(allocation)
        required = max(required, end - ARENA_START)
        peak = max(peak, sum(item["reserved_bytes"] for item in active))
    without_reuse = ARENA_START
    for allocation in allocations:
        without_reuse = (
            _align(without_reuse, allocation["alignment"]) + allocation["reserved_bytes"]
        )
    by_name = {allocation["name"]: allocation for allocation in allocations}
    descriptors = []
    for tensor in tensors.values():
        root = by_name[roots[tensor["name"]]]
        descriptors.append(
            {
                key: tensor[key]
                for key in (
                    "name",
                    "kind",
                    "shape",
                    "dtype",
                    "layout",
                    "strides",
                    "bytes",
                    "alias_of",
                )
            }
            | {
                "allocation": root["name"],
                "offset": root["offset"],
                "alignment": tensor["alignment"],
                "lifetime": root["lifetime"],
            }
        )
    return {
        "format": "framenpu-resident-layout",
        "version": 1,
        "name": name,
        "execution_supported": False,
        "firmware_sha256": PROFILE.firmware_sha256,
        "arena_offset": ARENA_START,
        "arena_limit": ARENA_START + capacity,
        "arena_bytes": capacity,
        "required_bytes": required,
        "peak_live_bytes": peak,
        "placement_overhead_bytes": required - peak,
        "without_reuse_bytes": without_reuse - ARENA_START,
        "headroom_bytes": capacity - required,
        "allocations": allocations,
        "tensors": descriptors,
        "steps": steps,
        "outputs": outputs,
    }


def load_resident_plan(path):
    """Read a bounded JSON schedule and plan it without opening a device."""
    with Path(path).open("rb") as stream:
        raw = stream.read(2 * 1024 * 1024 + 1)
    if len(raw) > 2 * 1024 * 1024:
        raise ValueError("resident plan exceeds 2 MiB")

    def unique_fields(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("duplicate JSON field")
            result[key] = value
        return result

    try:
        spec = json.loads(raw, object_pairs_hook=unique_fields)
    except (ValueError, RecursionError) as exc:
        raise ValueError("invalid resident plan JSON") from exc
    return plan_resident(spec)
