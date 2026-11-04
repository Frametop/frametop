#!/usr/bin/env python3
"""Frametop: one menu for Frametop's apps and commands, and Update Frametop
(docs/reference-apps.md, The Frametop menu).

SteamVR's Launch a program list is one flat list, so the entries of Frametop's apps leave it
(NotShowIn=gamescope: Steam runs there, and the Frametop desktop is KDE, so its menus keep
them) and this one entry takes their place. It lists the menu entries the installers put in
~/.local/share/applications that say which submenu they go in (X-Frametop-Menu=GROUP) and the
profiles' entries (X-Frametop-Profile, ft-layout launchers), so an app that isn't installed
isn't listed, and one an installer adds shows up without a change here. kdialog shows the
menus: the host's, as Konsole is, so it opens from Steam's list and in the desktop.

  ft-menu                  the menu
  ft-menu update           Update Frametop: the update command in Konsole (README.md, Update)
  ft-menu list             the menu as text, one submenu a block
  ft-menu update-command   the update command for this tree

Picked from Steam's list, it runs in Steam's session (XDG_CURRENT_DESKTOP=gamescope), so an
app it opens is handed to ft-floatd the way float/ft-dashroute hands one over ("dashlaunch"),
which floats it in the desktop when Apps from the SteamVR dashboard is Frametop's, and
answers "steamvr REASON" otherwise; then it starts here, as Steam would have started it.
FT_MENU_APPS_DIR moves the menu entries' folder, for tests.
"""
import configparser
import os
import re
import shlex
import socket
import subprocess
import sys

HERE = os.path.dirname(os.path.realpath(__file__))
REPO = os.path.dirname(HERE)
GROUP = "Desktop Entry"
# The submenus, in their order: (X-Frametop-Menu value, label).
GROUPS = [("settings", "Settings"), ("screens", "Screens"), ("profiles", "Profiles"),
          ("help", "Help"), ("tools", "Recorders and tools")]
GET_SH = "https://frametop.github.io/frametop/get.sh"
FLOAT = "\0" + os.environ.get("FT_FLOAT_SOCKET", "frametop_float")
TITLE = "Frametop"


def apps_dir():
    return os.environ.get("FT_MENU_APPS_DIR") or os.path.expanduser("~/.local/share/applications")


def read_entry(path):
    """The [Desktop Entry] group of a desktop file as a dict, or None."""
    kf = configparser.ConfigParser(interpolation=None, strict=False, delimiters=("=",))
    kf.optionxform = str
    try:
        kf.read(path, encoding="utf-8")
    except (configparser.Error, OSError, UnicodeDecodeError):
        return None
    return dict(kf[GROUP]) if kf.has_section(GROUP) else None


def label(name, group):
    """What the submenu shows: the name without "Frametop " ("Frametop: " for a profile)."""
    for prefix in ("Frametop: ", "Frametop "):
        if name.startswith(prefix) and len(name) > len(prefix):
            return name[len(prefix):]
    return name


def entries(folder=None):
    """{group: [(label, desktop file path)]}: the installed entries for each submenu, by label."""
    folder = folder or apps_dir()
    out = {}
    try:
        names = sorted(os.listdir(folder))
    except OSError:
        names = []
    known = {g for g, _ in GROUPS}
    for name in names:
        if not name.endswith(".desktop"):
            continue
        path = os.path.join(folder, name)
        e = read_entry(path)
        if not e or e.get("Type") != "Application" or not e.get("Exec"):
            continue
        if e.get("Hidden", "").lower() == "true":
            continue
        group = "profiles" if e.get("X-Frametop-Profile", "").lower() == "true" else e.get("X-Frametop-Menu")
        if group not in known:
            continue
        out.setdefault(group, []).append((label(e.get("Name") or name[:-8], group), path))
    for items in out.values():
        items.sort(key=lambda item: item[0].lower())
    return out


def update_command(tree=REPO, home=None):
    """The update command (README.md, Update) for the tree this menu runs from: a release
    (.frametop-release) updates with get.sh --release, a clone with get.sh, each with --dir
    when it isn't in the default place."""
    home = home or os.path.expanduser("~")
    tree = os.path.realpath(tree)
    args = []
    if os.path.isfile(os.path.join(tree, ".frametop-release")):
        args.append("--release")
        releases = os.path.dirname(tree)
        if releases != os.path.realpath(os.path.join(home, ".local/share/frametop/releases")):
            args += ["--dir", releases]
    elif tree != os.path.realpath(os.path.join(home, "frametop")):
        args += ["--dir", tree]
    cmd = f"curl -fsSL {GET_SH} | bash"
    return cmd + (" -s -- " + " ".join(shlex.quote(a) for a in args) if args else "")


