"""Exercise the actual Qt object and shipped QML, private config/socket only."""
import importlib
import json
import os
import subprocess
import sys
import textwrap
import threading
import time

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
os.environ.setdefault("QT_QUICK_BACKEND", "software")
import pytest
from PySide6.QtCore import Qt, QUrl
from PySide6.QtGui import QGuiApplication
from PySide6.QtQml import QQmlApplicationEngine
from PySide6.QtTest import QTest
from test_native_portal_client import REPO, Server, status, wire

sys.path.insert(0, str(REPO / "layout"))
import ft_layout

APP = QGuiApplication.instance() or QGuiApplication([])
VOLUME = {"enabled": True, "center": [.1, 1.2, -.8], "size": [.8, .1, .6], "yaw": 12., "pitch": 7., "roll": -4.}


class Native(Server):
    def __init__(self):
        self.value = status()
        self.volume = json.loads(json.dumps(VOLUME))
        self.controls = self.layer = False
        self.delay = 0
        self.bad = None
        super().__init__(self.respond)

    def respond(self, cmd):
        if self.delay:
            time.sleep(self.delay)
        if cmd.startswith("passthrough portal "):
            if self.bad is not None:
                return self.bad
            if cmd.endswith((" on", " off")):
                on = cmd.endswith(" on")
                self.value.update(wanted=on)
            return wire(self.value)
        for key in ("controls", "layer"):
            if cmd.startswith("passthrough " + key + " "):
                if not cmd.endswith(" state"):
                    setattr(self, key, cmd.endswith(" 1"))
                return f"ok {key} {int(getattr(self, key))}"
        if cmd == "head":
            return "ok 1 1.8 -2 90"
        if cmd.startswith("passthrough set "):
            fields = cmd.split()[2:]
            self.volume = {"enabled": fields[0] == "1", "center": list(map(float, fields[1:4])),
                "size": list(map(float, fields[4:7])), "yaw": float(fields[7]),
                "pitch": float(fields[8]) if len(fields) > 8 else 0., "roll": float(fields[9]) if len(fields) > 9 else 0.}
            v = self.volume
            return "ok " + " ".join(map(str, [int(v["enabled"]), *v["center"], *v["size"], v["yaw"]])) + " idle"
        if cmd == "passthrough state3d":
            v = self.volume
            return "ok " + " ".join(map(str, [int(v["enabled"]), *v["center"], *v["size"], v["yaw"], v["pitch"], v["roll"]])) + " idle"
        raise AssertionError("unexpected command " + cmd)


def wait(predicate, timeout=4):
    deadline = time.monotonic() + timeout
    while not predicate():
        APP.processEvents()
        assert time.monotonic() < deadline, "Qt job timed out"
        time.sleep(.002)
    APP.processEvents()


@pytest.fixture
def setup(tmp_path, monkeypatch):
    assert (REPO / "display-settings/native_portal_settings.py").exists(), "Qt native settings adapter is missing"
    module = importlib.import_module("native_portal_settings")
    config = tmp_path / "layout.json"
    raw = {"screens": [{"future": 99}], "profiles": {"keep": {"unknown": True}},
               "unknown": [1, None], "passthrough": VOLUME, "passthrough_controls": True}
    config.write_text(json.dumps(raw))
    conf = tmp_path / "frametop.conf"
    conf.write_text("BACKEND=screens\n")
    for key, value in (("LAYOUT_PATH", config), ("LOCK_PATH", tmp_path / "lock"), ("CONF_PATH", conf)):
        monkeypatch.setattr(ft_layout, key, str(value))
    with Native() as server:
        model = module.PortalSettings(address=server.address, timeout=.3)
        wait(lambda: not model.busy)
        yield model, server, config, raw
        model.close()
        wait(lambda: not model.busy)
        model.deleteLater()
        APP.processEvents()


def test_open_poll_close_never_change_intent_or_persist(setup):
    model, server, config, _raw = setup
    before = config.read_bytes()
    model.refresh()
    wait(lambda: not model.busy)
    model.close()
    assert all(cmd.endswith((" status", " state", " state3d")) for cmd in server.commands)
    assert config.read_bytes() == before
    assert model.portal["wanted"] is False
    assert "camera_capturing" not in model.portal
    assert model.volume == VOLUME


def test_portal_commands_are_nonblocking_serialized_and_not_saved(setup):
    model, server, config, _raw = setup
    before = config.read_bytes()
    server.commands.clear()
    server.delay = .02
    start = time.monotonic()
    model.setWanted(True)
    model.setWanted(False)
    model.setWanted(True)
    assert time.monotonic() - start < .05
    wait(lambda: not model.busy)
    assert server.commands == [f"passthrough portal {c}" for c in ("on", "status", "off", "status", "on", "status")]
    assert model.portal["wanted"] is True
    assert model.portal["summary"] == "Desk passthrough requested"
    assert config.read_bytes() == before


