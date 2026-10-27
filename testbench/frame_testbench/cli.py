"""Agent-friendly CLI. Input overrides persist until explicitly released."""
import argparse
import json
import os
import re
from pathlib import Path
import subprocess
import sys
import time
import uuid
from . import controller_inputs, desktop, fakes, handshape, install, pose, record, transport

ROOT = Path(__file__).resolve().parents[1]
FAKES_UNIT = 'frame-testbench-fakes'
RECORDINGS = ROOT / 'artifacts' / 'recordings'
RECORDING_NAME = r'[A-Za-z0-9][A-Za-z0-9._-]{0,63}'
RECORDING_FILES = {'gaze': 'gaze.jsonl', 'hands': 'hands.fhdump'}


class Bench:
    def __init__(self, socket_path=None):
        self.socket = socket_path or os.environ.get('FRAME_TESTBENCH_SOCKET',
            f'/run/user/{os.getuid()}/frame-testbench.sock')

    def control(self, command):
        return transport.request(self.socket, command)

    def fakes(self, command, start=False):
        """A command to the fake trackers (frame_testbench.fakes), or None when they aren't
        running. start: run them first, as a transient user unit, if they aren't."""
        try:
            return transport.request(fakes.SOCKET, command)
        except (FileNotFoundError, ConnectionRefusedError):
            if not start:
                return None
        subprocess.run(['systemd-run', '--user', '--quiet', '--collect', '--unit=' + FAKES_UNIT,
                        '--working-directory=' + str(ROOT), '--setenv=PYTHONPATH=' + str(ROOT),
                        sys.executable, '-m', 'frame_testbench.fakes'], check=True, timeout=20)
        deadline = time.monotonic() + 5
        while True:
            try:
                return transport.request(fakes.SOCKET, command)
            except (FileNotFoundError, ConnectionRefusedError):
                if time.monotonic() > deadline:
                    raise RuntimeError(f'the fake trackers did not start: journalctl --user -u {FAKES_UNIT}') from None
                time.sleep(0.05)

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


def recording_name(text):
    if not re.fullmatch(RECORDING_NAME, text):
        raise argparse.ArgumentTypeError(f'{text!r}: a recording name is letters, digits, ".", "_" and "-"')
    return text


