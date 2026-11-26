"""Offline HMX shape accounting from pinned public weights, optionally calibrated.

Forecasts assume already-packed operands streamed as in hardware_hmx_model.py.
They exclude spatial lowering, graph control, non-convolution operations and
cross-layer conversion; they are neither full-model measurements nor bounds.
"""

import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import statistics
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu.convolution import load_landmark_graph
from framenpu.landmark import specifications
from framenpu.resident_landmark import resident_landmark_plan


def tiles(n):
    return (n + 31) // 32


def group_width(channels, target):
    """Uniform power-of-two channel groups, including C24 and C144."""
    width = target
    while channels % width:
        width //= 2
    return width


def narrow_channels(channels, width):
    return max(8, math.ceil(channels * width / 8) * 8)


def layer_cost(name, channels, outputs, positions, taps, kind, *, row_tiles=None, group=1):
    if kind == "group":
        params = outputs * group * taps
        depth = taps
        tx = tiles(outputs) * tiles(positions)
    else:
        params = outputs * channels * taps
        depth = tiles(channels * taps)
        tx = tiles(outputs) * (tiles(positions) if row_tiles is None else row_tiles)
    return {
        "name": name,
        "kind": kind,
        "group_width": group if kind == "group" else None,
        "logical_macs": params * positions,
        "weight_parameters": params,
        "bias_parameters": outputs,
        "transactions": tx,
        "depth": depth,
        "load_pairs": tx * depth,
        "physical_macs": tx * depth * 32768,
        "nominal_output_half_bytes": 2 * outputs * positions,
    }


def variants(graph):
    plan = resident_landmark_plan(graph)
    records, names = plan["records"], plan["arithmetic_layers"]
    specs = {s["name"]: s for s in specifications(graph)}
    widths = {"fused3_w075": 0.75, "fused3_w050": 0.5, "fused3_w0375": 0.375}
    result = {}
    for variant in (
        "current",
        "diagonal",
        "group8",
        "group16",
        "group32",
        "dense",
        "fused3",
        "fused_same",
        *widths,
    ):
        fused = variant.startswith("fused")
        width = widths.get(variant, 1)
        skip = {
            i - 1
            for i, r in enumerate(records)
            if fused
            and r[0] == 3
            and i
            and records[i - 1][0] == 1
            and records[i - 1][7:10] == r[4:7]
        }
        rows = []
        for i, r in enumerate(records):
            if i in skip or r[0] not in (1, 2, 3):
                continue
            ci, _, _, co, ho, wo = r[4:10]
            if width != 1:
                if r[0] != 2:
                    ci = narrow_channels(ci, width)
                if specs[names[i]]["operator"] != "InnerProduct":
                    co = narrow_channels(co, width)
            taps = r[10] * r[11] if r[0] in (2, 3) else 1
            if r[0] == 3 and variant == "current":
                rows.append(
                    {
                        "name": names[i],
                        "kind": "hvx",
                        "logical_macs": co * ho * wo * taps,
                        "weight_parameters": co * taps,
                        "bias_parameters": co,
                        "transactions": 0,
                        "load_pairs": 0,
                        "physical_macs": 0,
                        "nominal_output_half_bytes": 2 * co * ho * wo,
                    }
                )
                continue
            if r[0] == 3 and (variant in ("diagonal", "group8", "group16", "group32")):
                group = 1 if variant == "diagonal" else group_width(co, int(variant[5:]))
                row = layer_cost(names[i], ci, co, ho * wo, taps, "group", group=group)
            else:
                name = names[i]
                if r[0] == 3 and fused:
                    if i - 1 in skip:
                        ci = (
                            narrow_channels(records[i - 1][4], width)
                            if width != 1
                            else records[i - 1][4]
                        )
                        name = names[i - 1] + "+" + name
                    taps = 9 if variant.startswith("fused3") else taps
                row = layer_cost(
                    name,
                    ci,
                    co,
                    ho * wo,
                    taps,
                    "dense",
                    row_tiles=ho * tiles(wo) if r[0] == 2 else None,
                )
            rows.append(row)
        sums = {
            key: sum(row[key] for row in rows)
            for key in (
                "logical_macs",
                "weight_parameters",
                "bias_parameters",
                "transactions",
                "load_pairs",
                "physical_macs",
                "nominal_output_half_bytes",
            )
        }
        sums["raw_weight_bytes"] = (
            2 * sums["weight_parameters"] + 4 * sums["bias_parameters"] + 4 * len(rows)
        )
        sums["streamed_operand_bytes"] = 4096 * sums["load_pairs"]
        hist = Counter()
        for row in rows:
            if row["transactions"]:
                hist[row["depth"]] += row["transactions"]
        sums["transaction_depth_histogram"] = dict(sorted(hist.items()))
        result[variant] = {
            "totals": sums,
            "layers": rows,
            "removed_expansion_layers": len(skip),
            "requires_retraining": fused,
            "width_multiplier": width,
            "channel_rounding": "internal channels rounded up to multiples of8; RGB input and four head dimensions retained",
            "note": "group variants admit a zero off-diagonal initialization from depthwise; training is needed to exploit added mixing"
            if variant.startswith("group")
            else "",
        }
    if result["current"]["totals"]["raw_weight_bytes"] != len(graph.raw):
        raise ValueError("parameter accounting differs from public binary")
    return plan, result