def test_off_works_when_publisher_disconnected(setup):
    model, server, config, _ = setup
    before = config.read_bytes()
    server.value["connected"] = False
    model.setWanted(True)
    wait(lambda: not model.busy)
    assert model.portal["summary"] == "Passthrough publisher unavailable"
    model.setWanted(False)
    wait(lambda: not model.busy)
    assert model.portal["wanted"] is False
    assert model.portal["summary"] == "Desk passthrough off"
    assert config.read_bytes() == before



def test_closing_settings_leaves_confirmed_native_on_intent_alone(setup):
    model, server, *_ = setup
    model.setWanted(True)
    wait(lambda: not model.busy)
    server.commands.clear()
    model.close()
    APP.processEvents()
    assert not server.commands
    assert server.value["wanted"] is True


def test_close_preserves_accepted_off_after_pending_on(setup):
    model, server, config, _ = setup
    model._timer.stop()
    before = config.read_bytes()
    gate, entered = threading.Event(), threading.Event()
    original = server.handler
    def delayed(cmd):
        if cmd == "passthrough portal on":
            entered.set()
            gate.wait(.2)
        return original(cmd)
    server.handler = delayed
    server.commands.clear()
    model.setWanted(True)
    assert entered.wait(.5)
    model.setWanted(False)
    start = time.monotonic()
    model.close()
    assert time.monotonic() - start < .05
    gate.set()
    wait(lambda: not model.busy)
    assert server.commands == [f"passthrough portal {c}" for c in ("on", "status", "off", "status")]
    assert server.value["wanted"] is False
    assert config.read_bytes() == before


