"""Shared deterministic canary pattern (matches canary_probe.c pattern())."""

import struct


def pattern_word(seed, off):
    v = (seed * 0x01000193) & 0xFFFFFFFF
    v ^= (off * 0x9E3779B1) & 0xFFFFFFFF
    v ^= v >> 15
    v = (v * 0x85EBCA77) & 0xFFFFFFFF
    v ^= v >> 13
    return v & 0xFFFFFFFF


def pattern_bytes(seed, off, length):
    words = (pattern_word(seed, off + 4 * i) for i in range(length // 4))
    return struct.pack("<%dI" % (length // 4), *words)
