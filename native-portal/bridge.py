#!/usr/bin/env python3
"""Follow the live Frametop desk top face and portal intent for the passthrough layer."""
import argparse
import json
import math
import os
from pathlib import Path
import signal
import socket
import struct
import sys
import time

import control

STOP = False


def stopping(_signal, _frame):
    global STOP
    STOP = True


def request(command):
    with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as wire:
        wire.bind('')
        wire.settimeout(0.2)
        wire.connect('\0ft_screens')
        wire.send(command.encode())
        return wire.recv(32768).decode()


def acknowledge(ready):
    reply = request(f'passthrough publisher {int(ready)}')
    if reply != f'ok publisher {int(ready)}':
        raise RuntimeError('publisher acknowledgement failed: ' + reply[:120])


def sample():
    reply = request('passthrough mask')
    if not reply.startswith('ok mask '):
        raise RuntimeError('ft-screens needs the passthrough snapshot command: ' + reply[:120])
    state = json.loads(reply[8:])
    if any(type(state.get(key)) is not bool for key in ('wanted', 'valid', 'above', 'hands_wanted', 'hands_valid')):
        raise ValueError('invalid desktop intent/validity')
    vectors = [state[key] for key in ('center', 'u', 'v')]
    if any(not isinstance(v, list) or len(v) != 3 or
           any(type(n) not in (int, float) or not math.isfinite(n) for n in v) for v in vectors):
        raise ValueError('invalid desktop aperture')
    capsules = state.get('capsules')
    if not isinstance(capsules, list) or len(capsules) > 64 or any(
            not isinstance(c, list) or len(c) != 8 or
            any(type(n) not in (int, float) or not math.isfinite(n) for n in c) or
            not 0 < c[3] < 1 or not 0 < c[7] < 1 for c in capsules):
        raise ValueError('invalid hand capsules')
    if not (state['hands_wanted'] and state['hands_valid']):
        capsules = []
    desk = state['wanted'] and state['valid']
    return vectors, desk, state['above'], capsules, desk or state['hands_wanted']


def publish(path, vectors=None, enabled=False, above=False, capsules=()):
    values = [0.0] * (16 + 64 * 8)
    if vectors:
        for i, vector in enumerate(vectors):
            values[4*i:4*i+3] = vector
    values[12] = float(enabled)
    values[13] = float(above)
    values[14] = float(len(capsules))
    for i, capsule in enumerate(capsules):
        values[16+8*i:24+8*i] = capsule
    temporary = path.with_name(path.name + '.tmp')
    temporary.write_bytes(struct.pack('<528f', *values))
    temporary.replace(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--geometry', type=Path, required=True)
    args = parser.parse_args()
    os.umask(0o077)
    args.geometry.parent.mkdir(parents=True, exist_ok=True)
    signal.signal(signal.SIGTERM, stopping)
    signal.signal(signal.SIGINT, stopping)
    last_enabled = None
    last_error = None
    try:
        while not STOP:
            started = time.monotonic()
            try:
                vectors, desk, above, capsules, enabled = sample()
                publish(args.geometry, vectors, desk, above, capsules)
                if enabled != last_enabled:
                    control.send(enabled)
                    last_enabled = enabled
                acknowledge(True)
                last_error = None
            except (OSError, RuntimeError, ValueError, struct.error) as error:
                try:
                    acknowledge(False)
                except (OSError, RuntimeError):
                    pass
                publish(args.geometry)
                if last_enabled is not False:
                    try:
                        control.send(False)
                        last_enabled = False
                    except (OSError, RuntimeError):
                        pass
                message = str(error)
                if message != last_error:
                    print('passthrough geometry unavailable: ' + message, file=sys.stderr, flush=True)
                last_error = message
            time.sleep(max(0, 0.011 - (time.monotonic() - started)))
    finally:
        try:
            acknowledge(False)
        except (OSError, RuntimeError):
            pass
        publish(args.geometry)
        try:
            control.send(False)
        except (OSError, RuntimeError):
            pass


if __name__ == '__main__':
    main()
