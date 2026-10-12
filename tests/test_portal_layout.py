"""Placement-only persistence and startup. No live socket or configuration writes."""
import json
from pathlib import Path
import sys
from types import SimpleNamespace
from unittest.mock import Mock
import pytest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "layout"))
import ft_layout as layout

VOLUME = dict(enabled=True, center=[1, 2, -3], size=[2, .5, 1], yaw=23, pitch=0, roll=0)

@pytest.fixture
def saved(tmp_path, monkeypatch):
    path = tmp_path / "layout.json"
    raw = dict(auto=False, screens=[{"unrecognized": [None, "keep"]}],
               profiles={"Work": {"windows": [1]}}, layouts={"Work": [2]}, unknown={"a": True},
               passthrough=VOLUME, passthrough_controls=True, portal_above_steam=True)
    path.write_text(json.dumps(raw))
    monkeypatch.setattr(layout, "LAYOUT_PATH", str(path))
    monkeypatch.setattr(layout, "LOCK_PATH", str(tmp_path / "lock"))
    monkeypatch.setattr(layout, "backend", lambda: "screens")
    return path, raw

@pytest.mark.parametrize("bad", [None, 1, "true", [], {}])
def test_controls_are_strict_boolean(bad):
    assert layout.passthrough_controls({"passthrough_controls": bad}) is False
    with pytest.raises(ValueError):
        layout.save_passthrough_controls(bad)

@pytest.mark.parametrize("field,bad", [("center", [True, 0, 0]), ("size", [1, 0, 1]),
    ("size", [1, 6, 1]), ("center", [101, 0, 0]), ("yaw", "2"), ("enabled", 1)])
def test_strict_geometry(field, bad):
    with pytest.raises(ValueError):
        layout.validate_passthrough(dict(VOLUME, **{field: bad}))

def test_controls_atomic_raw_preservation(saved, monkeypatch):
    path, raw = saved
    layout.save_passthrough_controls(False)
    assert json.loads(path.read_text()) == dict(raw, passthrough_controls=False)
    before = path.read_bytes()
    monkeypatch.setattr(layout.os, "replace", Mock(side_effect=OSError("injected")))
    with pytest.raises(OSError):
        layout.save_passthrough_controls(True)
    assert path.read_bytes() == before

@pytest.mark.parametrize("text", ["[]", "null", "{bad"])
def test_bad_root_never_overwritten(saved, text):
    path, _ = saved
    path.write_text(text)
    with pytest.raises(ValueError):
        layout.save_passthrough_controls(True)
    assert path.read_text() == text

def test_controls_ack_and_readback():
    sock = SimpleNamespace(ask=Mock(side_effect=["ok controls 1", "ok controls 0"]))
    with pytest.raises(ValueError):
        layout.send_passthrough_controls(sock, True)
    assert [c.args[0] for c in sock.ask.call_args_list] == ["passthrough controls 1", "passthrough controls state"]
    for reply in ("ok", "ok controls 01", "ok controls 1 extra", "ok controls 2"):
        with pytest.raises(ValueError):
            layout.parse_passthrough_controls(reply)

def test_startup_auto_off_restores_only_global_placement_and_preferences(saved, monkeypatch):
    path, raw = saved
    before = path.read_bytes()
    commands = []
    def ask(command, **kwargs):
        commands.append(command)
        if command.startswith("passthrough controls"): return "ok controls 1"
        if command.startswith("passthrough layer"): return "ok layer 1"
        return "ok"
    sock = SimpleNamespace(ask=ask)
    monkeypatch.setattr(layout, "screens_socket", lambda: sock)
    monkeypatch.setattr(layout, "screens_up", lambda _: 1)
    monkeypatch.setattr(layout, "apply_scales", lambda: [])
    monkeypatch.setattr(layout, "start_remotes", lambda *_: False)
    monkeypatch.setattr(layout, "apply", Mock(side_effect=AssertionError("must not arrange")))
    assert layout.main(["ft-layout", "start", "--wait", "1"]) == 0
    assert layout.passthrough_command(raw) in commands
    assert "passthrough controls 1" in commands and "passthrough layer 1" in commands
    assert not any(c.startswith(("place ", "portal ")) for c in commands)
    assert path.read_bytes() == before

def test_capture_cli_rejects_extra_arguments(monkeypatch):
    capture = Mock(return_value=VOLUME)
    monkeypatch.setattr(layout, "capture_passthrough", capture)
    assert layout.main(["ft-layout", "passthrough-capture", "extra"]) == 2
    capture.assert_not_called()
    assert layout.main(["ft-layout", "passthrough-capture"]) == 0
    capture.assert_called_once_with()


def test_named_layouts_restore_independent_passthrough_placements():
    data = dict(layout.DEFAULTS, count=1, screens=[{"pos": [0, 0, -2]}], passthrough=VOLUME)
    layout.save_named(data, "Desk")
    other = dict(VOLUME, center=[-1, 1, -2], yaw=-40, pitch=35, roll=12)
    data["passthrough"] = other
    layout.save_named(data, "Couch")
    layout.use_named(data, "Desk")
    assert data["passthrough"] == VOLUME
    data["passthrough"]["center"][0] = 9
    layout.use_named(data, "Couch")
    assert data["passthrough"] == other
    layout.use_named(data, "Desk")
    assert data["passthrough"] == VOLUME
    layout.rename_named(data, "Desk", "Work")
    assert data["profiles"]["Work"]["passthrough"] == VOLUME
    data["layouts"]["Old"] = data["layouts"]["Work"]
    layout.use_named(data, "Old")
    assert data["passthrough"] == VOLUME
