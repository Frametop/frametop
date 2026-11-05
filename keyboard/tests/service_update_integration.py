"""ARM64 opt-in service update/rollback regression, offline and input-disabled.

Usage: python3 service_update_integration.py release.tar.gz update-tests
Uses uniquely named disposable user services, never the installed keyboard unit.
"""
from pathlib import Path
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tarfile
import tempfile
import uuid
from startup_service_tests import cleanup, eventually, manager

archive, fixture = [Path(a).resolve() for a in sys.argv[1:]]
match = re.fullmatch(r'framekeyboard-(\d+\.\d+\.\d+)-aarch64.tar.gz', archive.name)
assert match
number = match[1]
digest = hashlib.sha256(archive.read_bytes()).hexdigest()
results = {}
with tempfile.TemporaryDirectory(prefix='full-keyboard-service-update-') as directory:
    base = Path(directory)
    with tarfile.open(archive) as source:
        source.extractall(base, filter='data')
    package = base / ('framekeyboard-' + number + '-aarch64')
    helper = package / 'share/framekeyboard/update/update_helper.py'
    selected = dict(tag=number, number=number, parts=list(map(int, number.split('.'))),
                    archive=archive.name, assets={
                        archive.name: dict(browser_download_url='https://fixture/archive',
                                           size=archive.stat().st_size, digest='sha256:' + digest),
                        'SHA256SUMS': dict(browser_download_url='https://fixture/sums')})
    for mode in ('success', 'rollback'):
        work = base / mode
        root = work / 'install'
        old = root / 'releases/old'
        shutil.copytree(package, old)
        shutil.copy2(fixture, old / 'bin/framekeyboard')
        (root / 'current').symlink_to('releases/old')
        (root / 'previous').symlink_to('releases/older')
        config = work / 'config'
        config.mkdir()
        shim = work / 'helper.py'
        shim.write_text(f'''import hashlib,importlib.util,json,shutil,sys
spec=importlib.util.spec_from_file_location('updates',{str(helper)!r})
u=importlib.util.module_from_spec(spec);spec.loader.exec_module(u)
selected=json.loads({json.dumps(selected)!r});selected['parts']=tuple(selected['parts'])
u.release=lambda tag=None:selected
u.read_url=lambda url,limit:{(digest+'  '+archive.name+chr(10)).encode()!r}
def download(url,output,limit,expected_size=None):
    with open({str(archive)!r},'rb') as source:shutil.copyfileobj(source,output)
    return {digest!r}
u.download=download
launch=u.launch
def bounded_launch(package,arguments,timeout=45):
    if package.name!='old':arguments=[*arguments,'--duration',{'1' if mode=='rollback' else '30'!r}]
    return launch(package,arguments,timeout)
u.launch=bounded_launch
raise SystemExit(u.main())
''')
        name = 'full-keyboard-update-test-' + uuid.uuid4().hex[:12] + '.service'
        unit = work / name
        unit.write_text('[Unit]\nStartLimitIntervalSec=60\nStartLimitBurst=3\n'
                        '[Service]\nType=exec\nNotifyAccess=all\n'
                        'Environment=FRAMEKEYBOARD_SERVICE=' + name + '\n'
                        # The fixture owns a private instance directory explicitly.
                        # Keep the real user runtime so systemctl's local transport
                        # can reach the manager, just as it does in production.
                        'Environment=SDL_VIDEODRIVER=dummy\n'
                        'ExecStart=' + str(old/'bin/framekeyboard') + ' --handoff-fixture ' + str(shim) + ' ' + str(config) + '\n'
                        'Restart=always\nRestartSec=3\nRestartPreventExitStatus=3\nSuccessExitStatus=3\nKillMode=process\n')
        unitdir = Path.home() / '.config/systemd/user'
        unitdir.mkdir(parents=True, exist_ok=True)
        link = unitdir / name
        assert not link.exists() and not link.is_symlink()
        link.symlink_to(unit)
        try:
            manager('daemon-reload'); manager('start', name)
            initial = int(manager('show', name, '-p', 'MainPID', '--value').stdout)
            result = root / 'update-status.log'
            eventually(lambda: result.exists() and ('acknowledged' if mode=='success' else 'relaunched') in result.read_text(), timeout=35)
            current = (root / 'current').resolve()
            assert (current != old) == (mode == 'success'), result.read_text()
            adopted = int(manager('show', name, '-p', 'MainPID', '--value').stdout)
            assert adopted > 0 and adopted != initial
            eventually(lambda: not Path('/proc/'+str(initial)).exists())
            assert Path('/proc/'+str(adopted)+'/exe').resolve() == current/'bin/framekeyboard'
            assert manager('is-active', name).stdout.strip() == 'active'
            assert int(manager('show', name, '-p', 'NRestarts', '--value').stdout) == 0
            os.kill(adopted, signal.SIGKILL)
            eventually(lambda: int(manager('show', name, '-p', 'NRestarts', '--value').stdout) >= 1)
            results[mode] = {'main_pid_transferred': True, 'original_exited_without_restart': True,
                             'successor_crash_restarts_service': True, 'status': result.read_text().strip()}
        except BaseException:
            print(subprocess.run(['journalctl', '--user', '-u', name, '-n', '25', '--no-pager'],
                                 capture_output=True, text=True).stdout, file=sys.stderr)
            raise
        finally:
            cleanup([name], [link])
print(json.dumps(results, indent=2))
