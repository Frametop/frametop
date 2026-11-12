#!/usr/bin/env python3
"""Records the loop of pictures ft-pcsend plays (together/assets/pcloop-640x360.bin): one second
of the test picture that counts, out of the Frame's hardware encoder, as ft-sharepanel hands it
to ft-together. By hand, on the Frame: it takes an encoder for a few seconds.

    together/tests/record-loop.py PATH-TO-ft-sharepanel OUT [--bits N] [--pictures N]

It stands where ft-together's media socket would, says the panel is watched, lets the encoder
settle for a second, asks for a keyframe and keeps that picture and the ones after it. The size
is 640x360: the encoder is only ever given a width that is a multiple of 128 (media/irisenc.h).
"""
import argparse
import os
import socket
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "conf"))
import ftipc  # noqa: E402

SIZE, FPS = "640x360", 30


def message(kind, body=b""):
    return struct.pack("<I", 1 + len(body)) + kind + body


def messages(conn, buf):
    """The next whole messages from the program, as (kind, body)."""
    data = conn.recv(1 << 16)
    if not data:
        raise ConnectionError("ft-sharepanel went")
    buf += data
    while len(buf) >= 4 and len(buf) - 4 >= struct.unpack_from("<I", buf)[0]:
        length = struct.unpack_from("<I", buf)[0]
        yield bytes(buf[4:5]), bytes(buf[5:4 + length])
        del buf[:4 + length]


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("sharepanel")
    parser.add_argument("out")
    parser.add_argument("--bits", type=int, default=400000, help="a second, that the encoder is set for")
    parser.add_argument("--pictures", type=int, default=FPS, help="in the loop")
    args = parser.parse_args()
    name = "ft_record_loop_%d" % os.getpid()
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM | socket.SOCK_CLOEXEC)
    server.bind(ftipc.address(name + "_media"))
    server.listen(1)
    server.settimeout(10)
    child = subprocess.Popen([args.sharepanel, "--panel", "0", "--size", SIZE, "--fps", str(FPS), "--socket", name, "--pattern"])
    try:
        conn, _ = server.accept()
        conn.settimeout(10)
        conn.sendall(message(b"R", struct.pack("<I", args.bits)) + message(b"F", bytes([FPS])) + message(b"P", bytes([FPS])) + message(b"W", b"\1"))
        buf, loop, settled, asked = bytearray(), [], None, False
        while len(loop) < args.pictures:
            for kind, body in messages(conn, buf):
                if kind != b"U" or len(body) < 2:
                    continue
                settled = settled or time.monotonic() + 1.0
                if not asked and time.monotonic() >= settled:
                    conn.sendall(message(b"K"))
                    asked = True
                elif asked and body[0] & 1:
                    loop = [body]              # the loop begins with a keyframe, and has no other
                elif loop and len(loop) < args.pictures:
                    loop.append(body)
        with open(args.out, "wb") as out:
            for body in loop:
                out.write(message(b"U", body))
        print("%d pictures, %d bytes: the keyframe %d, the others %d to %d" % (len(loop), sum(5 + len(b) for b in loop), len(loop[0]) - 1,
                                                                             min(len(b) for b in loop[1:]) - 1, max(len(b) for b in loop[1:]) - 1))
    finally:
        child.terminate()
        try:
            child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            child.kill()


if __name__ == "__main__":
    main()
