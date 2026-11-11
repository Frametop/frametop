"""A PNG as the tests read one: no library, 8 bit RGB or RGBA only."""
import struct
import zlib
from pathlib import Path


def read_png(path):
    """(width, height, channels, rows of bytes) of an 8 bit RGB or RGBA PNG."""
    data = Path(path).read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    at, packed, width, height, channels = 8, b'', 0, 0, 0
    while at < len(data):
        size, tag = struct.unpack_from('>I4s', data, at)
        body = data[at + 8:at + 8 + size]
        if tag == b'IHDR':
            width, height, depth, colour = struct.unpack_from('>IIBB', body)
            assert depth == 8 and colour in (2, 6)
            channels = 3 if colour == 2 else 4
        elif tag == b'IDAT':
            packed += body
        at += 12 + size
    raw, stride, rows, last = zlib.decompress(packed), width * channels, [], bytes(width * channels)
    for y in range(height):
        kind, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            left, up, corner = (line[i - channels] if i >= channels else 0), last[i], (last[i - channels] if i >= channels else 0)
            if kind == 1:
                line[i] = (line[i] + left) & 255
            elif kind == 2:
                line[i] = (line[i] + up) & 255
            elif kind == 3:
                line[i] = (line[i] + (left + up) // 2) & 255
            elif kind == 4:
                p = left + up - corner
                near = min((abs(p - left), 0, left), (abs(p - up), 1, up), (abs(p - corner), 2, corner))[2]
                line[i] = (line[i] + near) & 255
        rows.append(bytes(line))
        last = rows[-1]
    return width, height, channels, rows
