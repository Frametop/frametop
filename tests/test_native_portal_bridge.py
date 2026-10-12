"""A failed mask/control publication must not advertise desktop alpha readiness."""
from pathlib import Path
import sys
import pytest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "native-portal"))
import bridge


@pytest.mark.parametrize("failure", [None, "publish", "control"])
def test_readiness_follows_publication(tmp_path, monkeypatch, failure):
    events = []
    monkeypatch.setattr(sys, "argv", ["bridge", "--geometry", str(tmp_path / "mask")])
    monkeypatch.setattr(bridge, "STOP", False)
    monkeypatch.setattr(bridge.signal, "signal", lambda *_: None)
    monkeypatch.setattr(bridge.os, "umask", lambda *_: None)
    monkeypatch.setattr(bridge.time, "sleep", lambda _: setattr(bridge, "STOP", True))
    monkeypatch.setattr(bridge, "sample", lambda: ([[0, 0, 0]] * 3, True, False, [], True))
    def publish(_path, *args):
        events.append("publish" if args else "clear")
        if args and failure == "publish":
            raise OSError("publication failed")
    def control(enabled):
        events.append(("control", enabled))
        if enabled and failure == "control":
            raise OSError("control failed")
    monkeypatch.setattr(bridge, "publish", publish)
    monkeypatch.setattr(bridge.control, "send", control)
    monkeypatch.setattr(bridge, "acknowledge", lambda ready: events.append(("ready", ready)))
    bridge.main()
    if failure:
        assert ("ready", True) not in events
        assert ("ready", False) in events
    else:
        assert events[:3] == ["publish", ("control", True), ("ready", True)]
    assert events[-3:] == [("ready", False), "clear", ("control", False)]
