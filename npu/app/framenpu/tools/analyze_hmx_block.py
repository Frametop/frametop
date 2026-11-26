"""Summarize a fused HMX spatial-block measurement and forecast the
half-width fused3 student's convolution time from measured unit costs.

Per geometry: per-block time from the repeat slope and from phase ticks,
unit costs (per weight tile, per gathered K tile, per HMX load, per
transaction, per spilled tile), and the effect of copy batching, stage-2
weight residency and instrumentation. The forecast multiplies unit costs by
exact layer counts; stride-2 and RGB-stem layers reuse stride-1 unit costs
and are flagged as unmeasured. It is a planning estimate, not a bound.
"""

import argparse
import json
from pathlib import Path
import statistics
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "tools"))
from hmx_block_layout import BY_NAME

PHASES = ("weights", "stage", "pack", "hmx", "post", "spill", "store", "setup")
TICKS_PER_US = 19.2


def _per_block(samples, g, variant, reps):
    rows = [s for s in samples if s["variant"] == variant and s["reps"] == reps]
    return rows, reps * g.nj


def variant_summary(case, g, variant):
    samples = case["samples"]
    lo, hi = case["repeat_counts"]
    rows_hi, blocks_hi = _per_block(samples, g, variant, hi)
    if not rows_hi:
        return None
    result = {
        "block_us_median": statistics.median(s["body_ticks"] / blocks_hi for s in rows_hi)
        / TICKS_PER_US,
    }
    if hi > lo:
        slopes = []
        for round_ in sorted({s["round"] for s in samples}):
            by = {
                s["reps"]: s["body_ticks"]
                for s in samples
                if s["variant"] == variant and s["round"] == round_
            }
            if lo in by and hi in by:
                slopes.append((by[hi] - by[lo]) / ((hi - lo) * g.nj) / TICKS_PER_US)
        result["block_us_slope_median"] = statistics.median(slopes)
    result["phase_us_per_block"] = {
        p: statistics.median(s["phase_ticks"][p] / blocks_hi for s in rows_hi) / TICKS_PER_US
        for p in PHASES
    }
    return result


def unit_costs(g, phases, reuse_a2):
    """Per-unit microseconds from one geometry's per-block phase times."""
    weight_tiles = g.nm * g.nk1 + (0 if reuse_a2 and g.nm * g.no == 1 else g.nm * g.no)
    gathered = g.nm * g.nk1  # gather is repeated for each mid block
    loads = g.k_tiles_per_block
    spilled = g.nm + g.nm * g.no if g.nm > 1 else 0
    return {
        "weight_tile_us": phases["weights"] / weight_tiles,
        "gather_k_tile_us": (phases["stage"] + phases["pack"]) / gathered,
        "stage_k_tile_us": phases["stage"] / gathered,
        "pack_k_tile_us": phases["pack"] / gathered,
        "hmx_per_load_us": phases["hmx"] / loads,
        "post_per_mid_tile_us": phases["post"] / g.nm,
        "store_per_out_tile_us": phases["store"] / g.no,
        "spill_tile_us": phases["spill"] / spilled if spilled else None,
        "setup_per_block_us": phases["setup"],
    }


def summarize(report):
    if not report.get("all_checks_passed"):
        raise ValueError("measurement did not pass its checks")
    out = {}
    for case in report["cases"]:
        g = BY_NAME[case["geometry"]]
        key = f"{g.name}/{case['fixture']}"
        variants = {
            v: variant_summary(case, g, v) for v in ("main", "copy1", "notime", "noreuse", "nohmx")
        }
        variants = {k: v for k, v in variants.items() if v}
        main = variants["main"]
        item = {"variants": variants, "units": unit_costs(g, main["phase_us_per_block"], True)}
        phase_sum = sum(main["phase_us_per_block"].values())
        item["phase_share"] = {p: v / phase_sum for p, v in main["phase_us_per_block"].items()}
        if "notime" in variants:
            item["instrumentation_overhead"] = (
                main["block_us_median"] / variants["notime"]["block_us_median"] - 1
            )
        if "copy1" in variants:
            c = variants["copy1"]["phase_us_per_block"]
            m = main["phase_us_per_block"]
            item["batched_copy_speedup"] = {
                "weights": c["weights"] / m["weights"] if m["weights"] else None,
                "stage": c["stage"] / m["stage"] if m["stage"] else None,
            }
        if "nohmx" in variants:
            # Same data movement with no HMX instruction: the difference per
            # load is HMX execution, including stalls of later VTCM writes.
            item["hmx_inclusive_us_per_load"] = (
                main["block_us_median"] - variants["nohmx"]["block_us_median"]
            ) / g.k_tiles_per_block
            item["nohmx_phase_us_per_block"] = variants["nohmx"]["phase_us_per_block"]
        if "noreuse" in variants:
            item["stage2_weight_residency_saving_us_per_block"] = (
                variants["noreuse"]["block_us_median"] - main["block_us_median"]
            )
        item["resident"] = resident_summary(case, g)
        out[key] = item
    return out


