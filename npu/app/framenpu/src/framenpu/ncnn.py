"""Read-only ncnn graph inventory for planning the FrameTop backend."""

from collections import Counter
from pathlib import Path
from .errors import InvalidModel


def inspect_ncnn(path):
    lines = [line.strip() for line in Path(path).read_text().splitlines() if line.strip()]
    if len(lines) < 2 or lines[0] != "7767517":
        raise InvalidModel("not a text ncnn param file")
    try:
        declared, blobs = map(int, lines[1].split())
        if declared != len(lines) - 2:
            raise InvalidModel("ncnn layer count does not match the file")
        inventory = Counter()
        for line in lines[2:]:
            parts = line.split()
            op, name = parts[:2]
            bottom, top = map(int, parts[2:4])
            if bottom < 0 or top < 0 or len(parts) < 4 + bottom + top:
                raise InvalidModel("invalid ncnn layer declaration")
            inventory[op] += 1
    except (ValueError, IndexError) as exc:
        raise InvalidModel("invalid ncnn layer table") from exc
    mapping = {"Input": "input tensor", "InnerProduct": "Dense", "ReLU": "ReLU", "Clip": "Clip"}
    return {
        "format": "ncnn-param",
        "layers": declared,
        "blobs": blobs,
        "native_ncnn_loading": False,
        "operator_counts": dict(sorted(inventory.items())),
        "candidate_mappings": {op: mapping[op] for op in inventory if op in mapping},
        "missing_operators": {op: n for op, n in sorted(inventory.items()) if op not in mapping},
        "limitations": [
            "Graph inventory only; no ncnn binary weights conversion or graph execution",
            "Fused activation, broadcasting, shape and padding semantics require conversion validation",
        ],
    }
