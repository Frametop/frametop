#!/usr/bin/env python3
"""Why did a hand go missing? Reads an ft-handreplay --timeline written with the tracker's trace
(and best with --oracle N) and, per side (the tracker's left/right call), finds every
gap of 0.3 s or more between processed sets with a hand there, then says what the tracker was doing.

  tools/gap_report.py TIMELINE [--min 0.3] [--list]

Per gap:
- start: what happened to the side's last hand at the first set without it (its views lost, a
  handoff miss, a split, the misread guard, or the hand crossed to the other side);
- time held (the hand kept for 0.3 s, its crops retried where it was), time with the search blocked
  (two hand ids tracked), time searching, and time waiting between searches;
- the oracle (a full search every N sets): in how many of its samples the side's hand was findable,
  and whether any tile it was found in was searched during the gap;
- palms the search found on the way that the landmark model rejected, and how the hand came back.

Causes, one per gap, by where most of the missing time went:
  findable-not-searched  the oracle found it, but the search never tried a tile it was in
  searched-missed        a tile that holds it was searched, and the palm detector missed it then
  palm-rejected          the search found its palm, but the landmark model said no hand (< 0.5)
  search-blocked         the search didn't run: two hand ids were tracked
  not-findable           the oracle didn't find it either (out of view, or no model sees it)
  held                   back within the hold (rare at 0.3 s)
  label-flip             not lost: both hands were published, this one called the other side
"""
import argparse
import collections
import re
import sys


def parse(path):
    sets = collections.OrderedDict()   # ts -> dict(hands=[...], ev=[...])
    oracle = collections.defaultdict(list)
    breaks = []   # a pause in the recording (the hand recorder's takes stop between prompts) ends here
    for line in open(path):
        parts = line.split()
        if len(parts) < 2:
            continue
        try:
            ts = float(parts[0])
        except ValueError:
            continue
        rest = parts[1:]
        if rest[0] == 'ev':
            sets.setdefault(ts, {'hands': [], 'ev': [], 'proc': False})['ev'].append(' '.join(rest[1:]))
        elif rest[0] == 'oracle':
            # oracle CAM tile N presence P right R wrist X Y Z roi ...
            oracle[ts].append({'cam': rest[1], 'tile': int(rest[3]), 'presence': float(rest[5]),
                               'x': float(rest[9]), 'right': float(rest[7])})
        elif rest[0] == 'break':
            breaks.append(ts)
        elif rest[0] == 'oracle-done':
            oracle.setdefault(ts, [])
        elif rest[0] == '-':
            sets.setdefault(ts, {'hands': [], 'ev': [], 'proc': False})['proc'] = True
        elif re.fullmatch(r'\d+', rest[0]) and len(rest) >= 6 and rest[1] in ('L', 'R'):
            d = sets.setdefault(ts, {'hands': [], 'ev': [], 'proc': False})
            d['proc'] = True
            # side: the tracker's left/right call when the line has it (hand L|R), else by the wrist
            side = rest[7] if len(rest) >= 8 and rest[6] == 'hand' else rest[1]
            d['hands'].append({'id': int(rest[0]), 'side': side, 'views': int(rest[2]), 'x': float(rest[3])})
    procs = [(ts, d) for ts, d in sets.items() if d['proc']]
    return procs, oracle, breaks


