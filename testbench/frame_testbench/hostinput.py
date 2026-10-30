"""Remote displays whose host lends Frametop its keyboard and mouse (docs/host-input.md), and the
guard that keeps a faked wearer from taking them.

ft-stream leases a host's keyboard and mouse while the pointer helper says the headset is worn,
and the helper can't tell the testbench's `worn on` from a real head. So a faked wearer with a
live stream to a host that has "input": true on its layout entry takes that computer's keyboard
and mouse away from whoever sits at it (2026-10-30: oma7, mid-sentence). The testbench refuses to
fake the wearer then, unless asked with --allow-host-input. Otherwise, while it fakes the wearer,
a guard process (`python3 -m frame_testbench.hostinput guard`, its own user unit) holds every
such host's input lock: ft-stream leases only from the stream holding it
($XDG_RUNTIME_DIR/frametop-input-HOST.lock), so no stream that starts later can lease either.
FLAG says the wearer is faked, for ft-stream and for the commands that follow.

FLAG ($XDG_RUNTIME_DIR/frametop-fake-worn.json) is JSON: {"since": UNIX TIME, "pid": PID,
"allow_host_input": BOOL}, PID the guard's (the CLI's when allowed: nothing to guard). It's
written before the wearer is faked and goes when the wearer does (`worn off|physical`,
`release`); the guard lets go of the locks once FLAG goes or names another process.
"""
import fcntl
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

LAYOUT = Path.home() / '.config/frametop-layout.json'
GUARD_UNIT = 'frametop-fake-worn-guard'
GUARD_POLL = 0.5


def runtime_dir():
    """ft-screens' and ft-stream's runtime dir: XDG_RUNTIME_DIR, but not a nested desktop's
    (/run/user/UID/frametop), whose terminals would put FLAG where ft-stream doesn't look."""
    base = f'/run/user/{os.getuid()}'
    path = os.environ.get('XDG_RUNTIME_DIR') or base
    return Path(base if path.startswith(base + '/') else path)


def flag_path():
    return runtime_dir() / 'frametop-fake-worn.json'


def lock_path(host):
    """HOST's input lock, named as stream/hostinput.h's RelayId names it."""
    rid = ''.join(c if c.isalnum() or c in '._-' else '-' for c in host[:64]) or 'host'
    return runtime_dir() / f'frametop-input-{rid}.lock'


def read_flag():
    """The fake-worn flag, or None. A garbled one counts as not allowed."""
    try:
        flag = json.loads(flag_path().read_text())
    except FileNotFoundError:
        return None
    except (OSError, ValueError):
        return {'allow_host_input': False}
    return flag if isinstance(flag, dict) else {'allow_host_input': False}


def set_flag(allow, now, pid=None):
    path = flag_path()
    tmp = path.with_name(path.name + f'.{os.getpid()}')
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_CLOEXEC, 0o600)
    with os.fdopen(fd, 'w') as f:
        json.dump({'since': now, 'pid': pid or os.getpid(), 'allow_host_input': bool(allow)}, f)
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


