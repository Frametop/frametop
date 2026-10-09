"""Watch the gestures ft-hands publishes, live: pinches, grips and middle pinches, begins, ends, and drags.

usage: python3 tools/watch_gestures.py [--every S] [--distance]

Prints a line when a pinch, a grip (a closed hand) or a middle pinch (thumb to middle finger)
begins or ends on either hand. It goes
by the counters, so a quick tap between two reads still shows. While one is held, every
--every seconds (default 0.1) it prints how far its point has moved since it began, in the
head frame (turning your head moves it too; a real consumer turns both points into the room
first, see include/fh_gestures.h). --distance also prints each hand's thumb-to-index
distance and finger curl, to see how close a gesture comes to the thresholds.
Version 1 files (pinches only) and version 2 files (no middle pinches) still work.
"""
import argparse
import mmap
import os
import struct
import time

HDR = struct.Struct('<8sIIQQQffff8x')        # 64 bytes
SLOT = struct.Struct('<IIIIQQff3f3f')        # 64 bytes
TRACKED, DOWN, LOST = 1, 2, 4
SIDES = ('left ', 'right')
KINDS = ('pinch', 'grip ', 'middle pinch')


def path():
    return '/run/user/%d/frametop-hands/gestures' % os.getuid()


def read(m):
    """(header, [[pinch left, right], [grip left, right], [middle pinch left, right]]) under the
    sequence lock, or None if it's being written. Version 1 has no grips and version 2 no middle
    pinches: they read as all zero."""
    for _ in range(10):
        s1 = struct.unpack_from('<Q', m, 16)[0]
        if s1 % 2 == 0:
            h = HDR.unpack_from(m, 0)
            kinds = 1
            if h[1] >= 2 and len(m) >= HDR.size + 4 * SLOT.size:
                kinds = 2
            if h[1] >= 3 and len(m) >= HDR.size + 6 * SLOT.size:
                kinds = 3
            g = [[SLOT.unpack_from(m, HDR.size + (k * 2 + s) * SLOT.size) for s in range(2)] for k in range(kinds)]
            while len(g) < 3:
                g.append([(0,) * 14, (0,) * 14])
            if struct.unpack_from('<Q', m, 16)[0] == s1:
                return h, g
        time.sleep(0.0005)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--every', type=float, default=0.1, help='seconds between drag lines while held')
    ap.add_argument('--distance', action='store_true', help="print each hand's pinch distance and finger curl")
    a = ap.parse_args()
    with open(path(), 'rb') as f:
        m = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
    first = read(m)
    if first is None or first[0][0] != b'FHGEST01':
        raise SystemExit('%s is not an ft-hands gestures file' % path())
    h, g = first
    print('version %d; thresholds: pinch begins under %.3f m, ends over %.3f m; grip begins with every finger '
          'curled under %.2f, ends over %.2f' % (h[1], h[6], h[7], h[8], h[9]))
    seen = [[(q[2], q[3]) for q in kind] for kind in g]   # begins, ends
    last_drag = last_dist = 0.0
    while True:
        got = read(m)
        if got:
            h, g = got
            now = time.monotonic()
            for k, kind in enumerate(g):
                for s, q in enumerate(kind):
                    flags, hand, begins, ends, begin_ns, end_ns, dist, strength = q[:8]
                    point, begin_point = q[8:11], q[11:14]
                    if begins != seen[k][s][0]:
                        print('%s %s BEGIN (#%d, hand %d) at %+.3f %+.3f %+.3f  %s %.3f' %
                              (SIDES[s], KINDS[k], begins, hand, *begin_point, 'curl' if k else 'd', dist), flush=True)
                    if ends != seen[k][s][1]:
                        held = (end_ns - begin_ns) / 1e9 if end_ns >= begin_ns else 0
                        print('%s %s %s after %.2f s' % (SIDES[s], KINDS[k], 'LOST' if flags & LOST else 'END', held),
                              flush=True)
                    seen[k][s] = (begins, ends)
                    if flags & DOWN and now - last_drag >= a.every:
                        d = [point[i] - begin_point[i] for i in range(3)]
                        print('%s %s   drag %+6.1f %+6.1f %+6.1f mm  (%.0f mm)' %
                              (SIDES[s], KINDS[k], *(1000 * x for x in d), 1000 * sum(x * x for x in d) ** 0.5),
                              flush=True)
            if any(q[0] & DOWN for kind in g for q in kind) and now - last_drag >= a.every:
                last_drag = now
            if a.distance and now - last_dist >= 0.2:
                last_dist = now
                print('    ' + '   '.join(
                    '%s %s' % (SIDES[s].strip(), 'd %.3f curl %.2f' % (g[0][s][6], g[1][s][6]) if g[0][s][0] & TRACKED
                               else '-') for s in range(2)), flush=True)
        time.sleep(0.005)


if __name__ == '__main__':
    main()
