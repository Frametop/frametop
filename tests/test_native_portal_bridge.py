"""Native identity and recovery checks; no production sockets or camera controls."""
import json
import os
from pathlib import Path
import sys
import pytest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "native-portal"))
import bridge


def test_native_status_requires_same_live_process_and_successful_init(tmp_path):
    process = tmp_path / '123'
    process.mkdir()
    (process/'exe').write_text('compositor')
    executable = (process/'exe').stat()
    (process/'stat').write_text('123 (vrcompositor) ' + ' '.join(['S']+['0']*18+['456']))
    state = dict(version=1, pid=123, start_ticks=456, exe_dev=executable.st_dev,
                 exe_ino=executable.st_ino, ready=True, reason='ready')
    path = tmp_path/'status'
    path.write_text(json.dumps(state))
    assert bridge.native_status(path, tmp_path)[:2] == (123,456)
    for updates, message in [(dict(start_ticks=99), 'stale'),
                             (dict(exe_ino=0), 'stale'),
                             (dict(ready=False, reason='unsupported compositor'), 'unsupported')]:
        path.write_text(json.dumps(dict(state, **updates)))
        with pytest.raises(RuntimeError, match=message): bridge.native_status(path, tmp_path)
    path.unlink()
    with pytest.raises(OSError): bridge.native_status(path, tmp_path)


@pytest.mark.parametrize('supported', [False, True])
def test_bridge_publishes_only_for_ready_native_build(tmp_path, monkeypatch, supported):
    monkeypatch.setattr(sys, 'argv', ['bridge', '--geometry', str(tmp_path/'mask')])
    monkeypatch.setattr(bridge, 'STOP', False)
    monkeypatch.setattr(bridge.signal, 'signal', lambda *_: None)
    monkeypatch.setattr(bridge.os, 'umask', lambda *_: None)
    monkeypatch.setattr(bridge.time, 'sleep', lambda _: setattr(bridge, 'STOP', True))
    def status(_):
        if not supported:
            raise RuntimeError('unsupported compositor')
        return (123, 456)
    monkeypatch.setattr(bridge, 'native_status', status)
    monkeypatch.setattr(bridge, 'sample', lambda: ([[0, 0, 0]]*3, True, False, []))
    published, acknowledgements = [], []
    monkeypatch.setattr(bridge, 'publish', lambda *args: published.append(args))
    monkeypatch.setattr(bridge, 'acknowledge', lambda ready, *reason: acknowledgements.append((ready, reason)))
    bridge.main()
    assert any(len(args) > 1 for args in published) == supported
    assert any(ready for ready, _ in acknowledgements) == supported
    if not supported:
        assert 'unsupported compositor' in acknowledgements[0][1][0]
    assert published[-1] == (tmp_path/'mask',)
    assert acknowledgements[-1][0] is False