def key_combo(text):
    try:
        desktop.combo(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError(str(error)) from None
    return text


def hmd_button_path(text):
    if not re.fullmatch(controller_inputs.HMD_BUTTON_PATH, text):
        raise argparse.ArgumentTypeError(f'{text!r} is not an /input/... component path')
    return text


def parser():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--host', help='SSH host alias; commands execute on the headset')
    ap.add_argument('--remote-root', default='dev/frametop/testbench', help='remote checkout, relative to SSH home or absolute')
    ap.add_argument('--socket', help='override local input control socket')
    commands = ap.add_subparsers(dest='command', required=True)
    commands.add_parser('status', help='runtime poses, frames and input override readback')
    commands.add_parser('inputs', help='independent SteamVR controller action readback')
    compositor = commands.add_parser('compositor', help='persistently control compositor standby policy')
    compositor.add_argument('mode', choices=('awake', 'auto'))
    worn = commands.add_parser('worn', help='override HMD proximity independently of pose')
    worn.add_argument('state', choices=('on', 'off', 'physical'))
    button = commands.add_parser('hmd-button', help="override one of the HMD's own buttons; status lists them")
    button.add_argument('path', type=hmd_button_path, help='component path, such as /input/system/click')
    button.add_argument('state', choices=('on', 'off', 'physical'))
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
    gaze = commands.add_parser('gaze', help='fake eye tracking: ft-gaze reads it instead of both trackers')
    gazes = gaze.add_subparsers(dest='gaze_command', required=True)
    look = gazes.add_parser('look', help='both eyes look this way: head-relative degrees, yaw +left, pitch +up')
    look.add_argument('yaw', type=float)
    look.add_argument('pitch', type=float)
    look.add_argument('--right', type=float, nargs=2, metavar=('YAW', 'PITCH'),
                      help='the right eye apart; YAW PITCH are then the left eye')
    eyes = gazes.add_parser('eyes', help='each eye open, closed or lost (not seen); needs gaze look first')
    eyes.add_argument('left', choices=fakes.EYE_STATES)
    eyes.add_argument('right', choices=fakes.EYE_STATES)
    gazes.add_parser('release', help='the real eye trackers again, within 2 s')
    hand = commands.add_parser('hand', help='fake hands: ft-hands parks and these are published instead')
    hand.add_argument('side', choices=('left', 'right', 'all'), help='all: release only')
    hands = hand.add_subparsers(dest='hand_command', required=True)
    put = hands.add_parser('set', help='the wrist in the head frame; no rotation = fingers forward, palm down')
    put.add_argument('--position', type=float, nargs=3, required=True, metavar=('X', 'Y', 'Z'))
    put.add_argument('--rotation', type=float, nargs=3, default=[0, 0, 0], metavar=('YAW', 'PITCH', 'ROLL'))
    put.add_argument('--shape', choices=handshape.SHAPES, default='open',
                     help='pinch holds a pinch down, fist a grip')
    hands.add_parser('release', help='ft-hands tracks again once no hand is faked')
    rec = commands.add_parser('record', help="record the real trackers' output (never images) for replay")
    rec.add_argument('name', type=recording_name, help='a new recording, kept in artifacts/recordings/NAME')
    rec.add_argument('--seconds', type=float, required=True, help='how long, up to 300')
    rec.add_argument('--only', choices=tuple(RECORDING_FILES), help='just the gaze or just the hands')
    play = commands.add_parser('replay', help='play a recording through the fake trackers')
    play.add_argument('name', type=recording_name)
    play.add_argument('--loop', action='store_true', help='over and over until released (else released at its end)')
    play.add_argument('--only', choices=tuple(RECORDING_FILES), help='just its gaze or just its hands')
    keyboard = commands.add_parser('keyboard', help="a keyboard plugged into Frametop's input relay: keys go where typing goes")
    keyboards = keyboard.add_subparsers(dest='keyboard_command', required=True)
    keys = keyboards.add_parser('keys', help='press and let go of each combination in turn, e.g. ctrl+l enter')
    keys.add_argument('combos', nargs='+', type=key_combo, metavar='COMBO')
    typed = keyboards.add_parser('type', help='text, typed as Frametop Voice types it (no newlines: keys enter)')
    typed.add_argument('text')
    mouse = commands.add_parser('mouse', help="a mouse plugged into Frametop's input relay (in pointer mode, the 3D mouse)")
    mice = mouse.add_subparsers(dest='mouse_command', required=True)
    move = mice.add_parser('move', help='mouse counts, +x right, +y down')
    move.add_argument('dx', type=int)
    move.add_argument('dy', type=int)
    move.add_argument('--drag', choices=tuple(desktop.BUTTONS), help='hold this button through the move')
    click = mice.add_parser('click')
    click.add_argument('button', nargs='?', default='left', choices=tuple(desktop.BUTTONS))
    click.add_argument('--count', type=int, default=1, choices=(1, 2, 3))
    scroll = mice.add_parser('scroll', help='wheel notches, + up')
    scroll.add_argument('notches', type=int)
    scroll.add_argument('--horizontal', action='store_true', help='+ right')
    pointer = commands.add_parser('pointer', help="Frametop's pointer helper")
    pointers = pointer.add_subparsers(dest='pointer_command', required=True)
    pointers.add_parser('state', help='the head, gaze mode, and whether the headset is worn')
    pointers.add_parser('overlays', help="SteamVR's overlays, as the pointer helper lists them")
    measure = pointers.add_parser('measure', help="an overlay's centre, size and axes in the standing universe")
    measure.add_argument('overlay')
    for name in desktop.POINTER_COMMANDS:
        pointers.add_parser(name, help={'recenter': 'the cursor back in front'}.get(name))
    power = commands.add_parser('power', help="ft-powerd: the headset displays' state, or turn them on or off")
    power.add_argument('state', choices=desktop.POWER_COMMANDS)
    commands.add_parser('release', help='restore physical HMD pose/proximity, disconnect synthetic controllers, stop fake trackers')
    capture = commands.add_parser('capture', help='fresh compositor stereo PNGs and metadata')
    capture.add_argument('--output', help='new output directory, on headset when using --host')
    capture.add_argument('--fetch', help='with --host, download PNGs into this new local directory')
    # No --restart: install.sh restarts SteamVR with the checks SteamOS's health check needs.
    for name in ('install', 'uninstall'):
        commands.add_parser(name, help='change only the SteamVR user service override; applies at its next start')
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
        try:
            faked = bench.fakes('status') or {'running': False}
            faked.setdefault('running', True)
        except (OSError, RuntimeError) as error:
            faked = {'running': False, 'error': str(error)}
        return {'ok': True, 'runtime': runtime, 'input': state, 'fakes': faked}
    if args.command == 'gaze':
        if args.gaze_command == 'look':
            look = [args.yaw, args.pitch] + (args.right or [args.yaw, args.pitch])
            return bench.fakes('gaze ' + ' '.join(format(v, '.17g') for v in look), start=True)
        if args.gaze_command == 'eyes':
            reply = bench.fakes(f'eyes {args.left} {args.right}')
            if reply is None:
                raise ValueError('gaze eyes requires gaze look first')
            return reply
        return bench.fakes('gaze-release') or {'ok': True, 'gaze': None}
    if args.command == 'hand':
        if args.hand_command == 'set':
            if args.side == 'all':
                raise ValueError('hand set takes left or right')
            quaternion = pose.from_euler(args.position, args.rotation)['quaternion']
            values = list(args.position) + quaternion
            return bench.fakes(f'hand {args.side} ' + ' '.join(format(v, '.17g') for v in values) + ' ' + args.shape,
                               start=True)
        return bench.fakes('hand-release ' + args.side) or {'ok': True, 'hands': None}
    if args.command == 'keyboard':
        return desktop.press_keys(args.combos) if args.keyboard_command == 'keys' else desktop.type_text(args.text)
    if args.command == 'mouse':
        if args.mouse_command == 'move':
            return desktop.mouse_move(args.dx, args.dy, args.drag)
        if args.mouse_command == 'click':
            return desktop.mouse_click(args.button, args.count)
        return desktop.mouse_scroll(args.notches, args.horizontal)
    if args.command == 'pointer':
        if args.pointer_command == 'state':
            return desktop.pointer_state()
        if args.pointer_command == 'overlays':
            return desktop.overlays()
        if args.pointer_command == 'measure':
            return desktop.measure(args.overlay)
        return desktop.pointer(args.pointer_command)
    if args.command == 'power':
        return desktop.power(args.state)
    if args.command == 'compositor':
        return bench.native('setting', 'false' if args.mode == 'awake' else 'true')
    if args.command == 'worn':
        return bench.control({'on': 'worn 1', 'off': 'worn 0', 'physical': 'worn-release'}[args.state])
    if args.command == 'hmd-button':
        if args.state == 'physical':
            return bench.control('hmd-button-release ' + args.path)
        return bench.control(f"hmd-button {args.path} {'1' if args.state == 'on' else '0'}")
    if args.command == 'record':
        directory = RECORDINGS / args.name
        if directory.exists():
            raise ValueError(f'recording {args.name} already exists')
        RECORDINGS.mkdir(mode=0o700, parents=True, exist_ok=True)
        return record.record(str(directory), args.seconds, gaze=args.only != 'hands', hands=args.only != 'gaze')
    if args.command == 'replay':
        directory = RECORDINGS / args.name
        names = sorted(p.name for p in RECORDINGS.iterdir()) if RECORDINGS.is_dir() else []
        if not directory.is_dir():
            raise ValueError(f'no recording {args.name}; recordings: {", ".join(names) or "none"}')
        files = {kind: directory / name for kind, name in RECORDING_FILES.items()
                 if (directory / name).is_file() and args.only in (None, kind)}
        if not files:
            raise ValueError(f'recording {args.name} has no {args.only or "gaze or hands"}')
        if any(c.isspace() for c in str(directory)):
            raise ValueError(f'{directory}: the fake trackers take paths without spaces')
        reply = None
        for kind, path in files.items():
            reply = bench.fakes(f"replay-{kind} {path} {'loop' if args.loop else 'once'}", start=True)
        return reply
    if args.command == 'release':
        # The fake trackers first: they don't need SteamVR, so they go even when it's down. A
        # daemon that doesn't answer is stopped (it cleans up on SIGTERM), and the input proxy
        # is released either way.
        try:
            faked = bench.fakes('release') or {'running': False}
        except (OSError, RuntimeError) as error:
            subprocess.run(['systemctl', '--user', 'stop', FAKES_UNIT], timeout=20)
            faked = {'running': False, 'stopped': FAKES_UNIT, 'error': str(error)}
        result = bench.control('release')
        result['fakes'] = faked
        return result
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
            timeout = 110 + (args.seconds if args.command == 'record' else 0)
            result = transport.run_json(transport.remote_argv(args.host, args.remote_root, forwarded), timeout=timeout)
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