RESIDENT = ("res1", "res2", "res2_notime")


def _phase_slope(case, g, variant, phase):
    lo, hi = case["repeat_counts"]
    vals = []
    for round_ in sorted({s["round"] for s in case["samples"]}):
        by = {
            s["reps"]: s
            for s in case["samples"]
            if s["variant"] == variant and s["round"] == round_
        }
        if lo in by and hi in by:
            a, b = by[lo], by[hi]
            x = (
                (lambda s: s["body_ticks"])
                if phase is None
                else (lambda s: s["phase_ticks"][phase])
            )
            vals.append((x(b) - x(a)) / ((hi - lo) * g.nj) / TICKS_PER_US)
    return statistics.median(vals)


def resident_summary(case, g):
    """VTCM-resident v2: steady per-block cost (repeat slope, prologue cancels)
    and the once-per-job prologue (weights and source band copied into VTCM)."""
    lo, hi = case["repeat_counts"]
    if hi <= lo or not any(s["variant"] == "res2" for s in case["samples"]):
        return None
    out = {"block_us_slope": {v: _phase_slope(case, g, v, None) for v in RESIDENT}}
    out["block_phase_us"] = {p: _phase_slope(case, g, "res2", p) for p in PHASES}
    lo_rows = [s for s in case["samples"] if s["variant"] == "res2" and s["reps"] == lo]
    prologue = {
        p: statistics.median(s["phase_ticks"][p] for s in lo_rows) / TICKS_PER_US
        for p in ("weights", "stage")
    }
    out["prologue_us"] = prologue
    out["prologue_us_notime_estimate"] = statistics.median(
        s["body_ticks"] / TICKS_PER_US - lo * g.nj * out["block_us_slope"]["res2_notime"]
        for s in case["samples"]
        if s["variant"] == "res2_notime" and s["reps"] == lo
    )
    timing = out["block_us_slope"]["res2_notime"] / out["block_us_slope"]["res2"]
    ph = out["block_phase_us"]
    loads = g.k_tiles_per_block
    weight_tiles = g.nm * g.nk1 + g.no * g.nm
    source_vectors = (g.cin // 2) * g.nbin + g.nj
    out["units_notime"] = {
        "pack_k_tile_us": ph["pack"] * timing / g.nk1,
        "hmx_per_load_us": ph["hmx"] * timing / loads,
        "post_per_tile_us": (ph["post"] + ph["store"]) * timing / (g.nm + g.no),
        "setup_per_block_us": ph["setup"] * timing,
        "prologue_weight_tile_us": prologue["weights"] / weight_tiles,
        "prologue_source_vector_us": prologue["stage"] / source_vectors,
    }
    v1 = _phase_slope(case, g, "notime", None)
    out["speedup_vs_v1_notime"] = v1 / out["block_us_slope"]["res2_notime"]
    out["pair_vs_single"] = out["block_us_slope"]["res1"] / out["block_us_slope"]["res2"]
    return out


def forecast_resident(summary, graph):
    """fused3_w050 convolution ms with v2 unit costs: every layer's weights
    copied into VTCM once, B tiles packed once per position block from a
    VTCM-resident source, mid/output tiles kept in VTCM."""
    from analyze_hmx_model import variants

    _, costs = variants(graph)
    rows = costs["fused3_w050"]["layers"]
    units = [
        v["resident"]["units_notime"]
        for k, v in summary.items()
        if k.startswith("w050_") and v.get("resident")
    ]
    med = {k: statistics.median(u[k] for u in units) for k in units[0]}
    parts = dict.fromkeys(("weights_in", "source_in", "pack", "hmx", "post"), 0.0)
    for r in rows:
        spatial = "+" in r["name"] or r["name"] in ("convclip_0", "convdwclip_0")
        outputs = r["bias_parameters"]
        positions = r["nominal_output_half_bytes"] // (2 * outputs)
        blocks = -(-positions // 32)
        parts["weights_in"] += -(-outputs // 32) * r["depth"] * med["prologue_weight_tile_us"]
        parts["hmx"] += r["load_pairs"] * med["hmx_per_load_us"]
        parts["post"] += r["transactions"] * med["post_per_tile_us"]
        if spatial:
            parts["pack"] += blocks * r["depth"] * med["pack_k_tile_us"]
            cin_pairs = -(-(r["depth"] * 32 // 9) // 2)
            parts["source_in"] += cin_pairs * (blocks + 4) * med["prologue_source_vector_us"]
    return {
        "variant": "fused3_w050",
        "unit_costs_us": med,
        "parts_ms": {k: v / 1000 for k, v in parts.items()},
        "convolution_ms": sum(parts.values()) / 1000,
        "decision_gate_ms": [12, 20],
        "assumptions": [
            "Unit costs are timer-free (scaled by res2_notime/res2 slope) medians of the w050 geometries.",
            "1x1 layers read their B operand as resident activation tiles (no repacking).",
            "Source band: 9-tap 3x3 assumed for the channel count of spatial layers; stride-2 "
            "and RGB-stem layers reuse stride-1 costs (not measured).",
            "One prologue per layer; job splitting, the 128 KiB arena and DDR inter-layer traffic "
            "of a real executor are not modelled. Residual adds, pooling, heads and host work excluded.",
        ],
    }


def forecast(summary, graph):
    """fused3_w050 convolution ms from the median w050 unit costs."""
    from analyze_hmx_model import variants
    from framenpu.landmark import specifications

    specs = {s["name"]: s for s in specifications(graph)}
    _, costs = variants(graph)
    rows = costs["fused3_w050"]["layers"]
    units = [v["units"] for k, v in summary.items() if k.startswith("w050_")]
    med = {
        k: statistics.median(u[k] for u in units if u[k] is not None)
        for k in units[0]
        if any(u[k] is not None for u in units)
    }
    total = no_weights = 0.0
    unmeasured = []
    for r in rows:
        spatial = "+" in r["name"] or r["name"] in ("convclip_0", "convdwclip_0")
        loads, tx = r["load_pairs"], r["transactions"]
        source = med["gather_k_tile_us"] if spatial else med.get("spill_tile_us", 0.0)
        tile = med["weight_tile_us"] + source + med["hmx_per_load_us"]
        cost = loads * tile + tx * med["post_per_mid_tile_us"]
        total += cost
        no_weights += cost - loads * med["weight_tile_us"]
        last = specs.get(r["name"].split("+")[-1], {})
        if spatial and (r["name"] == "convclip_0" or tuple(last.get("stride", (1, 1))) != (1, 1)):
            unmeasured.append(r["name"])
    return {
        "variant": "fused3_w050",
        "unit_costs_us": med,
        "convolution_ms": total / 1000,
        "convolution_ms_if_weights_were_vtcm_resident": no_weights / 1000,
        "decision_gate_ms": [12, 20],
        "approximated_layers": unmeasured,
        "assumptions": [
            "1x1 layers read their B operand as a prepacked tile copy (spill-reload unit cost).",
            "Stride-2 and RGB-stem layers reuse stride-1 gather costs (not measured).",
            "Residual adds, pooling, sigmoid heads, graph control and host work excluded.",
            "Weight-resident figure is a counterfactual: 8 KiB VTCM cannot hold layer weights.",
        ],
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--measurement", required=True, type=Path)
    p.add_argument("--model", type=Path)
    p.add_argument("--output", required=True, type=Path)
    args = p.parse_args()
    report = json.loads(args.measurement.read_text())
    result = {"format": "framenpu-hmx-block-summary", "geometries": summarize(report)}
    if args.model:
        from framenpu.convolution import load_landmark_graph

        graph = load_landmark_graph(args.model / "hand.ncnn.param", args.model / "hand.ncnn.bin")
        if graph.weights_sha256 != report["model_sha256"]:
            p.error("measurement model differs")
        result["forecast"] = forecast(result["geometries"], graph)
        result["forecast_resident"] = forecast_resident(result["geometries"], graph)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result.get("forecast_resident", result["geometries"]), indent=2))


if __name__ == "__main__":
    main()
