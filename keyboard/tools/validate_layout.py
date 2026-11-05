#!/usr/bin/env python3
"""Validate bundled layout geometry before embedding the JSON resources."""
import argparse
import json
import math
from pathlib import Path
import sys


def text(value, field, *, empty=False):
    if not isinstance(value, str) or (not empty and not value) or any(ord(c) < 32 for c in value):
        raise ValueError(f"{field}: expected printable text")
    return value


def number(value, field, *, positive=False):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise ValueError(f"{field}: expected a finite number")
    if value < 0 or (positive and value == 0):
        raise ValueError(f"{field}: invalid dimension or position")
    return value


def validate(data):
    if not isinstance(data, dict) or data.get("schema_version") != 1:
        raise ValueError("expected layout schema_version 1")
    text(data["id"], "id")
    text(data["name"], "name")
    width = number(data["width"], "width", positive=True)
    height = number(data["height"], "height", positive=True)
    if not isinstance(data["keys"], list) or not data["keys"]:
        raise ValueError("keys must be a non-empty array")
    ids = set()
    rects = []
    for key in data["keys"]:
        ident = text(key["id"], "key.id")
        if ident in ids:
            raise ValueError(f"duplicate key ID: {ident}")
        ids.add(ident)
        text(key["label"], ident + ".label", empty=True)
        text(key.get("secondary_label", ""), ident + ".secondary_label", empty=True)
        x = number(key["x"], ident + ".x")
        y = number(key["y"], ident + ".y")
        w = number(key["width"], ident + ".width", positive=True)
        h = number(key["height"], ident + ".height", positive=True)
        if x + w > width + 0.001 or y + h > height + 0.001:
            raise ValueError(f"{ident}: outside the design area")
        action = key["action"]
        if action["kind"] not in ("key", "shortcut"):
            raise ValueError(f"{ident}: unsupported action kind")
        text(action["value"], ident + ".action.value")
        if action["kind"] == "shortcut" and action["value"] not in ("copy", "paste"):
            raise ValueError(f"{ident}: unsupported shortcut")
        for other, ox, oy, ow, oh in rects:
            if min(x + w, ox + ow) - max(x, ox) > 0.001 and min(y + h, oy + oh) - max(y, oy) > 0.001:
                raise ValueError(f"overlapping hit areas: {ident} and {other}")
        rects.append((ident, x, y, w, h))
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("layouts", type=Path, nargs="+")
    args = parser.parse_args()
    try:
        for path in args.layouts:
            data = validate(json.loads(path.read_text(encoding="utf-8")))
            print(f'Valid: {data["name"]}, {len(data["keys"])} keys, no overlapping hit areas')
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"Layout error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
