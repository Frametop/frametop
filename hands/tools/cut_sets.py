"""Copy a few frame sets out of a recording (ft-hands --record) into a small one, to look at
or check elsewhere without moving gigabytes. Plain Python, so it runs on the Frame's host.

usage: python3 tools/cut_sets.py REC_DIR OUT_DIR [--sets N] (8, spread evenly) | [--at I,J,...]
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from recorder import fhformat  # noqa: E402  (FHSET01, hands/track/record.h)

HDR = fhformat.SET_HDR


def offsets(path):
    offs, size = [], os.path.getsize(path)
    with open(path, 'rb') as f:
        off = 0
        while off + HDR.size <= size:
            f.seek(off)
            magic, _, nbytes = HDR.unpack(f.read(HDR.size))
            if magic[:7] != fhformat.SET_MAGIC or off + nbytes > size:
                break
            offs.append((off, nbytes))
            off += nbytes
    return offs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('rec')
    ap.add_argument('out')
    ap.add_argument('--sets', type=int, default=8)
    ap.add_argument('--at', default='')
    a = ap.parse_args()
    src = os.path.join(a.rec, 'sets.bin')
    offs = offsets(src)
    if a.at:
        pick = [int(i) for i in a.at.split(',')]
    else:
        n = max(1, min(a.sets, len(offs)))
        pick = [round(i * (len(offs) - 1) / max(n - 1, 1)) for i in range(n)]
    os.makedirs(a.out, exist_ok=True)
    with open(src, 'rb') as f, open(os.path.join(a.out, 'sets.bin'), 'wb') as out:
        for i in pick:
            off, nbytes = offs[i]
            f.seek(off)
            out.write(f.read(nbytes))
    print('%d of %d sets (%s) -> %s' % (len(pick), len(offs), ','.join(map(str, pick)), a.out))


if __name__ == '__main__':
    main()
