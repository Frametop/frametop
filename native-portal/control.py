#!/usr/bin/env python3
"""Send the native RoomView toggle over SteamVR's local mailbox WebSocket.

This only reports transport delivery. Camera pixels are verified separately.
No third-party Python packages or private helper executables are required.
"""
import argparse
import base64
import hashlib
import json
import os
import socket
import struct
import time


def send(enabled):
    key = base64.b64encode(os.urandom(16)).decode()
    with socket.create_connection(('127.0.0.1', 27062), timeout=3) as wire:
        wire.sendall((
            'GET / HTTP/1.1\r\nHost: 127.0.0.1:27062\r\n'
            'Upgrade: websocket\r\nConnection: Upgrade\r\n'
            'Origin: http://127.0.0.1:27062\r\n'
            f'Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n'
        ).encode())
        response = b''
        while b'\r\n\r\n' not in response:
            part = wire.recv(4096)
            if not part or len(response) > 16384:
                raise RuntimeError('invalid WebSocket handshake')
            response += part
        header = response.split(b'\r\n\r\n', 1)[0].decode('latin1')
        lines = header.split('\r\n')
        fields = dict(line.split(':', 1) for line in lines[1:] if ':' in line)
        fields = {k.lower(): v.strip() for k, v in fields.items()}
        expected = base64.b64encode(hashlib.sha1(
            (key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()
        ).digest()).decode()
        if lines[0].split()[1] != '101' or fields.get('sec-websocket-accept') != expected:
            raise RuntimeError('SteamVR refused WebSocket handshake')
        message = ('mailbox_send vrcompositor_systemlayer ' + json.dumps({
            'type': 'set_camera_roomview_enabled', 'enabled': enabled,
        }, separators=(',', ':'))).encode()
        mask = os.urandom(4)
        size = len(message)
        prefix = bytes([0x81, 0x80 | size]) if size < 126 else bytes([0x81, 0xfe]) + struct.pack('!H', size)
        wire.sendall(prefix + mask + bytes(value ^ mask[i % 4] for i, value in enumerate(message)))
        # The setter has no response message. Give the local server a chance to
        # process the command before the connection closes.
        time.sleep(0.15)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('state', choices=['on', 'off'])
    args = parser.parse_args()
    send(args.state == 'on')
    print(json.dumps({'command_sent': True, 'enabled': args.state == 'on', 'rendering_verified': False}))


if __name__ == '__main__':
    main()