@pytest.mark.parametrize("mode", ["quit", "destroyed", "unresponsive", "locked"])
def test_application_exit_completes_accepted_commands_without_qt_delivery(setup, tmp_path, mode):
    # Server lives in the parent so assertions see what survived interpreter exit.
    # The child has a real Qt event loop, private paths, and no manual drain helper.
    model, server, config, _ = setup
    model.close()
    before = config.read_bytes()
    gate = threading.Event()
    original = server.handler
    def delayed(cmd):
        if cmd == "passthrough portal on":
            gate.wait(2)
        reply = original(cmd)
        return None if mode == "unresponsive" else reply
    server.handler = delayed
    server.commands.clear()
    child = textwrap.dedent('''
        import fcntl, sys
        from PySide6.QtCore import QCoreApplication, QObject, QTimer
        import shiboken6
        sys.path.insert(0, sys.argv[1] + "/display-settings")
        from native_portal_settings import PortalSettings, ft_layout
        ft_layout.LAYOUT_PATH, ft_layout.LOCK_PATH, ft_layout.CONF_PATH = sys.argv[3:6]
        app = QCoreApplication([])
        app.aboutToQuit.connect(lambda: print("quitting", flush=True))
        parent = QObject()
        model = PortalSettings(parent, address="\\0" + sys.argv[2], timeout=.3)
        model._timer.stop()
        # Replace only the initial read-only poll with a wait for it to finish.
        def submit():
            if model.busy:
                QTimer.singleShot(2, submit)
                return
            if sys.argv[6] == "locked":
                global lock
                lock = open(ft_layout.LOCK_PATH, "w")
                fcntl.flock(lock, fcntl.LOCK_EX)
                model.placeAtDesk()
            model.setWanted(True)
            model.setWanted(False)
            if sys.argv[6] == "destroyed":
                shiboken6.delete(parent)
            QTimer.singleShot(20, app.quit)
        QTimer.singleShot(0, submit)
        app.exec()
        if sys.argv[6] != "destroyed":
            assert model._closed, "aboutToQuit must stop polling and new submissions"
            model.setWanted(True)
            model.refresh()
    ''')
    process = subprocess.Popen([sys.executable, "-c", child, str(REPO), server.address[1:],
        str(config), str(tmp_path / "lock"), str(tmp_path / "frametop.conf"), mode],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        # Do not release the delayed ACK until Qt actually starts exiting.
        import select
        assert process.stdout is not None
        assert select.select([process.stdout], [], [], 3)[0], "Qt quit was blocked"
        assert process.stdout.readline().strip() == "quitting"
        gate.set()
        start = time.monotonic()
        _out, err = process.communicate(timeout=3)
        assert process.returncode == 0, err
        assert time.monotonic() - start < 2
    finally:
        gate.set()
        if process.poll() is None:
            process.kill()
            process.communicate()
    intents = [c for c in server.commands if c.endswith((" on", " off"))]
    assert intents == ["passthrough portal on", "passthrough portal off"]
    if mode != "unresponsive":
        assert server.commands[-4:] == [f"passthrough portal {c}" for c in ("on", "status", "off", "status")]
    assert server.value["wanted"] is False
    assert config.read_bytes() == before


def test_stale_poll_cannot_overwrite_new_command(setup):
    model, server, *_ = setup
    gate, entered = threading.Event(), threading.Event()
    respond = server.handler
    def delayed(cmd):
        if cmd == "passthrough portal status" and not entered.is_set():
            old = wire(server.value)
            entered.set()
            gate.wait(.5)
            return old
        return respond(cmd)
    server.handler = delayed
    model.refresh()
    assert entered.wait(.5)
    seen = []
    model.changed.connect(lambda: seen.append(dict(model.portal)))
    model.setWanted(True)
    gate.set()
    wait(lambda: not model.busy)
    assert model.portal["wanted"] is True
    assert not any(v["known"] and v["wanted"] is False for v in seen)


def test_bad_reply_never_claims_off_or_camera_stopped(setup):
    model, server, *_ = setup
    model.setWanted(True)
    wait(lambda: not model.busy)
    server.bad = "ok"
    model.setWanted(False)
    wait(lambda: not model.busy)
    assert not model.portal["known"]
    assert model.portal["wanted"] is None
    assert "unknown" in model.portal["summary"].lower()
    assert "camera" not in model.portal or model.portal["camera"] != "off"


@pytest.mark.parametrize("slot,key,command", [("setControls", "passthrough_controls", "controls"),
                                               ("setAboveSteam", "portal_above_steam", "layer")])
def test_preferences_are_independent_exact_readback_and_raw_preserving(setup, slot, key, command):
    model, server, config, raw = setup
    server.commands.clear()
    getattr(model, slot)(True)
    wait(lambda: not model.busy)
    assert server.commands == [f"passthrough {command} 1", f"passthrough {command} state"]
    assert json.loads(config.read_text()) == dict(raw, **{key: True})
    assert model.preferences[command]["known"]
    original = server.handler
    server.handler = lambda cmd: f"ok {command} 1" if cmd.startswith(f"passthrough {command}") else original(cmd)
    getattr(model, slot)(False)
    wait(lambda: not model.busy)
    assert not model.preferences[command]["known"]
    assert "not applied" in model.preferences[command]["summary"]


@pytest.mark.parametrize("slot", ["setControls", "setAboveSteam"])
def test_preference_setters_reject_non_boolean_values(setup, slot):
    model, server, config, _ = setup
    before = config.read_bytes()
    server.commands.clear()
    getattr(model, slot)(1)
    wait(lambda: not model.busy)
    assert config.read_bytes() == before
    assert not server.commands




def test_failed_initial_geometry_query_saves_only_without_overwriting_live_axes(setup):
    model, server, config, raw = setup
    model._timer.stop()
    server.volume.update(pitch=23., roll=19.)
    live_before = json.loads(json.dumps(server.volume))
    original = server.handler
    failed = False
    def fail_first_query(cmd):
        nonlocal failed
        if cmd == "passthrough state3d" and not failed:
            failed = True
            return "error temporarily unavailable"
        return original(cmd)
    server.handler = fail_first_query
    server.commands.clear()
    model.placeAtDesk()
    wait(lambda: not model.busy)
    assert server.volume == live_before
    assert not any(c.startswith("passthrough set ") for c in server.commands)
    assert not model.geometryKnown
    assert "Saved" in model.geometrySummary and "not applied" in model.geometrySummary
    saved = json.loads(config.read_text())
    assert saved == dict(raw, passthrough=model.volume)
    assert saved["passthrough"]["pitch"] == 7.
    assert saved["passthrough"]["roll"] == -4.
    assert ft_layout.passthrough_rectangle_center(saved["passthrough"]) == pytest.approx([.35, 1.25, -2.])
    # A later explicit edit must re-read live state, not apply the saved fallback.
    model.placeAtDesk()
    wait(lambda: not model.busy)
    for key in ("size", "pitch", "roll", "enabled"):
        assert server.volume[key] == live_before[key]
    assert model.geometryKnown


def test_place_at_desk_preserves_size_tilt_and_legacy_enabled(setup):
    model, server, *_ = setup
    model.placeAtDesk()
    wait(lambda: not model.busy)
    assert ft_layout.passthrough_rectangle_center(server.volume) == pytest.approx([.35, 1.25, -2.])
    assert server.volume["yaw"] == 90
    for key in ("pitch", "roll", "size", "enabled"):
        assert server.volume[key] == VOLUME[key]






def test_layout_lock_cannot_block_portal_off_indefinitely(setup):
    import fcntl
    model, _server, config, _ = setup
    before = config.read_bytes()
    with open(ft_layout.LOCK_PATH, "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        model.placeAtDesk()
        model.setWanted(False)
        wait(lambda: not model.busy, timeout=1.5)
        assert model.portal["wanted"] is False
        assert config.read_bytes() == before


def test_gamescope_backend_never_sends_native_commands(setup, monkeypatch):
    model, server, *_ = setup
    server.commands.clear()
    monkeypatch.setattr(ft_layout, "backend", lambda: "gamescope")
    model.refresh()
    model.setWanted(True)
    model.setControls(True)
    model.setAboveSteam(True)
    model.placeAtDesk()
    APP.processEvents()
    assert not server.commands


def test_backend_wires_portal_without_custom_session_dependencies(setup, monkeypatch):
    model, server, *_ = setup
    module = importlib.import_module("ft_display_settings")
    monkeypatch.setattr(module, "FT_SCREENS", server.address)
    monkeypatch.setattr(module, "FT_POWERD", server.address + "-absent")
    monkeypatch.setattr(module.Backend, "_check_running", lambda self: None)
    backend = module.Backend()
    assert isinstance(backend.nativePortal, type(model))
    wait(lambda: not backend.nativePortal.busy)
    backend.poll.stop()
    backend.nativePortal.close()
    backend._sock.close()
    backend._psock.close()


def test_actual_shipped_qml_groups_controls_without_raw_geometry(setup, tmp_path):
    model, server, *_ = setup
    qml_path = REPO / "display-settings/PortalSettings.qml"
    assert qml_path.exists(), "shipped native portal QML is missing"
    engine = QQmlApplicationEngine()
    engine.rootContext().setContextProperty("portalModel", model)
    harness = tmp_path / "Harness.qml"
    harness.write_text('import QtQuick\nimport "' + qml_path.parent.as_uri() + '"\n'
        'Window { width: 900; height: 1500; visible: true; PortalSettings { anchors.fill: parent; model: portalModel } }')
    engine.load(QUrl.fromLocalFile(str(harness)))
    assert engine.rootObjects(), "production QML failed to instantiate"
    root = engine.rootObjects()[0]
    def item(name):
        pending = [root.contentItem()]
        while pending:
            obj = pending.pop()
            if obj.objectName() == name:
                return obj
            pending.extend(obj.childItems())
        raise AssertionError(name)
    APP.processEvents()
    for name in ("passthroughPlace", "passthroughControls", "portalAboveSteam"):
        item(name)
    for name in ("passthroughWidth", "passthroughDepth", "passthroughHeight", "passthroughX", "passthroughY", "passthroughZ", "passthroughYaw", "passthroughPitch", "passthroughRoll"):
        with pytest.raises(AssertionError):
            item(name)
    switch = item("cameraPassthroughEnabled")
    switch.forceActiveFocus()
    QTest.keyClick(root, Qt.Key_Space)
    wait(lambda: not model.busy)
    assert model.portal["wanted"] is True
    QTest.keyClick(root, Qt.Key_Space)
    wait(lambda: not model.busy)
    assert model.portal["wanted"] is False
    assert switch.property("text") == "Desk passthrough"
    assert item("portalAboveSteam").property("text") == "Passthrough above Steam menus"
    root.close()
    engine.deleteLater()
    APP.processEvents()


@pytest.mark.skipif(os.environ.get("NATIVE_PORTAL_KIRIGAMI") != "1", reason="Full Kirigami shell requires Frame dev packages; production portal component runs separately")
def test_full_kirigami_window_opens_native_page_without_activation(setup, monkeypatch):
    _model, server, config, _ = setup
    module = importlib.import_module("ft_display_settings")
    monkeypatch.setattr(module, "FT_SCREENS", server.address)
    monkeypatch.setattr(module, "FT_POWERD", server.address + "-absent")
    monkeypatch.setattr(module.Backend, "_check_running", lambda self: None)
    backend = module.Backend()
    before = config.read_bytes()
    engine = QQmlApplicationEngine()
    engine.rootContext().setContextProperty("backend", backend)
    engine.rootContext().setContextProperty("startPage", "passthrough")
    errors = []
    engine.warnings.connect(lambda warnings: errors.extend(str(w.toString()) for w in warnings))
    engine.load(QUrl.fromLocalFile(str(REPO / "display-settings/main.qml")))
    assert engine.rootObjects(), errors
    wait(lambda: not backend.nativePortal.busy)
    root = engine.rootObjects()[0]
    pending = [root.contentItem()]
    names = set()
    while pending:
        item = pending.pop()
        names.add(item.objectName())
        pending.extend(item.childItems())
    assert "cameraPassthroughEnabled" in names
    assert "passthroughPlace" in names
    assert "passthroughPitch" not in names
    assert config.read_bytes() == before
    assert not any(c.endswith((" on", " off")) for c in server.commands)
    assert not [e for e in errors if "TypeError" in e or "ReferenceError" in e], errors
    root.close()
    backend.poll.stop()
    backend.nativePortal.close()
    backend._sock.close()
    backend._psock.close()
    engine.deleteLater()
    APP.processEvents()






