"""Real native poll + Python persistence, private pipes/files and a real flock.

Build screens/test/volume-controls.py first. FT_VOLUME_TEST_EXE selects another
isolated build, including the sanitizer build. No production sockets or helpers.
"""
import fcntl
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "layout"))
import ft_layout as layout

EXE = Path(os.environ.get("FT_VOLUME_TEST_EXE", ROOT / "build/native-controls-cpu/volume-controls-vr-tests"))


@pytest.mark.parametrize("mode", ["--keyboard-release", "--catcher-ownership"])
def test_release_routing(mode):
    subprocess.run([str(EXE), mode], check=True, timeout=10)


@pytest.fixture
def native(tmp_path, monkeypatch):
    proc = subprocess.Popen([str(EXE), "--persistence-fixture"], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, text=True)
    pipe_lock = threading.Lock()

    def request(command):
        with pipe_lock:
            proc.stdin.write(command + "\n")
            proc.stdin.flush()
            reply = proc.stdout.readline().strip()
            assert reply, f"native fixture exited: {proc.poll()}"
            return reply

    path = tmp_path / "layout.json"
    path.write_text(json.dumps({"screens": [{"untouched": True}], "unknown": [None]}))
    monkeypatch.setattr(layout, "LAYOUT_PATH", str(path))
    monkeypatch.setattr(layout, "LOCK_PATH", str(tmp_path / "layout.lock"))
    monkeypatch.setattr(layout, "screens_socket", lambda: SimpleNamespace(
        ask=lambda cmd, **kwargs: request(cmd), sock=SimpleNamespace(close=lambda: None)))
    try:
        yield request, path
    finally:
        proc.stdin.write("quit\n")
        proc.stdin.flush()
        proc.wait(timeout=5)
        assert proc.returncode == 0


@pytest.mark.parametrize("supersede", ["cancel", "commit", "external"])
def test_delayed_release_never_saves_later_cancelled_gesture(native, monkeypatch, supersede):
    request, path = native
    request("move")
    committed = layout.parse_passthrough_state(request("release"))[0]
    command = request("capture-command")  # actual native argv, not a guessed helper
    waiting = threading.Event()
    real_flock = fcntl.flock
    results = []
    errors = []

    def flock(fd, operation):
        if threading.current_thread() is not threading.main_thread():
            waiting.set()
        return real_flock(fd, operation)

    monkeypatch.setattr(layout.fcntl, "flock", flock)

    def capture():
        try:
            results.append(layout.main(["ft-layout", command]))
        except BaseException as error:
            errors.append(error)

    workers = []
    with open(layout.LOCK_PATH, "w") as lock:
        real_flock(lock, fcntl.LOCK_EX)
        # Multiple delayed old helpers must coalesce to the latest committed state.
        for _ in range(2):
            waiting.clear()
            worker = threading.Thread(target=capture)
            workers.append(worker)
            worker.start()
            assert waiting.wait(5)
        held = layout.parse_passthrough_state(request("move"))[0]
        assert held != committed
        if supersede == "commit":
            committed = layout.parse_passthrough_state(request("release"))[0]
            held = layout.parse_passthrough_state(request("move"))[0]
        elif supersede == "external":
            request("passthrough set 0 1 2 -3 2 .5 1 23 12 -4")
            committed = layout.parse_passthrough_state(request("passthrough state3d"))[0]
            held = layout.parse_passthrough_state(request("move"))[0]
        assert held != committed
        real_flock(lock, fcntl.LOCK_UN)
    for worker in workers:
        worker.join(timeout=5)
        assert not worker.is_alive()
    assert not errors and results == [0, 0]
    reverted = layout.parse_passthrough_state(request("cancel"))[0]
    saved = json.loads(path.read_text())
    assert reverted == committed
    assert saved == {"screens": [{"untouched": True}], "unknown": [None], "passthrough": committed}


def test_committed_query_does_not_change_live_drag(native):
    request, _path = native
    original = request("passthrough committed")
    assert original == request("passthrough state3d")
    held = request("move")
    assert held != original
    assert request("passthrough committed") == original
    assert request("passthrough committed extra").startswith("error ")
    assert request("passthrough set 0 nan 2 -3 2 .5 1 23").startswith("error ")
    assert request("passthrough committed") == original
    assert request("passthrough state3d") == held
    assert request("release") == held
    assert request("passthrough committed") == held


def test_explicit_capture_keeps_live_held_semantics(native):
    request, path = native
    request("move")
    held = layout.parse_passthrough_state(request("passthrough state3d"))[0]
    assert layout.main(["ft-layout", "passthrough-capture"]) == 0
    assert json.loads(path.read_text())["passthrough"] == held


def test_release_capture_cli_rejects_extra_arguments(monkeypatch):
    monkeypatch.setattr(layout, "screens_socket", lambda: pytest.fail("unexpected socket"))
    assert layout.main(["ft-layout", "passthrough-capture-release", "extra"]) == 2
