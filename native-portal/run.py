#!/usr/bin/env python3
"""Enable or disable native passthrough for this boot. Restarts SteamVR."""
import argparse
import json
import os
from pathlib import Path
import platform
import shlex
import subprocess
import sys

HERE = Path(__file__).resolve().parent
STEAMVR = 'steamvr.service'
DESKTOP = 'frametop-desktop.service'
TIMER = 'ft-passthrough-rollback'
BRIDGE = 'frametop-passthrough.service'
COMPANIONS = ('frametop-pointer.service', 'frametop-gaze.service', 'frametop-power.service')
MARKER = '# Managed by frametop native-portal/run.py\n'


def command(*args, check=True):
    return subprocess.run(args, text=True, capture_output=True, check=check)


def property_value(unit, name):
    return command('systemctl', '--user', 'show', unit, '-p', name, '--value').stdout.strip()


def snapshot_desktop():
    if property_value(DESKTOP, 'ActiveState') != 'active':
        return None
    pid = int(property_value(DESKTOP, 'MainPID'))
    process = Path('/proc') / str(pid)
    argv = [os.fsdecode(item) for item in (process / 'cmdline').read_bytes().split(b'\0') if item]
    keep = {'PATH', 'HOME', 'USER', 'LOGNAME', 'SHELL', 'LANG', 'DISPLAY',
            'WAYLAND_DISPLAY', 'DBUS_SESSION_BUS_ADDRESS'}
    environment = {}
    for entry in (process / 'environ').read_bytes().split(b'\0'):
        if b'=' not in entry:
            continue
        key, value = map(os.fsdecode, entry.split(b'=', 1))
        if key in keep or key.startswith(('XDG_', 'FT_')):
            environment[key] = value
    if not argv or not pid:
        raise RuntimeError('active desktop has no restorable command')
    return {'argv': argv, 'environment': environment, 'cwd': os.readlink(process / 'cwd')}


def restore_desktop(saved):
    if not saved:
        return
    # Restart can unload a transient unit. First use its existing definition.
    started = command('systemctl', '--user', 'start', DESKTOP, check=False)
    if started.returncode == 0:
        return
    if property_value(DESKTOP, 'LoadState') != 'not-found':
        raise RuntimeError('desktop restart failed: ' + started.stderr.strip())
    args = ['systemd-run', '--user', '--collect', '--unit=' + DESKTOP,
            '--working-directory=' + saved['cwd']]
    args += ['--setenv=' + key + '=' + value for key, value in saved['environment'].items()]
    command(*args, '--', *saved['argv'])


def restore_session(saved):
    restore_desktop(saved.get('desktop'))
    for unit in saved.get('companions', []):
        if unit not in COMPANIONS:
            raise RuntimeError('unknown companion in saved state: ' + unit)
        command('systemctl', '--user', 'start', unit)


def quote_environment(value):
    # systemd unit quoting, not shell quoting. Percent specifiers must be literal.
    return '"' + value.replace('\\', '\\\\').replace('"', '\\"').replace('%', '%%').replace('\n', '\\n') + '"'


def launch_dropin(output):
    existing = dict(item.split('=', 1) for item in shlex.split(property_value(STEAMVR, 'Environment')) if '=' in item)
    additions = {'LD_PRELOAD': str(output / 'libft_passthrough_gate.so'),
                 'VK_ADD_LAYER_PATH': str(output), 'VK_INSTANCE_LAYERS': 'VK_LAYER_FT_passthrough'}
    values = {key: value + (':' + existing[key] if existing.get(key) else '') for key, value in additions.items()}
    values['FT_PASSTHROUGH_ENABLE'] = '1'
    values['FT_PASSTHROUGH_MASK'] = str(output / 'mask.bin')
    return MARKER + '[Service]\n' + ''.join('Environment=' + quote_environment(key + '=' + value) + '\n' for key, value in values.items())


def save_state(path, state):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(state, indent=2) + '\n')
    temporary.chmod(0o600)
    temporary.replace(path)


def disable(dropin, state_path):
    if not state_path.exists():
        if dropin.exists():
            raise RuntimeError('refusing existing drop-in without its saved ownership state')
        print('passthrough launch settings are already disabled.')
        return
    saved = json.loads(state_path.read_text())
    if saved.get('dropin_path') != str(dropin) or not saved.get('dropin', '').startswith(MARKER):
        raise RuntimeError('invalid passthrough ownership state')
    if dropin.exists() and dropin.read_text() != saved['dropin']:
        raise RuntimeError('refusing to remove a changed or unknown drop-in')
    if saved.get('bridge'):
        command('systemctl', '--user', 'stop', BRIDGE, check=False)
    try:
        import control
        control.send(False)
    except (OSError, RuntimeError) as error:
        print('RoomView Off transport unavailable; restarting stock SteamVR: ' + str(error), file=sys.stderr)
    if dropin.exists():
        dropin.unlink()
    command('systemctl', '--user', 'daemon-reload')
    command('systemctl', '--user', 'restart', STEAMVR)
    restore_session(saved)
    command('systemctl', '--user', 'stop', TIMER + '.timer', check=False)
    state_path.unlink()
    print('passthrough removed; SteamVR restarted with its prior launch configuration.')


