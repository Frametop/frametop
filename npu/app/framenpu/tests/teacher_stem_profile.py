"""Profiler protocol, timestamp validation, and exact band-work census. No device."""

import teacher_resident_layout as R
import teacher_stem_layout as S
import teacher_tail_layout as T

MAGIC, VECTORS, VTCM = 0x31505354, 128, 14016
FIELDS = (
    "parameter_copy",
    "band_clear",
    "operator",
    "packing",
    "hmx",
    "coefficient_splat",
    "tap_reduction",
    "post_store",
)
GLOBALS = {
    7: "rgb_load",
    8: "output_clear",
    9: "out8_ddr_store",
    10: "out10_ddr_store",
    11: "profile_log_copy",
}
FINE_FIELDS = (*FIELDS[3:], "packing_fence")


def descriptor(plan, level, *, band=None, phase=0):
    import struct

    row = list(struct.unpack("<32I", plan.table))
    if band is not None:
        row[1:3] = [band, band + 1]
    row[18:21] = [R._al(plan.end), level, phase]
    if not native_ok(row):
        raise ValueError("invalid profiling descriptor")
    return struct.pack("<32I", *row)


def native_ok(row):
    r = [int(v) for v in row]
    if len(r) != 32 or any(r[21:]):
        return False
    base = r.copy()
    base[18:21] = [0, 0, 0]
    if not S.native_ok(base):
        return False
    if not r[19]:
        return not r[18] and not r[20]
    if r[19] not in (1, 2, 3) or r[20] > 7 or r[15] or r[16] or (r[19] == 3 and r[20]):
        return False
    if r[19] >= 2 and r[2] != r[1] + 1:
        return False
    at = r[18]
    if not T.WIN_LO <= at <= T.WIN_HI or at % 128 or VECTORS > (T.WIN_HI - at) // 128:
        return False
    return not any(
        at < b + n * 128 and b < at + VECTORS * 128
        for b, n in ((r[5], 2352), (r[6], S.PARAMS), (r[9], 3616), (r[10], 904))
    )


def kept(stage, j):
    if stage != 4:
        return True
    a, b = (32 * j) // 114, (32 * j + 31) // 114
    return (2 <= a <= 16 and a % 2 == 0) or (2 <= b <= 16 and b % 2 == 0)


def block_class(j):
    a, b = (32 * j) // 114, (32 * j + 31) // 114
    if 1 <= a and b <= 16:
        return "core"
    if b < 1 or a > 16:
        return "halo"
    return "mixed"


def census():
    """Exact executed output slots, real duplicates and unused DW1 lanes."""
    result = {}
    for stage, name in enumerate(S.NAMES):
        unique, real, core, invalid, slots = set(), 0, 0, 0, 0
        blocks = {k: 0 for k in ("core", "halo", "mixed")}
        for band in range(7):
            for j in range(72):
                if not kept(stage, j):
                    continue
                blocks[block_class(j)] += 1
                for lane in range(32):
                    y, x = divmod(32 * j + lane, 114)
                    x -= 2
                    gy = 16 * band - 1 + y
                    slots += 1
                    if y < 20 and 0 <= gy < 112 and 0 <= x < 112:
                        real += 1
                        unique.add((gy, x))
                        if 1 <= y <= 16:
                            core += 1
                    else:
                        invalid += 1
        result[name] = {
            "executed_blocks": sum(blocks.values()),
            "block_classes": blocks,
            "executed_position_slots": slots,
            "valid_position_evaluations": real,
            "unique_valid_positions": len(unique),
            "repeated_valid_positions": real - len(unique),
            "invalid_padding_slots": invalid,
            "core_position_evaluations": core,
        }
        if stage == 4:
            result[name] |= {
                "required_output_positions": 56 * 56,
                "unused_executed_slots": slots - 56 * 56,
                "required_lane_fraction": 56 * 56 / slots,
            }
    return result


def decode(raw, row, body_ms):
    import numpy as np

    r = [int(v) for v in row]
    v = np.frombuffer(raw, "<u4")
    if len(v) != 4096 or list(v[:6]) != [MAGIC, 2, r[19], r[20], r[1], r[2]]:
        raise ValueError("profiling log identity differs")
    if not 0 < int(v[6]) <= body_ms * 19200 or v[13] != 256 or not v[14] <= v[12] / 256 <= v[15]:
        raise ValueError("profiling log timing/calibration differs")
    if np.any(v[16:64]) or np.any(v[64 + 7 * 448 :]):
        raise ValueError("profiling log reserved words differ")
    bands = {}
    accounted = sum(int(v[i]) for i in (7, 8, 9, 10))
    for band in range(7):
        b = v[64 + band * 448 : 64 + (band + 1) * 448]
        if not r[1] <= band < r[2]:
            if np.any(b):
                raise ValueError("profiling log wrote an unrequested band")
            continue
        stages = {}
        total = int(b[400]) + int(b[401])
        for s, name in enumerate(S.NAMES):
            values = [int(x) for x in b[80 * s : 80 * s + 80]]
            if min(values[:3]) <= 0:
                raise ValueError("profiling stage timestamps missing")
            blocks = values[8:]
            fence = int(b[403 + s])
            for j, dt in enumerate(blocks):
                wanted = (r[19] == 3 or (r[19] == 2 and j % 8 == r[20])) and kept(s, j)
                if bool(dt) != wanted:
                    raise ValueError("profiling sample coverage differs")
            if r[19] == 1 and (any(values[3:]) or fence):
                raise ValueError("coarse mode contains detailed timestamps")
            if r[19] >= 2 and sum(values[i] for i in (3, 4, 6, 7)) + fence != sum(blocks):
                raise ValueError("sampled component accounting differs")
            stages[name] = dict(zip(FIELDS, values[:8])) | {
                "blocks": blocks,
                "packing_fence": fence,
            }
            total += sum(values[:3])
        if np.any(b[408:]) or not 0 < total <= int(b[402]):
            raise ValueError("band accounting/reserved words differ")
        accounted += int(b[402])
        bands[str(band)] = {
            "stages": stages,
            "maxpool": int(b[400]),
            "decimation": int(b[401]),
            "total": int(b[402]),
            "residual": int(b[402]) - total,
        }
    if accounted > v[6]:
        raise ValueError("prefix accounting exceeds bracket")
    return {
        "ticks": int(v[6]),
        "globals": {name: int(v[i]) for i, name in GLOBALS.items()},
        "residual": int(v[6]) - accounted,
        "calibration": {"sum": int(v[12]), "n": int(v[13]), "min": int(v[14]), "max": int(v[15])},
        "bands": bands,
    }
