"""User-only SteamVR injection installation. Never modifies system files or SSH."""
import hashlib
import os
from pathlib import Path
import tempfile

LIBRARIES = ('libframe_loader.so', 'libframe_input.so')
MARKER = '# Managed by frame-testbench. Remove this file to disable injection.\n'


def target(home):
    return Path(home) / '.config/systemd/user/steamvr.service.d/90-frame-testbench.conf'


def dropin(directory):
    directory = Path(directory).resolve()
    value = str(directory)
    if ':' in value or any(c.isspace() or ord(c) < 32 for c in value):
        raise ValueError('LD_PRELOAD library directory cannot contain whitespace or colons')
    value = value.replace('\\', '\\\\').replace('"', '\\"').replace('%', '%%')
    return (MARKER + '[Service]\n'
            f'Environment="LD_PRELOAD={value}/libframe_loader.so"\n'
            f'Environment="FRAME_TESTBENCH_PROXY={value}/libframe_input.so"\n')


def atomic(path, data, mode=0o600):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix='.frame-testbench-', dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(name, mode)
        os.replace(name, path)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def stage(build, home):
    build, home = Path(build), Path(home)
    files = {name: (build / name).read_bytes() for name in LIBRARIES}
    digest = hashlib.sha256()
    for name, data in files.items():
        digest.update(name.encode() + b'\0' + data)
    directory = home / '.local/share/frame-testbench' / digest.hexdigest()[:20]
    text = dropin(directory)
    path = target(home)
    if path.exists() and not path.read_text().startswith(MARKER):
        raise ValueError(f'refusing unowned service override: {path}')
    for name, data in files.items():
        dest = directory / name
        if dest.exists():
            if dest.read_bytes() != data:
                raise ValueError(f'immutable library differs: {dest}')
        else:
            atomic(dest, data, 0o700)
    atomic(path, text.encode())
    return {'dropin': str(path), 'library_dir': str(directory), 'restart_required': True}


def remove(home):
    path = target(home)
    if path.exists():
        if not path.read_text().startswith(MARKER):
            raise ValueError(f'refusing unowned service override: {path}')
        path.unlink()
    return {'dropin_removed': str(path), 'restart_required': True}
