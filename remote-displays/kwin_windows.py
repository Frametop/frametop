"""Remote Displays' "Add windows": a KDE Plasma PC's open windows from utrack's KWin Window
Stream (issue 81; layout/ft_kwinwindows.py, docs/remote-displays.md, "A KDE PC's windows").

The backend (ft_remote_displays.py) makes one and shows it to the page as kwinWindows. The list
is read on a thread (ft-stream kwin-windows, with one of the host's displays' clients: the PC's
service knows clients by its Sunshine's pairings), and adding one runs ft-layout remote kwin-add
in the backend's queue, like every other change to the streams.
"""
import threading

from PySide6.QtCore import QObject, Qt, Signal, Slot

# layout/ and conf/, put on the path by the backend
import ft_hostapi
import ft_kwinwindows
import ft_layout


def offered(host, windows):
    """The host's windows as the dialog lists them, with `added` for the ones with a panel.
    The PC says what's in them, so each is checked: a bad one is left out."""
    added = {d.get("app") for d in host.get("displays", [])}
    out = []
    for w in windows:
        try:
            app = ft_kwinwindows.app_of(w.get("id", ""))
            width, height = int(w.get("width", 0)), int(w.get("height", 0))
        except (ValueError, TypeError):
            continue
        out.append({"id": app[5:], "title": ft_kwinwindows.printable(w.get("title", "")) or "Window",
                    "app": ft_kwinwindows.printable(w.get("app", ""), 60), "width": width, "height": height,
                    "minimized": bool(w.get("minimized")), "added": app in added})
    return out


class KwinWindows(QObject):
    # a host's name, its windows ([{id, title, app, width, height, minimized, added}]), the
    # client the PC took (what adding one connects with), what went wrong ("" nothing)
    windowsReady = Signal(str, "QVariantList", str, str)
    _listed = Signal(str, "QVariantList", str, str)

    def __init__(self, run, parent=None):
        """run(label, *ft_layout_args): the backend's queue of ft-layout runs."""
        super().__init__(parent)
        self._run = run
        self._cursors = {}  # host name -> find_client's cursor: which client to try first
        self._listed.connect(self.windowsReady, Qt.QueuedConnection)

    @staticmethod
    def available(host):
        """Whether to offer its windows: its Web UI certificate is pinned here (the service shows
        the same one), and one of its displays has a client to connect with."""
        address = host.get("address", "")
        pinned = bool(address) and bool(ft_hostapi.first_word(ft_hostapi.pin_path(address)))
        return pinned and bool(ft_kwinwindows.host_clients(host))

    @Slot(str)
    def listWindows(self, name):
        """The host's open windows (answer: windowsReady)."""
        host = next((h for h in ft_layout.load_layout().get("hosts", []) if h.get("name") == name), None)
        if host is None:
            return self._listed.emit(name, [], "", f"No host called {name}")
        cursor = self._cursors.setdefault(name, {})

        def work():
            try:
                client, windows = ft_kwinwindows.find_client(host, timeout=20, cursor=cursor)
            except RuntimeError as e:
                return self._listed.emit(name, [], "", str(e))
            self._listed.emit(name, offered(host, windows), client, "")
        threading.Thread(target=work, daemon=True).start()

    @Slot(str, str, str, str, int, int)
    def addWindow(self, host, window_id, title, client, width, height):
        """A panel for the window, started in front of you (ft-layout remote kwin-add)."""
        label = ft_kwinwindows.printable(title, 60) or "Window"
        args = ["remote", "kwin-add", host, window_id, "--label", label]
        if width >= 64 and height >= 64:
            args += ["--size", f"{width}x{height}"]
        if client:
            args += ["--client", client]
        self._run(f"Adding {label} from {host}", *args)

