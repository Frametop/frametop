"""The recorder windows' QML, for their tests: a window's main.qml with the shared pages it uses
(recorder/qml/), and whether it compiles (imports, types, property names) with Kirigami. Compiling
creates nothing, so no backend, window or page runs; it happens in a child process with its own
temp HOME, offscreen and without the QML disk cache, so nothing is written to the real home."""
import glob
import json
import os
import subprocess
import sys
import tempfile

SHARED = os.path.join(os.path.dirname(os.path.abspath(__file__)), "qml")

_COMPILE = r"""
import json, os, sys
from PySide6.QtCore import QUrl
from PySide6.QtGui import QGuiApplication
from PySide6.QtQml import QQmlComponent, QQmlEngine
app = QGuiApplication([])
engine = QQmlEngine()
out = {}
for path in sys.argv[1:]:
    c = QQmlComponent(engine, QUrl.fromLocalFile(path), QQmlComponent.CompilationMode.PreferSynchronous)
    out[path] = [e.toString() for e in c.errors()] if c.isError() else ([] if c.isReady() else ["not ready"])
print(json.dumps(out))
"""


def shared_files():
    """The shared pages and parts in recorder/qml/."""
    return sorted(glob.glob(os.path.join(SHARED, "*.qml")))


def window_text(main_qml):
    """main.qml and the shared files, as one text (for the backend name checks)."""
    texts = []
    for path in [main_qml, *shared_files()]:
        with open(path) as f:
            texts.append(f.read())
    return "\n".join(texts)


def compile_errors(paths, timeout=120):
    """{path: [error, ...]} for each QML file (empty when it compiles). Raises RuntimeError if the
    child couldn't run (no PySide6, no display platform)."""
    with tempfile.TemporaryDirectory(prefix="ft-qmlcheck-") as home:
        env = dict(os.environ, HOME=home, XDG_CONFIG_HOME=os.path.join(home, ".config"),
                   XDG_CACHE_HOME=os.path.join(home, ".cache"), XDG_DATA_HOME=os.path.join(home, ".local", "share"),
                   XDG_STATE_HOME=os.path.join(home, ".local", "state"), XDG_RUNTIME_DIR=home,
                   QT_QPA_PLATFORM="offscreen", QML_DISABLE_DISK_CACHE="1")
        r = subprocess.run([sys.executable, "-c", _COMPILE, *[os.path.abspath(p) for p in paths]],
                           env=env, cwd=home, capture_output=True, text=True, timeout=timeout)
    if r.returncode != 0 or not r.stdout.strip():
        raise RuntimeError((r.stderr or r.stdout).strip()[-2000:])
    return json.loads(r.stdout.strip().splitlines()[-1])
