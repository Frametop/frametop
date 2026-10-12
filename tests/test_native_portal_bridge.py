"""Native identity and recovery checks; no production sockets or camera controls."""
from concurrent.futures import Future
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


class Worker:
    def __init__(self, **_): self.calls=[]
    def submit(self, fn, enabled, identity):
        future=Future(); self.calls.append((enabled,future)); return future
    def shutdown(self, **_): pass


def test_control_recovery_never_waits_for_blocked_request(monkeypatch):
    monkeypatch.setattr(bridge, 'ThreadPoolExecutor', Worker)
    camera=bridge.RoomView(send=lambda _: None)
    identity=(123,456)
    assert not camera.update(True,identity,0)
    # A blocked network request does not block mask-loop calls or queue more work.
    for frame in range(180): assert not camera.update(True,identity,frame/90)
    assert len(camera.worker.calls)==1
    camera.worker.calls[0][1].set_result(None)
    assert camera.update(True,identity,2)
    assert camera.update(True,identity,3.1) # periodic reassert runs in the worker
    assert len(camera.worker.calls)==2
    assert not camera.update(False,identity,3.2) # game pause/off invalidates readiness
    camera.worker.calls[1][1].set_result(None)
    camera.update(False,identity,4.2)
    assert camera.worker.calls[-1][0] is False
    camera.worker.calls[-1][1].set_result(None)
    assert camera.update(False,identity,4.3)
    assert not camera.update(True,identity,4.4) # resume reapplies saved intent
    camera.worker.calls[-1][1].set_exception(OSError('compositor restarted'))
    assert not camera.update(True,(999,888),5.5)
    assert camera.worker.calls[-1][0] is True
    camera.worker.calls[-1][1].set_result(None)
    assert camera.update(True,(999,888),5.6)
    assert not camera.update(True,None,5.7) # rejected/missing native backend
    count=len(camera.worker.calls)
    camera.update(True,None,10)
    assert len(camera.worker.calls)==count
    camera.close()


def test_queued_control_rechecks_compositor_identity():
    calls = []
    camera = bridge.RoomView(send=calls.append, identity_check=lambda: (999,888))
    with pytest.raises(RuntimeError, match='compositor changed'):
        camera.deliver(True, (123,456))
    assert calls == []
    camera.deliver(True, (999,888))
    assert calls == [True]
    camera.close()


@pytest.mark.parametrize('supported', [False, True])
def test_bridge_never_requests_camera_on_unknown_native_build(tmp_path,monkeypatch,supported):
    calls=[]
    class Camera:
        error=None
        def __init__(self, **_): pass
        def update(self,enabled,identity,now): calls.append((enabled,identity));return identity is not None
        def close(self): pass
    monkeypatch.setattr(bridge,'RoomView',Camera)
    monkeypatch.setattr(sys,'argv',['bridge','--geometry',str(tmp_path/'mask')])
    monkeypatch.setattr(bridge,'STOP',False)
    monkeypatch.setattr(bridge.signal,'signal',lambda *_:None)
    monkeypatch.setattr(bridge.os,'umask',lambda *_:None)
    monkeypatch.setattr(bridge.time,'sleep',lambda _:setattr(bridge,'STOP',True))
    def status(_):
        if not supported: raise RuntimeError('unsupported compositor')
        return (123,456)
    monkeypatch.setattr(bridge,'native_status',status)
    monkeypatch.setattr(bridge,'sample',lambda: ([[0,0,0]]*3,True,False,[],True))
    ack=[]
    monkeypatch.setattr(bridge,'acknowledge',lambda ready,*_:ack.append(ready))
    bridge.main()
    assert any(enabled for enabled,identity in calls) == supported
    assert any(ack) == supported