def update_argv(tree=REPO):
    """Konsole running the update command, staying open until Enter so its last lines can be read."""
    cmd = update_command(tree)
    # pipefail: a download that fails is the command's failure, not an empty script that passes.
    script = (f"set -o pipefail; echo {shlex.quote('Updating Frametop: ' + cmd)}; echo; "
              f"{cmd}; status=$?; echo; "
              "read -r -p 'Press Enter to close this window. ' _; exit $status")
    return ["konsole", "-p", "tabtitle=Update Frametop", "-e", "bash", "-c", script]


def commandline(path):
    """The entry's Exec line without its field codes, as Steam hands a launch to ft-floatd."""
    e = read_entry(path) or {}
    return re.sub(r"\s*%[a-zA-Z]", "", e.get("Exec", "")).strip()


def ask_floatd(cmdline):
    """ft-floatd's answer to "dashlaunch CMDLINE" (float/ft-dashroute's ask): "ok" or "steamvr REASON"."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    try:
        s.bind("")
        s.settimeout(5)
        s.sendto(f"dashlaunch {cmdline}".encode(), FLOAT)
        return s.recv(4096).decode(errors="replace")
    except OSError as e:
        return f"steamvr ft-floatd didn't answer ({e})"
    finally:
        s.close()


def from_steam():
    return "gamescope" in os.environ.get("XDG_CURRENT_DESKTOP", "").lower().split(":")


def launch(path, ask=ask_floatd, start=None):
    """Open a menu entry: handed to ft-floatd when the menu came from Steam's list, else here."""
    if from_steam():
        cmd = commandline(path)
        if cmd and ask(cmd) == "ok":
            return "floated"
    (start or start_entry)(path)
    return "started"


def start_entry(path):
    import gi
    gi.require_version("Gio", "2.0")
    from gi.repository import Gio
    info = Gio.DesktopAppInfo.new_from_filename(path)
    if info is None or not info.launch([], None):
        raise RuntimeError(f"couldn't start {path}")


def start_detached(argv):
    subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                     start_new_session=True)


def choose(prompt, items):
    """kdialog's list: the tag picked, or None (Cancel, or the window closed)."""
    argv = ["kdialog", "--title", TITLE, "--menu", prompt]
    for tag, text in items:
        argv += [tag, text]
    r = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    return (r.stdout.strip() or None) if r.returncode == 0 else None


def top_items(groups):
    items = [("group:" + g, text + " ›") for g, text in GROUPS if groups.get(g)]
    return items + [("update", "Update Frametop…")]


def menu(choose=choose, launch=launch, update=None):
    """The menus, until something is opened or the menu is closed. Returns what it did."""
    groups = entries()
    titles = dict(GROUPS)
    while True:
        tag = choose("Frametop", top_items(groups))
        if tag is None:
            return "closed"
        if tag == "update":
            (update or (lambda: start_detached(update_argv())))()
            return "update"
        group = tag.split(":", 1)[1]
        paths = dict((f"app:{i}", path) for i, (_, path) in enumerate(groups.get(group, [])))
        items = [(f"app:{i}", text) for i, (text, _) in enumerate(groups.get(group, []))]
        tag = choose(titles.get(group, group), items + [("back", "‹ Back")])
        if tag is None:
            return "closed"
        if tag in paths:
            launch(paths[tag])
            return "opened " + os.path.basename(paths[tag])


def listing():
    groups = entries()
    lines = []
    for g, text in GROUPS:
        if groups.get(g):
            lines.append(text)
            lines += [f"  {name}  ({os.path.basename(path)})" for name, path in groups[g]]
    lines.append("Update Frametop: " + update_command())
    return "\n".join(lines)


def main(args):
    if not args:
        menu()
    elif args == ["update"]:
        start_detached(update_argv())
    elif args == ["list"]:
        print(listing())
    elif args == ["update-command"]:
        print(update_command())
    else:
        sys.exit("usage: ft-menu [update|list|update-command]")


if __name__ == "__main__":
    main(sys.argv[1:])
