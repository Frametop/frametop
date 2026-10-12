#!/usr/bin/env python3
"""Follow the live Frametop desk top face and portal intent for the passthrough layer."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import math
import os
import re
from pathlib import Path
import signal
import stat
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


def acknowledge(ready, reason=''):
    reason = re.sub('[^A-Za-z0-9_-]', '-', reason)[:96]
    reply = request(f'passthrough publisher {int(ready)}' + (f' {reason}' if reason else ''))
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
    return vectors, desk, state['above'], capsules, desk or (state['hands_wanted'] and not state.get('paused', False) and state.get('tracking', True))


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


def native_status(path, proc=Path('/proc')):
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid():
        raise RuntimeError('invalid native status file')
    state = json.loads(path.read_text())
    if state.get('version') != 1 or type(state.get('pid')) is not int or state['pid'] <= 0:
        raise RuntimeError('invalid native status')
    process = proc / str(state['pid'])
    fields = (process / 'stat').read_text().rsplit(')', 1)[1].split()
    executable = (process / 'exe').stat()
    if (int(fields[19]) != state.get('start_ticks') or executable.st_dev != state.get('exe_dev')
            or executable.st_ino != state.get('exe_ino')):
        raise RuntimeError('stale native status')
    if state.get('ready') is not True:
        raise RuntimeError(state.get('reason') or 'native backend not ready')
    return state['pid'], state['start_ticks'], state['exe_dev'], state['exe_ino']


class RoomView:
    """One bounded control request at a time; network waits never stop mask updates."""
    def __init__(self, send=control.send, identity_check=None):
        self.send = send
        self.identity_check = identity_check
        self.worker = ThreadPoolExecutor(max_workers=1, thread_name_prefix='roomview')
        self.future = None
        self.submitted = None
        self.confirmed = None
        self.completed = 0.0
        self.next_attempt = 0.0
        self.desired = None
        self.error = None

    def deliver(self, enabled, identity):
        if self.identity_check is not None and self.identity_check() != identity:
            raise RuntimeError('native compositor changed before control request')
        self.send(enabled)

    def update(self, enabled, identity, now):
        desired = (identity, enabled) if identity is not None else None
        if desired != self.desired:
            self.desired = desired
            self.confirmed = None
            self.next_attempt = 0.0
        if self.future is not None and self.future.done():
            try:
                self.future.result()
                self.confirmed = self.submitted
                self.completed = now
                self.error = None
            except (OSError, RuntimeError, ValueError, KeyError, IndexError) as error:
                self.confirmed = None
                self.error = str(error)
            self.future = None
        # Reassert On in a worker after native environment/game transitions.
        # Off is sent only on transitions or retry, so stock user toggles are not
        # repeatedly suppressed while Frametop passthrough is disabled.
        refresh = enabled and now - self.completed >= 1.0
        if desired is not None and self.future is None and now >= self.next_attempt and (self.confirmed != desired or refresh):
            self.submitted = desired
            self.future = self.worker.submit(self.deliver, enabled, identity)
            self.next_attempt = now + 1.0
        return desired is not None and self.confirmed == desired and (not enabled or now-self.completed < 2.0)

    def close(self):
        self.worker.shutdown(wait=True, cancel_futures=True)
        if self.desired is not None:
            try:
                self.deliver(False, self.desired[0])
            except (OSError, RuntimeError, ValueError, KeyError, IndexError):
                pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--geometry', type=Path, required=True)
    args = parser.parse_args()
    os.umask(0o077)
    args.geometry.parent.mkdir(parents=True, exist_ok=True)
    signal.signal(signal.SIGTERM, stopping)
    signal.signal(signal.SIGINT, stopping)
    status_path = Path(os.environ.get('FT_PASSTHROUGH_STATUS', str(args.geometry)+'.status'))
    camera = RoomView(identity_check=lambda: native_status(status_path))
    last_error = None
    identity = None
    status_error = None
    next_status = 0.0
    try:
        while not STOP:
            started = time.monotonic()
            try:
                if started >= next_status:
                    try:
                        identity = native_status(status_path)
                        status_error = None
                    except (OSError, ValueError, RuntimeError, KeyError, IndexError) as error:
                        identity = None
                        status_error = str(error)
                    next_status = started + 0.25
                if identity is None:
                    camera.update(False, None, started)
                    raise RuntimeError(status_error or 'native backend unavailable')
                vectors, desk, above, capsules, enabled = sample()
                publish(args.geometry, vectors, desk, above, capsules)
                ready = camera.update(enabled, identity, started)
                acknowledge(ready)
                if camera.error != last_error and camera.error:
                    print('passthrough control unavailable: ' + camera.error, file=sys.stderr, flush=True)
                last_error = camera.error
            except (OSError, RuntimeError, ValueError, struct.error) as error:
                try:
                    acknowledge(False, str(error))
                except (OSError, RuntimeError):
                    pass
                publish(args.geometry)
                camera.update(False, identity, started)
                message = str(error)
                if message != last_error:
                    print('passthrough unavailable: ' + message, file=sys.stderr, flush=True)
                last_error = message
            time.sleep(max(0, 0.011 - (time.monotonic() - started)))
    finally:
        try:
            acknowledge(False)
        except (OSError, RuntimeError):
            pass
        publish(args.geometry)
        camera.close()


if __name__ == '__main__':
    main()
