"""Rotation schema and release-capture persistence. Private paths and IPC only."""
import json
from pathlib import Path
import sys
from types import SimpleNamespace
from unittest.mock import Mock
import pytest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'layout'))
import ft_layout as layout

OLD = dict(enabled=True, center=[1,2,3], size=[2,1,3], yaw=31)
FULL = dict(OLD, pitch=24, roll=-17)
STATE = 'ok 1 1 2 3 2 1 3 31 24 -17 ready'


def test_saved_old_schema_and_extended_ipc_roundtrip():
    assert layout.validate_passthrough(OLD) == OLD
    parsed,status=layout.parse_passthrough_state(STATE)
    assert parsed==FULL and status=='ready'
    assert layout.passthrough_command({'passthrough':FULL}).endswith('31 24 -17')
    assert layout.passthrough_command({'passthrough':OLD}).endswith('31')
    assert layout.passthrough_command({'passthrough':dict(OLD, roll=2)}).endswith('31 0 2')


@pytest.mark.parametrize('key', ['pitch','roll'])
@pytest.mark.parametrize('bad', [True, '1', None, float('nan'), float('inf'), -181, 181])
def test_invalid_rotation_does_not_restore_enabled_volume(key,bad):
    value=dict(FULL, **{key:bad})
    with pytest.raises(ValueError):layout.validate_passthrough(value)
    assert layout.passthrough({'passthrough':value})['enabled'] is False


@pytest.mark.parametrize('bad', ['ok 1 1 2 3 2 1 3 31 24 ready',
    'ok 1 1 2 3 2 1 3 31 24 -17 extra', 'ok 1 1 2 3 2 1 3 31 24 -17 ready extra',
    'ok 1 1 2 3 2 1 3 31 nan -17 ready', 'ok 1 1 2 3 2 1 3 31 181 -17 ready'])
def test_extended_state_strict(bad):
    with pytest.raises(ValueError):layout.parse_passthrough_state(bad)


def test_release_capture_persists_all_angles_and_preserves_unrelated_layout(tmp_path,monkeypatch):
    path=tmp_path/'layout.json'
    saved={'screens':[{'untouched':'raw'}], 'profiles':{'Work':{}}, 'unknown':[1,None]}
    path.write_text(json.dumps(saved))
    monkeypatch.setattr(layout,'LAYOUT_PATH',str(path))
    monkeypatch.setattr(layout,'LOCK_PATH',str(tmp_path/'lock'))
    transport=SimpleNamespace(ask=Mock(return_value=STATE),sock=SimpleNamespace(close=Mock()))
    monkeypatch.setattr(layout,'screens_socket',lambda:transport)
    assert layout.capture_passthrough()==FULL
    transport.ask.assert_called_once_with('passthrough state3d',timeout=1.0)
    transport.sock.close.assert_called_once_with()
    assert json.loads(path.read_text())==dict(saved,passthrough=FULL)
    assert layout.passthrough_command(layout.load_layout()).endswith('31 24 -17')
    transport.ask.return_value='ok 1 1 2 3 2 1 3 31 nan -17 ready'
    with pytest.raises(RuntimeError):layout.capture_passthrough()
    assert json.loads(path.read_text())==dict(saved,passthrough=FULL)
