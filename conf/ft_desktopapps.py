"""ft_desktopapps: GLib's reader of .desktop files, under the name this GLib gives it.

    from ft_desktopapps import DesktopAppInfo
    info = DesktopAppInfo.new("org.kde.konsole.desktop")

GLib 2.80 moved the Unix-only DesktopAppInfo into GioUnix. Gio's old name still works, but
from SteamOS 0.5 (GLib 2.86, PyGObject 3.54) each use prints a PyGIDeprecationWarning, and
a later GLib may drop it. A GLib without GioUnix (an older host) gets Gio's.

So does an older PyGObject. SteamOS 0.4 (GLib 2.84, PyGObject 3.48) has GioUnix, but its
PyGObject binds none of GioUnix's methods to the object: info.get_filename() raises "takes
exactly 1 argument (0 given)". Loading GioUnix there breaks every DesktopAppInfo in the
process, Gio's too, so on that PyGObject this module must not load it at all.
"""
import gi

# The oldest PyGObject known to bind GioUnix's methods (SteamOS 0.5). 3.48 (SteamOS 0.4) does not;
# the versions between are untested, and Gio's name works on them.
GIOUNIX_FROM = (3, 54)


def _desktop_app_info():
    try:
        if gi.version_info >= GIOUNIX_FROM:
            gi.require_version("GioUnix", "2.0")
            from gi.repository import GioUnix
            return GioUnix.DesktopAppInfo
    # AttributeError: a "gi" that isn't PyGObject (the float tests' stand-in) has no version_info.
    except (ValueError, ImportError, AttributeError):
        pass
    from gi.repository import Gio
    return Gio.DesktopAppInfo


DesktopAppInfo = _desktop_app_info()
