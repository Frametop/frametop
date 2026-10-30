"""Remote displays whose host lends Frametop its keyboard and mouse (docs/host-input.md), and the
flag that says the testbench is faking the wearer.

ft-stream leases a host's keyboard and mouse while the pointer helper says the headset is worn,
and the helper can't tell the testbench's `worn on` from a real head. So a faked wearer with a
live stream to a host that has "input": true on its layout entry takes that computer's keyboard
and mouse away from whoever sits at it (2026-10-30: oma7, mid-sentence). The testbench refuses to
fake the wearer then, unless asked with --allow-host-input, and while it fakes the wearer it
leaves FLAG for ft-stream, which leases nothing while FLAG says it isn't allowed to.

FLAG ($XDG_RUNTIME_DIR/frametop-fake-worn.json) is JSON: {"since": UNIX TIME, "pid": PID,
"allow_host_input": BOOL}. It exists exactly while the testbench holds `worn on`.
"""
import json
import os
from pathlib import Path

LAYOUT = Path.home() / '.config/frametop-layout.json'


def runtime_dir():
    return Path(os.environ.get('XDG_RUNTIME_DIR') or f'/run/user/{os.getuid()}')


def flag_path():
    return runtime_dir() / 'frametop-fake-worn.json'


def read_flag():
    """The fake-worn flag, or None. A garbled one counts as not allowed."""
    try:
        flag = json.loads(flag_path().read_text())
    except FileNotFoundError:
        return None
    except (OSError, ValueError):
        return {'allow_host_input': False}
    return flag if isinstance(flag, dict) else {'allow_host_input': False}


def set_flag(allow, now):
    path = flag_path()
    tmp = path.with_name(path.name + f'.{os.getpid()}')
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_CLOEXEC, 0o600)
    with os.fdopen(fd, 'w') as f:
        json.dump({'since': now, 'pid': os.getpid(), 'allow_host_input': bool(allow)}, f)
    os.replace(tmp, path)


def clear_flag():
    flag_path().unlink(missing_ok=True)


def input_hosts(layout=LAYOUT):
    """Layout host address -> name, for the hosts with "input": true."""
    try:
        hosts = json.loads(Path(layout).read_text()).get('hosts') or []
    except (OSError, ValueError, AttributeError):
        return {}
    return {h['address']: h.get('name') or h['address'] for h in hosts
            if isinstance(h, dict) and h.get('input') is True and isinstance(h.get('address'), str)}


def streams(proc='/proc'):
    """Every running `ft-stream stream HOST[:PORT] --id NAME ...`: [{pid, host, id}]."""
    found = []
    for entry in os.scandir(proc):
        if not entry.name.isdigit():
            continue
        try:
            argv = Path(entry.path, 'cmdline').read_bytes().split(b'\0')
        except OSError:
            continue   # gone, or not ours to read
        argv = [a.decode(errors='replace') for a in argv if a]
        if len(argv) < 3 or os.path.basename(argv[0]) != 'ft-stream' or argv[1] != 'stream':
            continue
        host = argv[2]
        if host.count(':') == 1:   # HOST:PORT (a Mac's second Sunshine); an IPv6 address has more
            host = host.split(':')[0]
        cid = argv[argv.index('--id') + 1] if '--id' in argv[:-1] else None
        found.append({'pid': int(entry.name), 'host': host, 'id': cid})
    return found


def locks(proc='/proc'):
    """Each host's input lock (ft-stream's $XDG_RUNTIME_DIR/frametop-input-HOST.lock):
    [{host, held, pid}], read from PROC/locks so looking takes nothing."""
    held = {}
    try:
        for line in Path(proc, 'locks').read_text().splitlines():
            f = line.split()   # 1: FLOCK ADVISORY WRITE PID MAJ:MIN:INODE START END
            if len(f) >= 6 and f[1] == 'FLOCK' and f[3] == 'WRITE':
                held[f[5]] = int(f[4])
    except (OSError, ValueError):
        pass
    found = []
    for path in sorted(runtime_dir().glob('frametop-input-*.lock')):
        try:
            st = path.stat()
        except OSError:
            continue
        key = f'{os.major(st.st_dev):02x}:{os.minor(st.st_dev):02x}:{st.st_ino}'
        found.append({'host': path.name[len('frametop-input-'):-len('.lock')], 'held': key in held,
                      'pid': held.get(key)})
    return found


def log_lines(keep=20):
    """The last KEEP `hostinput:` lines of each remote display's ft-stream log
    ($XDG_RUNTIME_DIR/frametop-remote-N.log, which has no timestamps: its mtime says when it last
    wrote)."""
    found = []
    for path in sorted(runtime_dir().glob('frametop-remote-*.log')):
        try:
            with open(path, 'rb') as f:
                f.seek(max(0, f.seek(0, os.SEEK_END) - 65536))
                tail = f.read().decode(errors='replace').splitlines()
            mtime = path.stat().st_mtime
        except OSError:
            continue
        lines = [ln for ln in tail if ln.startswith('hostinput: ')][-keep:]
        if lines:
            found.append({'log': path.name, 'mtime': round(mtime, 3), 'lines': lines})
    return found


def state(layout=LAYOUT, proc='/proc', detail=False):
    """What a faked wearer would take: the live streams to hosts that lend their input. With
    DETAIL, also who holds each host's input lock and what ft-stream logged about leases."""
    hosts = input_hosts(layout)
    live = [dict(s, name=hosts[s['host']]) for s in streams(proc) if s['host'] in hosts]
    st = {'input_hosts': sorted(hosts.values()), 'input_streams': live, 'fake_worn': read_flag()}
    if detail:
        st.update(locks=locks(proc), logs=log_lines())
    return st


def refusal(st):
    """Why faking the wearer would take a host's input now, or None."""
    if not st['input_streams']:
        return None
    names = sorted({s['name'] for s in st['input_streams']})
    return (f'a remote display streams from {", ".join(names)}, which lends Frametop its keyboard and '
            'mouse ("input": true): a faked wearer would take them from whoever sits there. Turn that '
            "host's displays off, or pass --allow-host-input once the user allows it")