def report(path, min_gap=0.3, listing=False):
    procs, oracle, breaks = parse(path)
    otimes = sorted(oracle)
    total_time = procs[-1][0] - procs[0][0] if procs else 0
    out = {}
    for side in 'LR':
        gaps = []
        last = None   # (index, hand id)
        for i, (ts, d) in enumerate(procs):
            here = [h for h in d['hands'] if h['side'] == side]
            if not here:
                continue
            if last is not None and any(procs[last[0]][0] < b <= ts for b in breaks):
                last = None
            if last is not None and ts - procs[last[0]][0] >= min_gap:
                gaps.append((last[0], i, last[1], here[0]['id']))
            last = (i, here[0]['id'])
        rows = []
        for a, b, hid, back_id in gaps:
            t0, t1 = procs[a][0], procs[b][0]
            nxt = procs[a + 1][1]
            ev0 = [e for e in nxt['ev'] if re.search(r'\b%d\b' % hid, e)]
            crossed = any(h['id'] == hid for h in nxt['hands'])
            start = 'crossed' if crossed else ','.join(sorted(set(e.split()[0] for e in ev0
                    if e.split()[0] in ('lost', 'miss', 'split', 'misread', 'dup', 'over-budget')))) or '?'
            # state per processed interval in the gap
            forgotten = crossed
            t = collections.Counter()
            searched_tiles = set()
            rejected = kept = 0
            blocked_ids = set()
            for k in range(a + 1, b + 1):
                ts, d = procs[k]
                dt = ts - procs[k - 1][0]
                evs = d['ev']
                if any(e.startswith('forget %d ' % hid) for e in evs):
                    forgotten = True
                srch = [e for e in evs if e.startswith('search ')]
                blk = [e for e in evs if e.startswith('search-blocked')]
                for e in srch:
                    for m in re.finditer(r'(\S+)#(\d+)', e):
                        searched_tiles.add((m.group(1), int(m.group(2))))
                for e in evs:
                    if e.startswith('palm '):
                        if e.endswith('rejected'):
                            rejected += 1
                        else:
                            kept += 1
                for e in blk:
                    blocked_ids.update(e.split()[2:])
                if len(d['hands']) >= 2 and all(h['side'] != side for h in d['hands']):
                    t['flip'] += dt   # two hands published, neither called this side: a labelling error
                elif not forgotten:
                    t['held'] += dt
                elif blk:
                    t['blocked'] += dt
                elif srch:
                    t['search'] += dt
                else:
                    t['wait'] += dt
            # oracle samples inside the gap (after the hold)
            osamp = [o for o in otimes if t0 < o < t1]
            found = [o for o in osamp if any((h['right'] > 0.5) == (side == 'R') for h in oracle[o])]
            otiles = set()
            for o in found:
                for h in oracle[o]:
                    if (h['right'] > 0.5) == (side == 'R'):
                        otiles.add((h['cam'], h['tile']))
            frac = len(found) / len(osamp) if osamp else None
            end_ev = procs[b][1]['ev']
            how = 'search' if any(e.startswith('palm ') and e.endswith('kept') for e in end_ev) else \
                  'split-off' if any('split off' in e for e in end_ev) else \
                  'crossed-back' if any(h['id'] == hid for h in procs[b][1]['hands']) else 'handover/other'
            dur = t1 - t0
            if t['flip'] >= 0.5 * dur:
                cause = 'label-flip'
            elif t['held'] >= 0.8 * dur:
                cause = 'held'
            elif t['blocked'] >= 0.5 * dur:
                cause = 'search-blocked'
            elif frac is not None and frac < 0.3:
                cause = 'not-findable'
            elif rejected >= 2:
                cause = 'palm-rejected'
            elif otiles and not (otiles & searched_tiles):
                cause = 'findable-not-searched'
            elif otiles & searched_tiles:
                cause = 'searched-missed'
            else:
                cause = 'not-findable' if frac is not None else 'no-oracle'
            rows.append(dict(t0=t0, t1=t1, dur=dur, hid=hid, back=back_id, start=start, cause=cause, t=t,
                             frac=frac, rejected=rejected, kept=kept, how=how, ntiles=len(searched_tiles),
                             otiles=len(otiles), hit=len(otiles & searched_tiles), blocked=blocked_ids))
        out[side] = rows
    print(f'{path}: {total_time:.0f} s processed')
    # label-free: per oracle sample, hands the full search found (distinct left/right calls) against
    # hands the tracker published at the last processed set
    import bisect
    pts = [p[0] for p in procs]
    oh = om = 0
    for o in otimes:
        i = bisect.bisect_right(pts, o + 1e-6) - 1
        if i < 0:
            continue
        n = len(set(h['right'] > 0.5 for h in oracle[o]))
        oh += n
        om += max(0, n - min(2, len(procs[i][1]['hands'])))
    if oh:
        print(f'  oracle: {oh} hands findable, tracker had {om} fewer ({100 * om / oh:.1f}% missed)')
    for side in 'LR':
        rows = out[side]
        tot = sum(r['dur'] for r in rows)
        longest = max([r['dur'] for r in rows], default=0)
        print(f'  {side}: {len(rows)} gaps >= {min_gap} s, {tot:.1f} s missing, longest {longest:.1f} s')
        by = collections.Counter()
        n = collections.Counter()
        for r in rows:
            by[r['cause']] += r['dur']
            n[r['cause']] += 1
        for c, s in by.most_common():
            print(f'     {c:22s} {n[c]:3d} gaps {s:6.1f} s ({100 * s / max(tot, 1e-9):.0f}%)')
        st = collections.Counter()
        for r in rows:
            st[r['start']] += 1
        tt = collections.Counter()
        for r in rows:
            tt.update(r['t'])
        print('     started by: ' + ', '.join(f'{k} {v}' for k, v in st.most_common()) +
              ';  time labelled as the other side %.1f, held %.1f, blocked %.1f, searching %.1f, waiting %.1f s' %
              (tt['flip'], tt['held'], tt['blocked'], tt['search'], tt['wait']))
        if listing:
            for r in rows:
                fr = '-' if r['frac'] is None else f"{100 * r['frac']:.0f}%"
                print(f"     {r['t0']:7.2f}-{r['t1']:7.2f} {r['dur']:4.1f}s id {r['hid']}->{r['back']} start {r['start']:12s} "
                      f"{r['cause']:22s} flip {r['t']['flip']:.1f} held {r['t']['held']:.1f} blk {r['t']['blocked']:.1f} srch {r['t']['search']:.1f} "
                      f"wait {r['t']['wait']:.1f}  oracle {fr} tiles searched {r['ntiles']} oracle-tiles {r['otiles']} hit {r['hit']} "
                      f"palms rejected {r['rejected']} kept {r['kept']} back by {r['how']}"
                      + (f" blocked by {sorted(r['blocked'])}" if r['blocked'] else ''))
    return out


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('timeline', nargs='+')
    ap.add_argument('--min', type=float, default=0.3)
    ap.add_argument('--list', action='store_true')
    a = ap.parse_args()
    for p in a.timeline:
        report(p, a.min, a.list)