def calibration(report):
    if not report.get("all_checks_passed"):
        raise ValueError("calibration did not pass")
    cases = []
    for case in report["cases"]:
        modes = {}
        for mode in range(4):
            samples = [s for s in case["samples"] if s["mode"] == mode]
            rounds = sorted({s["round"] for s in samples})
            slopes = []
            for round_ in rounds:
                by_repeat = {s["repeat"]: s["pipeline_us"] for s in samples if s["round"] == round_}
                slopes.append((by_repeat[8] - by_repeat[1]) / 7)
            modes[mode] = {
                "median_us": statistics.median(slopes),
                "min_us": min(slopes),
                "max_us": max(slopes),
            }
        cases.append({"name": case["name"], "depth": case["depth"], "modes": modes})
    fits = {}
    dense = [c for c in cases if c["name"].startswith("dense_")]
    for mode in (1, 2, 3):
        x = [c["depth"] for c in dense]
        y = [c["modes"][mode]["median_us"] for c in dense]
        mx, my = statistics.mean(x), statistics.mean(y)
        slope = sum((a - mx) * (b - my) for a, b in zip(x, y)) / sum((a - mx) ** 2 for a in x)
        intercept = my - slope * mx
        fits[mode] = {
            "intercept_us": intercept,
            "per_depth_us": slope,
            "max_all_fixture_residual_us": max(
                abs(c["modes"][mode]["median_us"] - intercept - slope * c["depth"]) for c in cases
            ),
        }
    return {
        "cases": cases,
        "fits": fits,
        "method": "five within-round (repeat8-repeat1)/7 slopes; median; linear fit to synthetic depths1,9,25; public-weight fixtures cross-check",
    }


def analyze(graph, measured=None):
    plan, costs = variants(graph)
    result = {
        "format": "framenpu-hmx-model-costs",
        "model_sha256": graph.weights_sha256,
        "param_sha256": graph.param_sha256,
        "model_bytes": len(graph.raw),
        "source_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "conditions": "pinned public v20a model; no private images; shapes are exact, runtime forecasts are conditional",
        "existing_arena_bytes": plan["span_bytes"],
        "existing_packed_weight_bytes": plan["weight_bytes"],
        "existing_fp32_activation_span_bytes": plan["high_water_offset"]
        - plan["activation_offset"],
        "variants": costs,
        "limitations": [
            "Prepacked stream forecast excludes spatial lowering, graph control, pooling/residuals/sigmoids, host dispatch and cross-layer layout work.",
            "No coefficient or overlapping activation reuse across transactions is credited; a new dataflow can outperform this streaming implementation.",
            "Raw parameter bytes exclude HMX packing/padding and are not a proof of fit in the existing arena.",
            "Fused variants remove intermediate nonlinearities; fused3 also replaces 5x5 kernels. They require training and accuracy validation.",
            "Resident mode0 repeats the first operand pair at each depth and is only an arithmetic control.",
        ],
    }
    if measured is not None:
        result["calibration"] = calibration(measured)
        for item in costs.values():
            hist = item["totals"]["transaction_depth_histogram"]
            item["max_depth"] = max(hist)
            item["loads_above_measured_depth25"] = sum(
                depth * count for depth, count in hist.items() if depth > 25
            )
            item["streamed_prepacked_forecast_ms"] = {
                mode: (
                    fit["intercept_us"] * item["totals"]["transactions"]
                    + fit["per_depth_us"] * item["totals"]["load_pairs"]
                )
                / 1000
                for mode, fit in result["calibration"]["fits"].items()
            }
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--model", required=True, type=Path)
    p.add_argument("--measurement", type=Path)
    p.add_argument("--output", required=True, type=Path)
    args = p.parse_args()
    graph = load_landmark_graph(args.model / "hand.ncnn.param", args.model / "hand.ncnn.bin")
    report = json.loads(args.measurement.read_text()) if args.measurement else None
    if report and (
        report["model_sha256"] != graph.weights_sha256
        or report["param_sha256"] != graph.param_sha256
    ):
        p.error("measurement model differs")
    result = analyze(graph, report)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({name: item["totals"] for name, item in result["variants"].items()}, indent=2))


if __name__ == "__main__":
    main()
