#!/usr/bin/env python3
"""Frametop Settings: every Frametop setting in one app (docs/reference-apps.md, Frametop Settings).

A Kirigami (QML) app with Python backends. It runs in the dev container. A sidebar lists the
sections by group, with a search field; each section is one page from one area:
  - input: Devices, Buttons, Controllers, Keyboard, Commands, Pointer, Ignored panels,
    Bluetooth, Gaze, Hands, Voice and Game optimization (input-settings/: ft_input_settings.py
    and InputPages.qml)
  - display: Screens, Layout, Visibility & pins and Power (display-settings/:
    ft_display_settings.py and DisplayPages.qml)
  - remote: Remote displays (remote-displays/: ft_remote_displays.py and RemoteDisplayPages.qml)
  - access: Remote access over VNC (remote/: remote_access.py and RemoteAccessPages.qml)
An area's backend and pages are made the first time one of its sections opens, so a section
nobody opens costs nothing (its backend's polling never starts).

  ft-settings [SECTION]    open on that section (or FT_SETTINGS_PAGE=SECTION); sections.py has the names

Launch with settings/ft-settings (host wrapper).
"""
import importlib
import os
import sys

from PySide6.QtCore import Property, QObject, QProcess, QUrl, Signal, Slot
from PySide6.QtGui import QGuiApplication, QIcon
from PySide6.QtQml import QJSEngine, QQmlApplicationEngine
from PySide6.QtQuickControls2 import QQuickStyle

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
TOUR = os.path.join(REPO, "tour", "ft_tour.py")
sys.path.insert(0, HERE)
from sections import AREAS, NAMES, SECTIONS, start_section  # noqa: E402,F401  (the sections, without Qt)

def make_backend(area):
    folder, module, _ = AREAS[area]
    path = os.path.join(REPO, folder)
    if path not in sys.path:
        sys.path.insert(0, path)
    return importlib.import_module(module).Backend()


def display_kind():
    """ft_layout.backend(): "screens" (ft-screens) or "gamescope"; Visibility & pins is ft-screens' only."""
    path = os.path.join(REPO, "layout")
    if path not in sys.path:
        sys.path.insert(0, path)
    import ft_layout
    return ft_layout.backend()


class Settings(QObject):
    """The app itself ("settings" in main.qml): the sections, the areas' backends, the help.

    backends: {area: backend} already made, for tests; areas: the areas there are (default: all
    of them, or just those in backends when it's given)."""
    message = Signal(str, bool)

    def __init__(self, start=None, backends=None, areas=None, kind=None):
        super().__init__()
        self._backends = {}
        for area, b in (backends or {}).items():
            self._adopt(area, b)
        self._areas = list(areas or (self._backends if backends else AREAS))
        self._kind = kind
        self._start = start or NAMES[0]

    def _adopt(self, area, b):
        # Ours, not QML's: an object a slot returns without a parent is the JavaScript engine's
        # to collect otherwise.
        QJSEngine.setObjectOwnership(b, QJSEngine.ObjectOwnership.CppOwnership)
        self._backends[area] = b
        return b

    def kind(self):
        if self._kind is None:
            b = self._backends.get("display")
            self._kind = b.backend if b is not None else display_kind()
        return self._kind

    @Property("QVariantList", constant=True)
    def sections(self):
        shown = []
        for name, area, group, title, icon, words in SECTIONS:
            if area not in self._areas or (name == "visibility" and self.kind() != "screens"):
                continue
            shown.append({"name": name, "area": area, "group": group, "title": title, "icon": icon,
                          "keywords": words})
        return shown

    @Property("QVariantMap", constant=True)
    def areaPages(self):
        """Each area's pages file, as a URL main.qml can load."""
        return {a: QUrl.fromLocalFile(os.path.join(REPO, AREAS[a][0], AREAS[a][2])).toString() for a in self._areas}

    @Property(str, constant=True)
    def startSection(self):
        return self._start if any(s["name"] == self._start for s in self.sections) else self.sections[0]["name"]

    @Slot(str, result=QObject)
    def backend(self, area):
        """The area's backend, made the first time it's asked for."""
        if area not in self._backends:
            if area not in self._areas:
                return None
            self._adopt(area, make_backend(area))
        return self._backends[area]

    @Slot()
    def openTour(self):
        """Frametop Tour (tour/ft_tour.py), in this container as it runs."""
        if not QProcess.startDetached(sys.executable, [TOUR])[0]:
            self.message.emit("Couldn't start Frametop Tour", True)

    @Slot()
    def openControlsGuide(self):
        """The tour's controls page, without changing this app's environment."""
        if not QProcess.startDetached("env", ["FT_TOUR_PAGE=recenter", sys.executable, TOUR])[0]:
            self.message.emit("Couldn't start the controls guide", True)


def load(engine, settings):
    """Load the window into engine with settings as its "settings"; False when it didn't load."""
    engine.rootContext().setContextProperty("settings", settings)
    engine.load(QUrl.fromLocalFile(os.path.join(HERE, "main.qml")))
    return bool(engine.rootObjects())


def main():
    app = QGuiApplication(sys.argv)
    app.setApplicationName("ft-settings")
    app.setApplicationDisplayName("Frametop Settings")
    app.setDesktopFileName("ft-settings")
    if not QIcon.themeName():
        QIcon.setThemeName("breeze")
    QQuickStyle.setStyle("org.kde.desktop")
    engine = QQmlApplicationEngine()
    settings = Settings(start=start_section())
    if not load(engine, settings):
        sys.exit(1)
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
