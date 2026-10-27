"""Bounded local control and explicit SSH transport."""
import json
import re
import shlex
import socket
import subprocess


def request(path, command, timeout=4):
    if not command or '\n' in command or '\r' in command or len(command) > 2048:
        raise ValueError('control request must be one bounded line')
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(timeout)
        client.connect(str(path))
        client.sendall(command.encode() + b'\n')
        data = bytearray()
        while b'\n' not in data:
            part = client.recv(4096)
            if not part:
                raise RuntimeError('input proxy disconnected without a complete reply')
            data.extend(part)
            if len(data) > 65536:
                raise RuntimeError('oversized proxy reply')
    value = json.loads(data.split(b'\n', 1)[0])
    if not isinstance(value, dict) or value.get('ok') is not True:
        raise RuntimeError(f'input proxy refused: {value}')
    return value


def run_json(argv, timeout=20):
    result = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
    try:
        value = json.loads(result.stdout)
    except ValueError:
        raise RuntimeError(f'command returned invalid JSON: {result.stdout[-2048:]} {result.stderr[-2048:]}') from None
    if result.returncode or not isinstance(value, dict) or value.get('ok') is False:
        raise RuntimeError(f'command failed ({result.returncode}): {value}; {result.stderr[-2048:]}')
    return value


def remote_argv(host, root, arguments):
    if not re.fullmatch(r'[A-Za-z0-9_][A-Za-z0-9_.@:-]*', host):
        raise ValueError('expected SSH hostname or configured alias, not SSH options')
    return ['ssh', '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=yes',
            '-o', 'ConnectTimeout=10', host,
            shlex.join(['python3', root.rstrip('/') + '/frame-testbench'] + list(arguments))]
