#!/usr/bin/env python3
"""Install or remove persistent native passthrough. Restarts SteamVR."""
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


def launch_dropin(output, previous_outputs=()):
    existing = dict(item.split('=', 1) for item in shlex.split(property_value(STEAMVR, 'Environment')) if '=' in item)
    additions = {'LD_PRELOAD': str(output / 'libft_passthrough_gate.so'),
                 'VK_ADD_LAYER_PATH': str(output), 'VK_INSTANCE_LAYERS': 'VK_LAYER_FT_passthrough'}
    owned = {'LD_PRELOAD': {str(p / 'libft_passthrough_gate.so') for p in previous_outputs},
             'VK_ADD_LAYER_PATH': {str(p) for p in previous_outputs},
             'VK_INSTANCE_LAYERS': {'VK_LAYER_FT_passthrough'} if previous_outputs else set()}
    values = {}
    for key, value in additions.items():
        # LD_PRELOAD accepts spaces or colons; Vulkan layer lists use colons.
        old = existing.get(key, '')
        if key == 'LD_PRELOAD':
            old = old.replace(' ', ':')
        rest = [item for item in old.split(':') if item and item not in owned[key]]
        values[key] = ':'.join([value] + rest)
    values['FT_PASSTHROUGH_ENABLE'] = '1'
    values['FT_PASSTHROUGH_MASK'] = str(output / 'mask.bin')
    return MARKER + '[Service]\n' + ''.join('Environment=' + quote_environment(key + '=' + value) + '\n' for key, value in values.items())


def bridge_unit(output):
    # ExecStart also expands dollar signs; keep paths literal.
    argv = ' '.join(quote_environment(str(v)).replace('$', '$$') for v in
                    (sys.executable, HERE / 'bridge.py', '--geometry', output / 'mask.bin'))
    return (MARKER + '[Unit]\nDescription=Frametop native passthrough bridge\n'
            'After=steamvr.service\nPartOf=steamvr.service\nRequisite=steamvr.service\n'
            '[Service]\nExecStart=' + argv + '\nRestart=on-failure\nRestartSec=2\n'
            '[Install]\nWantedBy=steamvr.service\n')


def save_state(path, state):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(state, indent=2) + '\n')
    temporary.chmod(0o600)
    temporary.replace(path)


def owned_state(path, dropin, bridge=None):
    if not path.exists():
        if dropin.exists() or (bridge and bridge.exists()):
            raise RuntimeError('refusing existing native passthrough files without ownership state')
        return None
    state = json.loads(path.read_text())
    if state.get('dropin_path') != str(dropin) or not state.get('dropin', '').startswith(MARKER):
        raise RuntimeError('invalid passthrough ownership state')
    if dropin.exists() and dropin.read_text() != state['dropin']:
        raise RuntimeError('refusing changed or unknown passthrough drop-in')
    if bridge:
        if state.get('bridge_path') != str(bridge) or not state.get('bridge_content', '').startswith(MARKER):
            raise RuntimeError('invalid passthrough bridge ownership state')
        if bridge.exists() and bridge.read_text() != state['bridge_content']:
            raise RuntimeError('refusing changed or unknown passthrough bridge')
    return state


def cancel_rollback():
    command('systemctl', '--user', 'stop', TIMER + '.timer', check=False)


def legacy_state_file(dropin, fallback):
    if not dropin.exists():
        return fallback
    content = dropin.read_text()
    if not content.startswith(MARKER):
        raise RuntimeError('refusing unknown runtime passthrough drop-in')
    for line in content.splitlines():
        if not line.startswith('Environment='):
            continue
        for entry in shlex.split(line.split('=', 1)[1]):
            key, separator, value = entry.partition('=')
            if separator and key == 'FT_PASSTHROUGH_MASK':
                mask = Path(value.replace('%%', '%'))
                if not mask.is_absolute() or mask.name != 'mask.bin':
                    raise RuntimeError('invalid legacy passthrough mask path')
                state = mask.parent / 'launch-state.private.json'
                # The marker alone is insufficient: require the exact previously
                # saved drop-in before using its path for migration/removal.
                owned_state(state, dropin)
                return state
    raise RuntimeError('runtime passthrough drop-in has no mask path')


def disable(dropin, bridge, state_path, legacy_dropin, legacy_state_path):
    saved = owned_state(state_path, dropin, bridge)
    legacy = owned_state(legacy_state_path, legacy_dropin)
    if not saved and not legacy:
        print('Passthrough is already disabled.')
        return
    # Capture this session only. Never resurrect environment saved before a reboot.
    session = {'desktop': snapshot_desktop(), 'companions':
               [u for u in COMPANIONS if property_value(u, 'ActiveState') == 'active']}
    command('systemctl', '--user', 'stop', BRIDGE, check=False)
    command('systemctl', '--user', 'disable', BRIDGE, check=False)
    cancel_rollback()
    for path in (dropin, bridge) if saved else ():
        path.unlink(missing_ok=True)
    if legacy:
        legacy_dropin.unlink(missing_ok=True)
    command('systemctl', '--user', 'daemon-reload')
    command('systemctl', '--user', 'restart', STEAMVR)
    restore_session(session)
    if saved:
        state_path.unlink()
    if legacy:
        legacy_state_path.unlink()
    print('Passthrough uninstalled; SteamVR restarted with its prior launch configuration.')