def take_locks(hosts, held):
    """Take each of HOSTS' input locks not in HELD (address -> fd) yet. The hosts whose lock
    another process holds."""
    busy = []
    for host in hosts:
        if host in held:
            continue
        fd = os.open(lock_path(host), os.O_RDWR | os.O_CREAT | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            held[host] = fd
        except BlockingIOError:
            os.close(fd)
            busy.append(host)
    return busy


def guard(layout=LAYOUT, poll=GUARD_POLL, release=None):
    """The guard process: take every input host's lock, then write FLAG naming this process, and
    hold them while FLAG does. A host given "input": true meanwhile has its lock taken too; if a
    stream holds it, or the guard is told to stop while FLAG still names it, the faked wearer goes
    (RELEASE) and FLAG with it. Exit 0 once FLAG has gone, 3 if a lock was busy from the start."""
    held = {}
    stop = []
    signal.signal(signal.SIGTERM, lambda *_: stop.append(1))
    signal.signal(signal.SIGHUP, lambda *_: stop.append(1))
    busy = take_locks(input_hosts(layout), held)
    if busy:
        print('busy: ' + ' '.join(busy), flush=True)
        return 3
    set_flag(False, time.time())
    print('holding: ' + ' '.join(sorted(held)), flush=True)
    try:
        while not stop:
            time.sleep(poll)
            if (read_flag() or {}).get('pid') != os.getpid():
                return 0
            busy = take_locks(input_hosts(layout), held)
            if busy:
                print('a stream holds the input lock of ' + ' '.join(busy) + ': releasing the wearer', flush=True)
                break
        if (read_flag() or {}).get('pid') == os.getpid():
            try:
                (release or release_wearer)()
            except OSError:
                pass   # no input proxy: nothing fakes the wearer
            clear_flag()
        return 0
    finally:
        for fd in held.values():
            os.close(fd)


def release_wearer():
    from .cli import Bench   # the guard runs on its own, as `python3 -m frame_testbench.hostinput guard`
    Bench().control('worn-release')


def start_guard(timeout=5.0):
    """Start the guard as its own user unit (an agent's shell may be killed with all it started)
    and wait till it holds the locks and wrote FLAG. Its pid, or raises with why it couldn't."""
    package = Path(__file__).resolve().parent.parent
    subprocess.run(['systemctl', '--user', 'stop', GUARD_UNIT], capture_output=True, timeout=20)
    clear_flag()
    done = subprocess.run(['systemd-run', '--user', '--collect', '--quiet', f'--unit={GUARD_UNIT}',
                           f'--working-directory={package}', f'--setenv=XDG_RUNTIME_DIR={runtime_dir()}',
                           sys.executable, '-m', 'frame_testbench.hostinput', 'guard'],
                          capture_output=True, text=True, timeout=20)
    if done.returncode:
        raise RuntimeError(f'the host-input guard didn\'t start: {done.stderr.strip()}')
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        flag = read_flag()
        if flag and flag.get('allow_host_input') is False and guard_alive(flag):
            return flag['pid']
        if subprocess.run(['systemctl', '--user', 'is-active', '--quiet', GUARD_UNIT]).returncode:
            break
        time.sleep(0.05)
    log = subprocess.run(['journalctl', '--user', '-u', GUARD_UNIT, '-n', '3', '-o', 'cat', '--no-pager'],
                         capture_output=True, text=True).stdout.strip()
    subprocess.run(['systemctl', '--user', 'stop', GUARD_UNIT], capture_output=True, timeout=20)
    raise RuntimeError('the host-input guard couldn\'t take the input locks' + (f' ({log})' if log else ''))


def guard_alive(flag, proc='/proc'):
    """Whether FLAG's pid is a running guard."""
    try:
        argv = Path(proc, str(int(flag.get('pid'))), 'cmdline').read_bytes().split(b'\0')
    except (OSError, TypeError, ValueError):
        return False
    return b'frame_testbench.hostinput' in argv and b'guard' in argv


def state(layout=LAYOUT, proc='/proc', detail=False):
    """What a faked wearer would take: the live streams to hosts that lend their input. With
    DETAIL, also who holds each host's input lock and what ft-stream logged about leases."""
    hosts = input_hosts(layout)
    live = [dict(s, name=hosts[s['host']]) for s in streams(proc) if s['host'] in hosts]
    flag = read_flag()
    st = {'input_hosts': sorted(hosts.values()), 'input_streams': live, 'fake_worn': flag,
          'guarded': bool(flag) and (flag.get('allow_host_input') is True or guard_alive(flag, proc))}
    if detail:
        st.update(locks=locks(proc), logs=log_lines())
    return st


def refusal(st):
    """Why faking the wearer would take a host's input now, or None."""
    if not st['input_streams']:
        return None
    names = sorted({s['name'] for s in st['input_streams']})
    return (f'a remote display streams from {", ".join(names)}, which lends Frametop its keyboard and '
            'mouse ("input": true): a faked wearer would take them from whoever sits there. Wait for '
            'that stream to end, or pass --allow-host-input once the user allows it')


if __name__ == '__main__' and sys.argv[1:2] == ['guard']:
    sys.exit(guard(*sys.argv[2:3]))   # a layout path only in tests
