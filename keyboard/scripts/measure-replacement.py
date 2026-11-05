#!/usr/bin/env python3
"""Read-only Linux process counters for replacement polling comparisons. Never reads field text."""
import argparse
import json
import os
from pathlib import Path
import time


def counters(pid, proc=Path('/proc')):
    path = proc / str(pid)
    fields = (path / 'stat').read_text().rsplit(') ', 1)[1].split()
    result = {'start': int(fields[19]), 'ticks': int(fields[11]) + int(fields[12]),
              'rss_bytes': int(fields[21]) * os.sysconf('SC_PAGE_SIZE')}
    for filename, wanted in [('status', {'voluntary_ctxt_switches', 'nonvoluntary_ctxt_switches'}),
                             ('io', {'syscr', 'syscw'})]:
        try:
            for line in (path / filename).read_text().splitlines():
                name, _, value = line.partition(':')
                if name in wanted:
                    result[name] = int(value.strip())
        except (OSError, ValueError):
            pass
    return result


def discover(installation, include_steam=False, proc=Path('/proc')):
    """Identify only this user's installed app, its helper/guard and optional Steam totals."""
    release = (installation / 'current').resolve(strict=True)
    rows, groups = {}, {'app': [], 'helper': [], 'guard': []}
    if include_steam:
        groups['steam_webhelpers'] = []
    for path in proc.iterdir():
        if not path.name.isdecimal():
            continue
        try:
            if path.stat().st_uid != os.getuid():
                continue
            fields = (path / 'stat').read_text().rsplit(') ', 1)[1].split()
            rows[int(path.name)] = {'parent': int(fields[1]), 'exe': (path / 'exe').resolve(strict=True),
                                   'comm': (path / 'comm').read_text().strip()}
        except (OSError, ValueError, IndexError):
            continue
    for pid, row in rows.items():
        if row['exe'] == release / 'bin/framekeyboard':
            groups['app'].append(pid)
        if include_steam and row['comm'] == 'steamwebhelper':
            groups['steam_webhelpers'].append(pid)
    for pid, row in rows.items():
        if row['parent'] not in groups['app']:
            continue
        try:
            # Read only children of the verified keyboard, and never include argv in output.
            arguments = (proc / str(pid) / 'cmdline').read_bytes().split(b'\0')
            expected = os.fsencode(release / 'share/framekeyboard/bridge/native_bridge.py')
            if expected in arguments:
                groups['helper'].append(pid)
        except OSError:
            continue
    groups['guard'] = [pid for pid, row in rows.items() if row['parent'] in groups['helper'] and
                       row['exe'] == release / 'bin/framekeyboard-native-guard']
    return {name: sorted(pids) for name, pids in groups.items()}


def snapshot(groups, proc=Path('/proc')):
    result = {}
    for pids in groups.values():
        for pid in pids:
            try:
                result[pid] = counters(pid, proc)
            except (OSError, ValueError, IndexError):
                pass
    return result


def summarize(groups, before, after, seconds, ticks_per_second):
    result = {}
    for name, pids in groups.items():
        row = {'pids': pids, 'valid': bool(pids)}
        stable = all(pid in before and pid in after and before[pid]['start'] == after[pid]['start']
                     for pid in pids)
        names = ('ticks', 'voluntary_ctxt_switches', 'nonvoluntary_ctxt_switches', 'syscr', 'syscw')
        deltas = {}
        if stable:
            for counter in names:
                if all(counter in before[pid] and counter in after[pid] for pid in pids):
                    values = [after[pid][counter] - before[pid][counter] for pid in pids]
                    if any(value < 0 for value in values):
                        stable = False
                        break
                    deltas[counter] = sum(values)
        row['valid'] = bool(pids) and stable
        if row['valid']:
            row['cpu_percent_one_core'] = deltas['ticks'] / ticks_per_second / seconds * 100
            row['rss_bytes_end'] = sum(after[pid]['rss_bytes'] for pid in pids)
            row['per_second'] = {name: value / seconds for name, value in deltas.items() if name != 'ticks'}
        result[name] = row
    return result


def battery(supplies=Path('/sys/class/power_supply')):
    result = {}
    for path in supplies.glob('*'):
        try:
            if (path / 'type').read_text().strip() != 'Battery':
                continue
            fields = {}
            for name in ('status', 'capacity', 'energy_now', 'charge_now', 'power_now', 'current_now', 'voltage_now'):
                try:
                    value = (path / name).read_text().strip()
                    fields[name] = value if name == 'status' else int(value)
                except (OSError, ValueError):
                    pass
            result[path.name] = fields
        except OSError:
            pass
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--installation', type=Path, default=Path.home() / '.local/share/framekeyboard')
    parser.add_argument('--seconds', type=float, default=20)
    parser.add_argument('--label', default='baseline')
    parser.add_argument('--include-steam', action='store_true', help='Include aggregate Steam webhelper CPU, not inspect() attribution')
    options = parser.parse_args()
    if not 1 <= options.seconds <= 60:
        parser.error('--seconds must be between 1 and 60')
    try:
        groups = discover(options.installation, options.include_steam)
    except OSError:
        parser.exit(1, 'Installed keyboard could not be identified.\n')
    if not groups['helper']:
        parser.exit(1, 'No helper belonging to the current installed keyboard was found.\n')
    before, power_before = snapshot(groups), battery()
    started = time.monotonic()
    time.sleep(options.seconds)
    seconds = time.monotonic() - started
    after, power_after = snapshot(groups), battery()
    try:
        same_groups = groups == discover(options.installation, options.include_steam)
    except OSError:
        same_groups = False
    report = {'label': options.label, 'seconds': seconds, 'same_process_groups': same_groups,
              'processes': summarize(groups, before, after, seconds, os.sysconf('SC_CLK_TCK')),
              'battery_before': power_before, 'battery_after': power_after,
              'limits': 'CPU uses one core as 100%. Read/write syscall and context-switch counts are not CDP poll counts. Steam totals do not isolate inspect(). Charging and short samples cannot establish battery savings. Changed process groups invalidate a comparison.'}
    print(json.dumps(report, indent=2))
    return 0 if same_groups and report['processes']['helper']['valid'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