def retain(dropin, state_path):
    saved = json.loads(state_path.read_text())
    if (saved.get('dropin_path') != str(dropin) or not saved.get('dropin', '').startswith(MARKER)
            or not dropin.is_file() or dropin.read_text() != saved['dropin']):
        raise RuntimeError('refusing to retain an unknown or changed passthrough launch configuration')
    if property_value(TIMER + '.service', 'ActiveState') not in ('inactive', 'failed'):
        raise RuntimeError('rollback has already started; cannot retain this session')
    command('systemctl', '--user', 'stop', TIMER + '.timer')
    if property_value(TIMER + '.service', 'ActiveState') not in ('inactive', 'failed'):
        raise RuntimeError('rollback started while stopping its timer; let it finish')
    saved['retained_current_boot'] = True
    save_state(state_path, saved)
    print('passthrough retained for this boot; rollback timer cancelled. Use run.py disable with the same --output to restore stock SteamVR.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['enable', 'disable', 'retain'])
    parser.add_argument('--output', type=Path, default=HERE / 'build/current')
    parser.add_argument('--rollback-minutes', type=int, default=0,
                        help='optional automatic disable for experimental builds')
    args = parser.parse_args()
    if args.rollback_minutes < 0:
        parser.error('rollback minutes must be nonnegative')
    if platform.system() != 'Linux' or platform.machine() != 'aarch64' or not Path('/opt/steamvr/bin/linuxarm64/vrcompositor').is_file():
        parser.error('run this helper on the Steam Frame host, outside distrobox')
    if Path('/run/.containerenv').exists() or Path('/.dockerenv').exists():
        parser.error('leave the dev container and run this helper on the host')
    os.umask(0o077)
    runtime = Path('/run/user') / str(os.getuid())
    os.environ['XDG_RUNTIME_DIR'] = str(runtime)
    os.environ['DBUS_SESSION_BUS_ADDRESS'] = 'unix:path=' + str(runtime / 'bus')
    output = args.output.resolve()
    dropin = runtime / 'systemd/user/steamvr.service.d/99-ft-passthrough.conf'
    state_path = output / 'launch-state.private.json'
    if args.action == 'disable':
        disable(dropin, state_path)
        return
    if args.action == 'retain':
        retain(dropin, state_path)
        return
    if dropin.exists() or state_path.exists():
        raise RuntimeError('passthrough drop-in or state already exists; disable the owned trial first. Unknown drop-ins are never overwritten.')
    for name in ('libft_passthrough.so', 'libft_passthrough_gate.so', 'VkLayer_FT_passthrough.json'):
        if not (output / name).is_file():
            raise RuntimeError('build the passthrough first: missing ' + name)
    if property_value(BRIDGE, 'LoadState') != 'not-found':
        raise RuntimeError('passthrough bridge unit already exists; stop the previous trial first')
    desktop = snapshot_desktop()
    companions = [unit for unit in COMPANIONS if property_value(unit, 'ActiveState') == 'active']
    content = launch_dropin(output)
    saved = {'dropin_path': str(dropin), 'dropin': content, 'desktop': desktop, 'companions': companions}
    save_state(state_path, saved)
    try:
        if args.rollback_minutes:
            command('systemd-run', '--user', '--collect', '--unit=' + TIMER,
                    '--on-active=' + str(args.rollback_minutes) + 'm',
                    sys.executable, str(HERE / 'run.py'), 'disable', '--output', str(output))
        dropin.parent.mkdir(parents=True, exist_ok=True)
        with dropin.open('x') as stream:
            stream.write(content)
        command('systemctl', '--user', 'daemon-reload')
        command('systemctl', '--user', 'restart', STEAMVR)
        restore_session(saved)
        saved['bridge'] = True
        save_state(state_path, saved)
        command('systemd-run', '--user', '--collect', '--unit=' + BRIDGE,
                sys.executable, str(HERE / 'bridge.py'), '--geometry', str(output / 'mask.bin'))
    except Exception:
        # Keep state if rollback itself fails, so disable can be retried.
        disable(dropin, state_path)
        raise
    print('Passthrough enabled for this boot. Use Display Settings to toggle and place the mask.')
    if args.rollback_minutes:
        print(f'Automatic disable in {args.rollback_minutes} minutes; retain cancels the timer.')


if __name__ == '__main__':
    main()
