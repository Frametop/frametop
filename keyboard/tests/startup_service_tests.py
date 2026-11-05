"""Opt-in user-manager regression. Only uniquely named disposable services run.

Usage: python3 tests/startup_service_tests.py /path/to/startup-tests /path/to/framekeyboard
Never stops or restarts SteamVR, Gamescope, or an installed keyboard.
"""
from pathlib import Path
import json
import os
import signal
import subprocess
import sys
import tempfile
import time
import uuid


def manager(*args, check=True):
    return subprocess.run(['systemctl', '--user', *args], capture_output=True, text=True,
                          timeout=10, check=check)


def eventually(predicate, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.05)
    raise AssertionError('Disposable service did not reach the expected state')


def cleanup(names, links):
    for name in names:
        manager('stop', name, check=False)
        # KillMode=process preserves independent helpers. Cleanup only these
        # disposable cgroups, including a child left by an interrupted fixture.
        for proc in Path('/proc').iterdir():
            try:
                if proc.name.isdecimal() and any(line.rsplit('/', 1)[-1] == name
                        for line in (proc / 'cgroup').read_text().splitlines()):
                    os.kill(int(proc.name), signal.SIGKILL)
            except OSError:
                pass
    for link in links:
        link.unlink(missing_ok=True)
    manager('daemon-reload')
    for name in names:
        manager('reset-failed', name, check=False)


if __name__ == '__main__':
    generator, binary = [str(Path(a).resolve()) for a in sys.argv[1:]]
    token = uuid.uuid4().hex[:12]
    provider = 'full-keyboard-test-provider-' + token + '.service'
    client = 'full-keyboard-test-client-' + token + '.service'
    units = Path.home() / '.config/systemd/user'
    units.mkdir(parents=True, exist_ok=True)
    links = []
    with tempfile.TemporaryDirectory(prefix='full-keyboard-service-') as directory:
        base = Path(directory)
        fixture = base / 'fixture'
        subprocess.run([generator, str(fixture)], check=True, capture_output=True)
        config = fixture / 'config'
        config.mkdir()
        count = config / 'starts'
        app = fixture / 'install/releases/test/bin/framekeyboard'
        app.write_text('''#!/usr/bin/python3
from pathlib import Path
import signal,sys,time
signal.signal(signal.SIGTERM,lambda *args:sys.exit(0))
b=Path(sys.argv[sys.argv.index('--config-dir')+1]); c=b/'starts'
c.write_text(str(int(c.read_text())+1) if c.exists() else '1')
while True:
    for marker,code in [('quit',3),('runtime',0)]:
        if (b/marker).exists():
            (b/marker).unlink(); (b/'exited').touch(); raise SystemExit(code)
    time.sleep(.02)
''')
        app.chmod(0o700)
        script = base / 'provider.py'
        script.write_text(f'''from pathlib import Path
import time
b=Path({str(config)!r}); c=b/'provider-starts'
c.write_text(str(int(c.read_text())+1) if c.exists() else '1')
while not (b/'restart-provider').exists(): time.sleep(.02)
(b/'restart-provider').unlink(); (b/'exited').unlink(missing_ok=True); (b/'runtime').touch()
end=time.monotonic()+1
while not (b/'exited').exists() and time.monotonic()<end: time.sleep(.02)
''')
        provider_unit = base / provider
        provider_unit.write_text('[Service]\nType=exec\nExecStart=/usr/bin/python3 ' + str(script) +
                                 '\nRestart=always\nRestartSec=0.2\n')
        generated = (fixture / 'units/framekeyboard-autostart.service').read_text()
        generated = generated.replace('steamvr.service', provider).replace('RestartSec=3', 'RestartSec=0.2')
        generated = generated.replace('FRAMEKEYBOARD_SERVICE=framekeyboard-autostart.service',
                                      'FRAMEKEYBOARD_SERVICE=' + client)
        client_unit = base / client
        client_unit.write_text(generated)
        for name, target in ((provider, provider_unit), (client, client_unit)):
            link = units / name
            assert not link.exists() and not link.is_symlink()
            link.symlink_to(target); links.append(link)
        wanted = units / (provider + '.wants')
        wanted.mkdir()
        dependency = wanted / client
        dependency.symlink_to('../' + client)
        links.append(dependency)
        starts = lambda: int(count.read_text()) if count.exists() else 0
        try:
            parsed = subprocess.run(['systemd-analyze', '--user', 'verify', str(client_unit)],
                                    text=True, capture_output=True)
            assert parsed.returncode == 0 and not parsed.stderr.strip(), parsed.stderr
            manager('daemon-reload'); manager('start', provider)
            eventually(lambda: starts() == 1)
            (config / 'runtime').touch()
            eventually(lambda: starts() == 2)
            manager('kill', '--signal=SIGTERM', client)
            eventually(lambda: starts() == 3)
            (config / 'quit').touch()
            eventually(lambda: manager('is-active', client, check=False).stdout.strip() == 'inactive')
            time.sleep(.6)
            assert starts() == 3, 'Intentional Quit restarted'
            manager('restart', provider)
            eventually(lambda: starts() == 4)
            (config / 'restart-provider').touch()
            eventually(lambda: starts() >= 5)
            manager('stop', client)
            app.rename(app.with_name('missing-framekeyboard'))
            before = starts()
            manager('start', client)
            assert manager('show', client, '-p', 'ConditionResult', '--value').stdout.strip() == 'no'
            time.sleep(.6)
            assert starts() == before, 'Missing installation was retried'
            for args in (['--version'], ['--check', '--config-dir', str(base/'private')]):
                subprocess.run([binary, *args], check=True, capture_output=True)
            guard = subprocess.run([binary, '--vr', '--autostart', '--input', 'none',
                                    '--config-dir', str(base/'private')], capture_output=True)
            assert guard.returncode == 3, guard.stderr
            print(json.dumps({'runtime_exit_restart': True, 'watchdog_sigterm_recovers': True,
                'quit_pauses': True,
                'provider_restart_recovers': True, 'automatic_provider_restart_recovers': True,
                'missing_installation_skipped': True, 'boot_guard_status': 3,
                'cli_validation_status': 0}))
        finally:
            cleanup([client, provider], links)
            wanted.rmdir()
