"""fthost: commands on the SteamOS host, from the dev container the apps run in.

host_command(*cmd) is the argv for one. distrobox-host-exec reaches the host through the user's
real session bus; inside the desktop our DBUS_SESSION_BUS_ADDRESS is the nested session's private
one, where it fails (exit 127, silently). It runs from the home folder: host-spawn starts the
command in the caller's folder, and a container-only one doesn't exist on the host (every command
then exits 127). Outside a container, or without distrobox-host-exec, it's the command itself.

recorder/common.py has the same function: the standalone Hand Recorder ships recorder/ without
conf/. Standard library only.
"""

import os
import shutil


def in_container():
    return os.path.exists("/run/.containerenv") or os.path.exists("/.dockerenv")


def host_command(*cmd):
    exe = shutil.which("distrobox-host-exec")
    if not in_container() or not exe:
        return list(cmd)
    return ["env", "-C", os.path.expanduser("~"), f"DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/{os.getuid()}/bus",
            exe] + list(cmd)
