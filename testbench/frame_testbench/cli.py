"""Agent-friendly CLI. Input overrides persist until explicitly released."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import uuid
from . import controller_inputs, install, pose, transport

ROOT = Path(__file__).resolve().parents[1]


class Bench:
    def __init__(self, socket_path=None):
        self.socket = socket_path or os.environ.get('FRAME_TESTBENCH_SOCKET',
            f'/run/user/{os.getuid()}/frame-testbench.sock')

    def control(self, command):
        return transport.request(self.socket, command)

    def native(self, *arguments):
        binary = os.environ.get('FRAME_TESTBENCH_OBSERVER', str(ROOT / 'build/frame-observe'))
        return transport.run_json([binary, *map(str, arguments)], timeout=125)

    def capture(self, output):
        if output:
            directory = Path(output).resolve()
            directory.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        else:
            parent = ROOT / 'artifacts'
            parent.mkdir(mode=0o700, exist_ok=True)
            directory = parent / ('capture-' + uuid.uuid4().hex)
        before = self.native('status')
        result = self.native('capture', str(directory))
        result['runtime_before'] = before
        result['runtime_after'] = self.native('status')
        result['output'] = str(directory)
        result['files'] = {name: str(directory / name) for name in ('preview.png', 'stereo.png')
                           if (directory / name).is_file()}
        (directory / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        return result


def fetch_capture(host, result, output):
    transport.remote_argv(host, '.', [])  # validate host before scp
    directory = Path(output).resolve()
    directory.mkdir(mode=0o700, parents=True, exist_ok=False)
    local = {}
    for name in ('preview.png', 'stereo.png'):
        remote = result['files'][name]
        if not remote.startswith('/') or Path(remote).name != name or any(ord(c) < 32 for c in remote):
            raise ValueError('invalid remote capture path')
        dest = directory / name
        subprocess.run(['scp', '-q', '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=yes',
                        '--', f'{host}:{remote}', str(dest)], check=True, timeout=45)
        with dest.open('rb') as stream:
            if stream.read(8) != b'\x89PNG\r\n\x1a\n':
                raise ValueError('download is not PNG')
        local[name] = str(dest)
    result = dict(result, local_files=local)
    (directory / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def parser():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--host', help='SSH host alias; commands execute on the headset')
    ap.add_argument('--remote-root', default='dev/frame-testbench', help='remote checkout, relative to SSH home or absolute')
    ap.add_argument('--socket', help='override local input control socket')
    commands = ap.add_subparsers(dest='command', required=True)
    commands.add_parser('status', help='runtime poses, frames and input override readback')
    commands.add_parser('inputs', help='independent SteamVR controller action readback')
    compositor = commands.add_parser('compositor', help='persistently control compositor standby policy')
    compositor.add_argument('mode', choices=('awake', 'auto'))
    worn = commands.add_parser('worn', help='override HMD proximity independently of pose')
    worn.add_argument('state', choices=('on', 'off', 'physical'))
    hmd = commands.add_parser('pose', help='override the actual HMD pose')
    controller = commands.add_parser('controller', help='drive synthetic controller poses and inputs')
    controller.add_argument('side', choices=('left', 'right'))
    for p, reset_help in ((hmd, 'restore physical pose while retaining worn override'),
                          (controller, 'clear inputs and disconnect the selected synthetic controller')):
        poses = p.add_subparsers(dest='pose_command', required=True)
        absolute = poses.add_parser('set')
        absolute.add_argument('--position', type=float, nargs=3, required=True, metavar=('X', 'Y', 'Z'))
        absolute.add_argument('--rotation', type=float, nargs=3, default=[0, 0, 0], metavar=('YAW', 'PITCH', 'ROLL'))
        absolute.add_argument('--space', choices=('standing', 'raw'), default='standing')
        relative = poses.add_parser('move')
        relative.add_argument('--translation', type=float, nargs=3, default=[0, 0, 0], metavar=('X', 'Y', 'Z'))
        relative.add_argument('--rotation', type=float, nargs=3, default=[0, 0, 0], metavar=('YAW', 'PITCH', 'ROLL'))
        relative.add_argument('--space', choices=('standing', 'raw'), default='standing')
        poses.add_parser('reset', help=reset_help)
    for name in ('button', 'touch', 'trigger', 'grip', 'thumbstick'):
        inputs = poses.add_parser(name, help='update inputs; requires controller set first')
        if name in ('button', 'touch'):
            inputs.add_argument('control')
            inputs.add_argument('state', choices=('on', 'off'))
        elif name == 'thumbstick':
            inputs.add_argument('x', type=float)
            inputs.add_argument('y', type=float)
        else:
            inputs.add_argument('value', type=float)
        if name in ('trigger', 'grip', 'thumbstick'):
            inputs.add_argument('--click', choices=('on', 'off'))
        if name != 'touch':
            inputs.add_argument('--touch', choices=('on', 'off'))
    poses.add_parser('inputs-reset', help='neutralize inputs, retaining the controller pose')
    commands.add_parser('release', help='restore physical HMD pose/proximity and disconnect synthetic controllers')
    capture = commands.add_parser('capture', help='fresh compositor stereo PNGs and metadata')
    capture.add_argument('--output', help='new output directory, on headset when using --host')
    capture.add_argument('--fetch', help='with --host, download PNGs into this new local directory')
    for name in ('install', 'uninstall'):
        p = commands.add_parser(name, help='change only the SteamVR user service override')
        p.add_argument('--restart', action='store_true', help='restart SteamVR now; interrupts VR')
    return ap


def dispatch(args, bench):
    input_command = controller_inputs.command(args)
    if input_command is not None:
        return bench.control(input_command)
    if args.command == 'inputs':
        return bench.native('inputs', str(ROOT / 'resources/input-actions.json'))
    if args.command == 'status':
        runtime = bench.native('status')
        try:
            state = bench.control('status')
            state['available'] = True
        except (OSError, RuntimeError) as error:
            state = {'available': False, 'error': str(error)}
        return {'ok': True, 'runtime': runtime, 'input': state}
    if args.command == 'compositor':
        return bench.native('setting', 'false' if args.mode == 'awake' else 'true')
    if args.command == 'worn':
        return bench.control({'on': 'worn 1', 'off': 'worn 0', 'physical': 'worn-release'}[args.state])
    if args.command == 'release':
        return bench.control('release')
    if args.command in ('pose', 'controller'):
        controller = args.command == 'controller'
        target = 'controller ' + args.side if controller else 'pose'
        if args.pose_command == 'reset':
            return bench.control('controller-release ' + args.side if controller else 'pose-release')
        if args.pose_command == 'set':
            desired = pose.from_euler(args.position, args.rotation)
        else:
            pose.from_euler(args.translation, args.rotation)  # validate before I/O
            state = bench.control('status')
            if controller:
                state = state['controllers'][args.side]
            if not state['pose_override']:
                raise ValueError(f'{target} move requires {target} set first')
            desired = state['pose']
        transform = None
        if args.space == 'standing':
            matrix = bench.native('status')['transforms']['raw_to_standing']
            transform = pose.from_matrix([value for row in matrix for value in row])
            if args.pose_command == 'move':
                desired = pose.compose(transform, desired)
        if args.pose_command == 'move':
            desired = pose.offset(desired, args.translation, args.rotation)
        if transform:
            desired = pose.compose(pose.inverse(transform), desired)
        values = desired['position'] + desired['quaternion']
        command = 'controller-pose ' + args.side if controller else 'pose'
        return bench.control(command + ' ' + ' '.join(format(v, '.17g') for v in values))
    if args.command == 'capture':
        return bench.capture(args.output)
    if args.command in ('install', 'uninstall'):
        result = install.stage(ROOT / 'build', Path.home()) if args.command == 'install' else install.remove(Path.home())
        subprocess.run(['systemctl', '--user', 'daemon-reload'], check=True, timeout=20)
        if args.restart:
            subprocess.run(['systemctl', '--user', 'restart', 'steamvr.service'], check=True, timeout=90)
            result['restart_requested'] = True
        return result
    raise ValueError('unsupported command')


def main(argv=None):
    arguments = list(sys.argv[1:] if argv is None else argv)
    args = parser().parse_args(arguments)
    try:
        controller_inputs.command(args)  # validate inputs before local or SSH I/O
        if args.host:
            # Remove only the transport options; preserve arguments and literal values.
            forwarded = []
            i = 0
            while i < len(arguments):
                item = arguments[i]
                if item in ('--host', '--remote-root', '--fetch'):
                    i += 2
                    continue
                if item.startswith(('--host=', '--remote-root=', '--fetch=')):
                    i += 1
                    continue
                forwarded.append(item)
                i += 1
            result = transport.run_json(transport.remote_argv(args.host, args.remote_root, forwarded), timeout=110)
            if args.command == 'capture' and args.fetch:
                result = fetch_capture(args.host, result, args.fetch)
        else:
            if args.command == 'capture' and args.fetch:
                raise ValueError('--fetch requires --host; use --output for local capture')
            result = dispatch(args, Bench(args.socket))
        print(json.dumps(result, indent=2, allow_nan=False))
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(json.dumps({'ok': False, 'error': str(error)}))
        return 1
