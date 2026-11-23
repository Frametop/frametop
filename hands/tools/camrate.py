#!/usr/bin/env python3
"""camrate: how fast each camera in ft-camd's ring runs, and how much of it is usable.

Polls the ring for SECONDS and, per camera, reports:
  pub fps   frames ft-camd published (bright, complete)
  sensor    the sensor's own rate, from the V4L2 sequence numbers' steps
  dark      frames ft-camd dropped as near-black (the controller-LED exposures)
  luma      median mean brightness of the published frames
  gap ms    median and largest time between published frames (capture clock)
  age ms    median capture -> published (camera clock to CLOCK_MONOTONIC_RAW is ~ +1.7 ms
            from dequeue; this is dqbuf -> publish plus capture -> dqbuf by V4L2 timestamps)

Used to compare XRService's trackingCameras modes (streamingMode, slamFrameFPSMode).
Standard library only: python3 hands/tools/camrate.py [SECONDS] [--json]
"""
import json
import os
import statistics
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from recorder import fhformat  # noqa: E402

import mmap  # noqa: E402


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    seconds = float(args[0]) if args else 5.0
    fd = os.open(fhformat.ring_path(), os.O_RDONLY)
    m = mmap.mmap(fd, 0, mmap.MAP_SHARED, mmap.PROT_READ)
    os.close(fd)
    hdr = fhformat.RING_HDR.unpack_from(m, 0)
    ncams = hdr[3]
    cams = []
    for i in range(ncams):
        f = fhformat.RING_CAM.unpack_from(m, fhformat.ring_cam_offset(i))
        cams.append(dict(i=i, name=f[1].split(b'\0', 1)[0].decode(), node=f[2], nslots=f[7],
                         slot_offset=f[8], slot_bytes=f[9], flags=f[13], seen={}))

    def counts(c):
        f = fhformat.RING_CAM.unpack_from(m, fhformat.ring_cam_offset(c['i']))
        return f[11], f[12]          # published, dropped

    for c in cams:
        c['pub0'], c['drop0'] = counts(c)
    t0 = time.monotonic()
    while time.monotonic() - t0 < seconds:
        for c in cams:
            n = struct.unpack_from('<Q', m, fhformat.ring_cam_offset(c['i']) + fhformat.RING_CAM_LATEST_OFF)[0]
            for k in range(max(1, n - c['nslots'] + 1), n + 1):
                if k in c['seen']:
                    continue
                off = c['slot_offset'] + (k % c['nslots']) * c['slot_bytes']
                s = fhformat.RING_SLOT.unpack_from(m, off)
                if s[0] == 2 * k + 2:
                    c['seen'][k] = s
        time.sleep(0.001)
    dt = time.monotonic() - t0
    now_mono = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
    raw_off = now_mono - time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)
    out = []
    for c in cams:
        pub, drop = counts(c)
        frames = [c['seen'][k] for k in sorted(c['seen'])]
        caps = [f[2] for f in frames]
        seqs = [f[5] for f in frames]
        gaps = [(b - a) / 1e6 for a, b in zip(caps, caps[1:])]
        steps = [b - a for a, b in zip(seqs, seqs[1:]) if b > a]
        span = (caps[-1] - caps[0]) / 1e9 if len(caps) > 1 else 0
        sensor = (seqs[-1] - seqs[0]) / span if span > 0 else 0
        ages = [(f[4] - (f[2] + raw_off)) / 1e6 for f in frames]
        out.append(dict(
            cam=c['name'], node=c['node'], dark_twin=bool(c['flags'] & 1), color=bool(c['flags'] & 2),
            pub_fps=round((pub - c['pub0']) / dt, 1), dark_fps=round((drop - c['drop0']) / dt, 1),
            sensor_fps=round(sensor, 1), seq_step=statistics.median(steps) if steps else None,
            luma=round(statistics.median([f[6] for f in frames]), 1) if frames else None,
            gap_ms=(round(statistics.median(gaps), 2), round(max(gaps), 2)) if gaps else None,
            age_ms=round(statistics.median(ages), 2) if ages else None))
    if '--json' in sys.argv:
        print(json.dumps(out))
        return
    print('%-26s %7s %7s %7s %5s %6s %15s %7s' % ('camera', 'pub/s', 'dark/s', 'sensor', 'step', 'luma', 'gap ms med/max', 'age ms'))
    for o in out:
        print('%-26s %7.1f %7.1f %7.1f %5s %6s %15s %7s' % (
            o['cam'][:26], o['pub_fps'], o['dark_fps'], o['sensor_fps'], o['seq_step'], o['luma'],
            '%s/%s' % o['gap_ms'] if o['gap_ms'] else '-', o['age_ms']))


if __name__ == '__main__':
    main()
