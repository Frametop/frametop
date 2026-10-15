#!/usr/bin/env python3
"""A testing session take's live.bin (hands/rec/livelog.py) as a publish dump (FHDUMP02) for
screens/tests/handinput_test --replay and --take, with the take's poses.jsonl beside it, so what
the panels did with the hands can be played again.

  live_dump.py TAKE_DIR OUT.dump      (writes OUT.dump and OUT.poses.jsonl)

Each hands publish goes out with the gestures file as it stood then; dq is when the recorder
copied the hands file. Standard library only.
"""
import shutil
import struct
import sys

FILE_HDR = struct.Struct("<8sII")
REC_HDR = struct.Struct("<B3xIQQ")


def main(take, out):
    with open(take + "/live.bin", "rb") as f:
        data = f.read()
    if data[:8] != b"FHLIVE01":
        sys.exit("%s/live.bin: not a live log" % take)
    off, gest, recs = FILE_HDR.size, None, []
    while off + REC_HDR.size <= len(data):
        src, n, read_ns, _ = REC_HDR.unpack_from(data, off)
        body = data[off + REC_HDR.size:off + REC_HDR.size + n]
        off += REC_HDR.size + n
        if len(body) < n:
            break
        if src == 1:
            gest = body
        elif src == 0 and gest is not None:
            recs.append((body, gest, read_ns))
    if not recs:
        sys.exit("%s/live.bin: no hands with gestures" % take)
    hs, gs = len(recs[0][0]), len(recs[0][1])
    with open(out, "wb") as f:
        f.write(b"FHDUMP02" + struct.pack("<II", hs, gs))
        for h, g, dq in recs:
            if len(h) == hs and len(g) == gs:
                f.write(h + g + struct.pack("<q", dq))
    base = out[:-5] if out.endswith(".dump") else out
    shutil.copyfile(take + "/poses.jsonl", base + ".poses.jsonl")
    print("%s: %d publishes" % (out, len(recs)))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("live_dump.py TAKE_DIR OUT.dump")
    main(sys.argv[1], sys.argv[2])
