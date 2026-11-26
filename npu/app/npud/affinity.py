"""ft-npud's cores: --cpus -> the set for sched_setaffinity.

On a little core (the Frame's cpu 0-1, capacity 216 of 1024) the crop copy-in, the claim and the gather each run
~4x slower: 7% of requests took +1.5-2 ms each side (60 Hz x 4 crops, 2026-11-22); on cores 2-4 p95 fell from 7.9
to 4.9 ms. ft-hands' tracker pool runs on 5-7 by default, so a production unit passes --cpus 2-4: auto picks every
big core it may use, which under full affinity overlaps that pool."""
import os
from pathlib import Path


def parse_list(spec, allowed):
    """'2-4', '2,4', '2-3,5' -> a set inside allowed; ValueError for anything else (empty parts, a range backwards,
    a core this process may not use)"""
    out = set()
    for part in spec.split(','):
        lo, dash, hi = part.partition('-')
        if not lo.strip().isdigit() or (dash and not hi.strip().isdigit()):
            raise ValueError('--cpus %r: expected a list such as 2-4 or 2,3' % spec)
        lo, hi = int(lo), int(hi) if dash else int(lo)
        if hi < lo:
            raise ValueError('--cpus %r: range %d-%d runs backwards' % (spec, lo, hi))
        out.update(range(lo, hi + 1))
    if not out <= allowed:
        raise ValueError('--cpus %r: cores %s are outside this process\'s affinity %s' % (
            spec, sorted(out - allowed), sorted(allowed)))
    return out


def big_cores(allowed, sysfs=Path('/sys/devices/system/cpu')):
    """every core in allowed with at least half the biggest core's capacity, or None when sysfs doesn't say"""
    cap = {}
    for d in sysfs.glob('cpu[0-9]*'):
        try:
            cap[int(d.name[3:])] = int((d / 'cpu_capacity').read_text())
        except (OSError, ValueError):
            return None
    if not cap:
        return None
    big = {c for c, v in cap.items() if v * 2 >= max(cap.values())} & allowed
    return big or None


def pick_cpus(spec, allowed=None):
    """auto (big_cores), all (None: leave the affinity), or a list (parse_list)"""
    allowed = os.sched_getaffinity(0) if allowed is None else allowed
    if spec == 'all':
        return None
    if spec == 'auto':
        return big_cores(allowed)
    return parse_list(spec, allowed)
