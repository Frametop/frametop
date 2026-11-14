"""ft_desktopapps: GLib's reader of .desktop files, under the name this GLib gives it.

    from ft_desktopapps import DesktopAppInfo
    info = DesktopAppInfo.new("org.kde.konsole.desktop")

GLib 2.80 moved the Unix-only DesktopAppInfo into GioUnix. Gio's old name still works, but
from SteamOS 0.5 (GLib 2.86, PyGObject 3.54) each use prints a PyGIDeprecationWarning, and
a later GLib may drop it. A GLib without GioUnix (an older host) gets Gio's.
"""
import gi


def _desktop_app_info():
    try:
        gi.require_version("GioUnix", "2.0")
        from gi.repository import GioUnix
        return GioUnix.DesktopAppInfo
    # AttributeError: a "gi" that isn't PyGObject (the float tests' stand-in) has no require_version.
    except (ValueError, ImportError, AttributeError):
        from gi.repository import Gio
        return Gio.DesktopAppInfo


DesktopAppInfo = _desktop_app_info()
