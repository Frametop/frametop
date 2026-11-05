"""Draw frame sets from a recording (ft-hands --record) with what the tracker saw.

usage: python tools/show_set.py REC_DIR SET [SET...] [--timeline TL] [--out DIR]

SET is a set index (ft-handreplay's timeline gives them). With --timeline (ft-handreplay
--timeline), each camera shows the tracker's views at that set: the crop for the next
frame, labelled with the hand and presence. Recordings made with ft-camd --with-dark get
a second row: each camera's latest dark frame (<name>_dk), stretched to be visible and
labelled with its mean brightness. Recordings made with ft-camd --with-color get a row of
the color cameras (color_video<N>). Writes OUT/set_<n>.jpg (default /tmp).
"""
import argparse
import os
import sys

import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from recorder import fhformat  # noqa: E402  (FHSET01, hands/track/record.h)

HDR, CAM = fhformat.SET_HDR, fhformat.SET_CAM
ORDER = ['slam_left', 'slam_right', 'upper_left', 'upper_right']


def index(path):
    """Byte offset of every set in sets.bin."""
    offs, size = [], os.path.getsize(path)
    with open(path, 'rb') as f:
        off = 0
        while off + HDR.size <= size:
            f.seek(off)
            magic, n, nbytes = HDR.unpack(f.read(HDR.size))
            if magic[:7] != fhformat.SET_MAGIC or off + nbytes > size:
                break
            offs.append(off)
            off += nbytes
    return offs


def read_set(path, off):
    with open(path, 'rb') as f:
        f.seek(off)
        _, n, _ = HDR.unpack(f.read(HDR.size))
        cams = [CAM.unpack(f.read(CAM.size)) for _ in range(n)]
        out = {}
        for name, w, h, cap, dq in cams:
            px = np.frombuffer(f.read(w * h), np.uint8).reshape(h, w)
            out[name.rstrip(b'\0').decode()] = (px, cap)
    return out


def views_at(timeline, n):
    out = []
    for line in open(timeline):
        f = line.split()
        if len(f) > 1 and f[1] == 'view' and int(f[-1]) == n:
            out.append({'hand': int(f[2]), 'cam': f[3], 'presence': float(f[5]),
                        'c': (float(f[7]), float(f[8])), 'size': float(f[9]), 'rot': float(f[10])})
    return out


def dark_tile(frame, shape, name):
    """A dark frame, stretched from its 1st to 99.5th percentile; black if there's none."""
    h, w = shape
    if frame is None:
        return np.zeros((h, w, 3), np.uint8)
    px = frame[0]
    lo, hi = np.percentile(px, (1, 99.5))
    gain = 255 / max(hi - lo, 1)
    img = np.clip((px.astype(np.float32) - lo) * gain, 0, 255).astype(np.uint8)
    img = cv2.cvtColor(cv2.resize(img, (w, h)), cv2.COLOR_GRAY2BGR)
    cv2.putText(img, '%s_dk mean %.1f, x%.0f' % (name, px.mean(), gain), (10, 30), cv2.FONT_HERSHEY_SIMPLEX, 1.0,
                (255, 255, 0), 2)
    return img


def view_tile(px, name, views):
    """A frame, CLAHE'd, with the tracker's views on it, 512 px high."""
    img = cv2.cvtColor(cv2.createCLAHE(2.0, (8, 8)).apply(px), cv2.COLOR_GRAY2BGR)
    for v in views:
        if v['cam'] != name:
            continue
        c, s, r = v['c'], v['size'], v['rot']
        box = cv2.boxPoints(((c[0], c[1]), (s, s), np.degrees(r)))
        col = (0, 255, 0) if v['presence'] >= 0.5 else (0, 0, 255)
        cv2.polylines(img, [box.astype(np.int32)], True, col, 2)
        cv2.putText(img, 'h%d %.2f' % (v['hand'], v['presence']), (int(c[0] - s / 2), int(c[1] - s / 2) - 6),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.8, col, 2)
    cv2.putText(img, name, (10, 30), cv2.FONT_HERSHEY_SIMPLEX, 1.0, (255, 255, 0), 2)
    scale = 512 / img.shape[0]
    return cv2.resize(img, (int(img.shape[1] * scale), 512))


def draw(images, views):
    """Rows: the mono cameras; their dark frames, if recorded; the color cameras, if recorded."""
    tiles, dark = [], []
    for name in ORDER:
        if name not in images:
            continue
        tiles.append(view_tile(images[name][0], name, views))
        dark.append(dark_tile(images.get(name + '_dk'), tiles[-1].shape[:2], name))
    rows = [np.hstack(tiles)]
    if any(k.endswith('_dk') for k in images):
        rows.append(np.hstack(dark))
    color = sorted(k for k in images if k.startswith('color_'))
    if color:
        rows.append(np.hstack([view_tile(images[k][0], k, views) for k in color]))
    width = max(r.shape[1] for r in rows)
    return np.vstack([np.pad(r, ((0, 0), (0, width - r.shape[1]), (0, 0))) for r in rows])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('rec')
    ap.add_argument('sets', type=int, nargs='+')
    ap.add_argument('--timeline')
    ap.add_argument('--out', default='/tmp')
    a = ap.parse_args()
    path = os.path.join(a.rec, 'sets.bin')
    offs = index(path)
    for n in a.sets:
        images = read_set(path, offs[n])
        views = views_at(a.timeline, n) if a.timeline else []
        out = os.path.join(a.out, 'set_%05d.jpg' % n)
        cv2.imwrite(out, draw(images, views), [cv2.IMWRITE_JPEG_QUALITY, 85])
        print(out)


if __name__ == '__main__':
    main()