def install(output, dropin, bridge, state_path, legacy_dropin, legacy_state_path, rollback_minutes):
    saved = owned_state(state_path, dropin, bridge)
    legacy = owned_state(legacy_state_path, legacy_dropin)
    previous = []
    if saved:
        previous.append(Path(saved['output']))
    if legacy:
        previous.append(legacy_state_path.parent)
    if not saved and not legacy and property_value(BRIDGE, 'LoadState') != 'not-found':
        raise RuntimeError('passthrough bridge unit already exists without ownership state')
    for name in ('libft_passthrough.so', 'libft_passthrough_gate.so', 'VkLayer_FT_passthrough.json'):
        if not (output / name).is_file():
            raise RuntimeError('build passthrough first: missing ' + name)
    session = {'desktop': snapshot_desktop(), 'companions':
               [u for u in COMPANIONS if property_value(u, 'ActiveState') == 'active']}
    content = launch_dropin(output, previous)
    unit = bridge_unit(output)
    state = {'dropin_path': str(dropin), 'dropin': content, 'bridge_path': str(bridge),
             'bridge_content': unit, 'output': str(output)}
    # Stop the old transient bridge before replacing its definition. Disabling
    # never removes unknown files; ownership is checked before the first change.
    cancel_rollback()
    if property_value(TIMER + '.service', 'ActiveState') not in ('inactive', 'failed'):
        raise RuntimeError('rollback has already started; let it finish before enabling')
    if saved or legacy:
        command('systemctl', '--user', 'stop', BRIDGE, check=False)
    save_state(state_path, state)
    try:
        dropin.parent.mkdir(parents=True, exist_ok=True)
        bridge.parent.mkdir(parents=True, exist_ok=True)
        dropin.write_text(content)
        bridge.write_text(unit)
        if legacy:
            legacy_dropin.unlink(missing_ok=True)
            legacy_state_path.unlink()
        command('systemctl', '--user', 'daemon-reload')
        command('systemctl', '--user', 'enable', BRIDGE)
        if rollback_minutes:
            command('systemd-run', '--user', '--collect', '--unit=' + TIMER,
                    '--on-active=' + str(rollback_minutes) + 'm',
                    sys.executable, str(HERE / 'run.py'), 'disable')
        command('systemctl', '--user', 'restart', STEAMVR)
        restore_session(session)
        command('systemctl', '--user', 'start', BRIDGE)
    except Exception:
        # Preserve ownership state if cleanup fails, so disable can be retried.
        try:
            disable(dropin, bridge, state_path, legacy_dropin, legacy_state_path)
        finally:
            restore_session(session)
        raise
    print('Passthrough installed for future SteamVR starts, including after reboot. Unknown native binaries remain blocked.')
    if rollback_minutes:
        print(f'Automatic uninstall in {rollback_minutes} minutes; retain cancels the timer.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['enable', 'disable', 'retain'])
    parser.add_argument('--output', type=Path, default=HERE / 'build/current')
    parser.add_argument('--rollback-minutes', type=int, default=0,
                        help='optional automatic uninstall for experimental builds')
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
    config = Path.home() / '.config'
    dropin = config / 'systemd/user/steamvr.service.d/99-ft-passthrough.conf'
    bridge = config / 'systemd/user' / BRIDGE
    state_path = config / 'frametop/native-passthrough.json'
    legacy_dropin = runtime / 'systemd/user/steamvr.service.d/99-ft-passthrough.conf'
    legacy_state_path = legacy_state_file(legacy_dropin, output / 'launch-state.private.json')
    if args.action == 'disable':
        disable(dropin, bridge, state_path, legacy_dropin, legacy_state_path)
    elif args.action == 'retain':
        if not owned_state(state_path, dropin, bridge):
            raise RuntimeError('enable the persistent installation before retaining it')
        if property_value(TIMER + '.service', 'ActiveState') not in ('inactive', 'failed'):
            raise RuntimeError('rollback has already started; cannot retain this installation')
        cancel_rollback()
        if property_value(TIMER + '.service', 'ActiveState') not in ('inactive', 'failed'):
            raise RuntimeError('rollback started while stopping its timer; let it finish')
        print('Passthrough retained across reboots; rollback cancelled. Use run.py disable to uninstall.')
    else:
        install(output, dropin, bridge, state_path, legacy_dropin, legacy_state_path, args.rollback_minutes)


if __name__ == '__main__':
    main()
