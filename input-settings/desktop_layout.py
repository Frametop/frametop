"""The Frametop desktop's keyboard layout, for Input Settings' Keyboard page (standard library only).

Every keyboard typing on the desktop goes through one layout: a USB or Bluetooth keyboard's keys,
Full Keyboard's keys, dictation and host input alike. KWin keeps it in Plasma's kxkbrc in the
desktop's config folder (~/.config/frametop, its XDG_CONFIG_HOME), where the desktop's System
Settings saves it too. The page picks the first layout there (the one KWin starts with), and tells
the running desktop's KWin to load it again (org.kde.keyboard.reloadConfig on /Layouts, the signal
System Settings sends). The input relay and ft-frunner read kxkbrc themselves.

The layouts and their variants come from xkeyboard-config's evdev.xml, the host's first: KWin runs
on the host and builds its keymap from the host's copy."""
import os
import re
import stat
import subprocess
import tempfile
import xml.etree.ElementTree as ET

CONFIG_DIR = os.path.expanduser("~/.config/frametop")
KXKBRC = os.path.join(CONFIG_DIR, "kxkbrc")
RULES_XML = ("/run/host/usr/share/X11/xkb/rules/evdev.xml", "/usr/share/X11/xkb/rules/evdev.xml")
GROUP = "Layout"
LISTS = ("LayoutList", "VariantList", "DisplayNames")  # kept in step, one entry per layout
ENV_NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")  # what env(1) takes as NAME=VALUE, not an option


def read_rules(paths=RULES_XML):
    """{layout: (description, [(variant, description), ...])} from the first evdev.xml that parses;
    {} when there's none."""
    for path in paths:
        try:
            root = ET.parse(path).getroot()
        except (OSError, ET.ParseError):
            continue
        found = {}
        for layout in root.iterfind("layoutList/layout"):
            name = (layout.findtext("configItem/name") or "").strip()
            if not name:
                continue
            variants = [((v.findtext("configItem/name") or "").strip(),
                         (v.findtext("configItem/description") or "").strip())
                        for v in layout.iterfind("variantList/variant")]
            found[name] = ((layout.findtext("configItem/description") or name).strip(),
                           [(n, d or n) for n, d in variants if n])
        if found:
            return found
    return {}


def layout_choices(rules):
    """The page's layout list: [{"value", "text"}], by name, US English first when it's there."""
    items = sorted(({"value": k, "text": d} for k, (d, _) in rules.items()), key=lambda i: i["text"].casefold())
    return sorted(items, key=lambda i: i["value"] != "us")


def variant_choices(rules, layout):
    """A layout's variants for the page: [{"value", "text"}], its default ("") first."""
    desc, variants = rules.get(layout, (layout, []))
    return [{"value": "", "text": desc}] + [{"value": n, "text": d} for n, d in variants]


def _lines(path):
    try:
        with open(path, encoding="utf-8") as f:
            return f.read().splitlines()
    except FileNotFoundError:
        return []


def _group(lines):
    """(start, end) of [Layout]'s entries in lines (start is just after its header); None without one."""
    for i, line in enumerate(lines):
        if line.strip() == f"[{GROUP}]":
            end = next((j for j in range(i + 1, len(lines)) if lines[j].lstrip().startswith("[")), len(lines))
            return i + 1, end
    return None


def _entries(lines):
    span = _group(lines)
    if not span:
        return {}
    found = {}
    for line in lines[span[0]:span[1]]:
        key, sep, value = line.partition("=")
        if sep and not line.lstrip().startswith("#"):
            found[key.strip()] = value.strip()
    return found


def stamp(path=KXKBRC):
    """kxkbrc's (mtime, size), None without it: whether it changed since."""
    try:
        st = os.stat(path)
        return st.st_mtime_ns, st.st_size
    except OSError:
        return None


def current(path=KXKBRC):
    """(layout, variant) KWin starts with: kxkbrc's first, or ("us", "") without a file, without a
    list, or with layouts switched off there (Use=false), as KWin does."""
    entries = _entries(_lines(path))
    layouts = entries.get("LayoutList", "").split(",")
    if entries.get("Use", "true").lower() == "false" or not layouts[0].strip():
        return "us", ""
    variants = entries.get("VariantList", "").split(",")
    return layouts[0].strip(), variants[0].strip() if variants else ""


def put_first(layout, variant="", path=KXKBRC):
    """Make (layout, variant) the desktop's first layout in kxkbrc, in place of the one that was
    first, and turn layouts on (Use=true). The other layouts there stay after it, in their order
    (without a second copy of this one), with their variants and display names, so a list made in
    System Settings keeps its extra layouts and picking again doesn't make it longer. The file's
    other groups and entries stay as they are."""
    lines = _lines(path)
    entries = _entries(lines)
    use_on = entries.get("Use", "true").lower() != "false"
    lists = {k: entries[k].split(",") if k in entries else [] for k in LISTS}
    rows = []  # (layout, variant, display name) already there, in order
    for i in range(len(lists["LayoutList"]) if use_on else 0):
        row = tuple((lists[k][i] if i < len(lists[k]) else "").strip() for k in LISTS)
        if row[0] and row[:2] not in [r[:2] for r in rows]:
            rows.append(row)
    rows = [(layout, variant, "")] + [r for r in rows[1:] if r[:2] != (layout, variant)]
    want = {"LayoutList": ",".join(r[0] for r in rows), "VariantList": ",".join(r[1] for r in rows), "Use": "true"}
    if "DisplayNames" in entries or any(r[2] for r in rows):
        want["DisplayNames"] = ",".join(r[2] for r in rows)

    span = _group(lines)
    if span is None:
        if lines and lines[-1].strip():
            lines.append("")
        lines.append(f"[{GROUP}]")
        span = (len(lines), len(lines))
    start, end = span
    body = lines[start:end]
    for key, value in sorted(want.items()):
        # KConfig reads the last copy of a key: set that one, and drop the copies before it
        found = [i for i, line in enumerate(body) if "=" in line and line.partition("=")[0].strip() == key]
        for i in reversed(found[:-1]):
            del body[i]
        at = found[-1] - len(found[:-1]) if found else None
        if at is None:
            # after the group's last entry, before any blank lines that end it
            k = len(body)
            while k and not body[k - 1].strip():
                k -= 1
            body.insert(k, f"{key}={value}")
        else:
            body[at] = f"{key}={value}"
    lines[start:end] = body

    path = os.path.realpath(path)  # a kxkbrc linked from elsewhere stays a link
    os.makedirs(os.path.dirname(path), exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=".kxkbrc.", dir=os.path.dirname(path))
    try:
        try:
            os.fchmod(fd, stat.S_IMODE(os.stat(path).st_mode))
        except FileNotFoundError:
            os.fchmod(fd, 0o666 & ~_umask())
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def _umask():
    mask = os.umask(0o022)
    os.umask(mask)
    return mask


def desktop_env(proc="/proc", config_dir=CONFIG_DIR, uid=None):
    """The running Frametop desktop's environment, from its Plasma shell (the plasmashell of this
    user whose XDG_CONFIG_HOME is Frametop's config folder; KWin's own environment isn't
    readable, it runs with extra capabilities). {} when the desktop isn't running."""
    uid = os.getuid() if uid is None else uid
    try:
        pids = [p for p in os.listdir(proc) if p.isdigit()]
    except OSError:
        return {}
    for pid in pids:
        base = os.path.join(proc, pid)
        try:
            if os.stat(base).st_uid != uid:
                continue
            with open(os.path.join(base, "comm"), encoding="utf-8", errors="replace") as f:
                if f.read().strip() != "plasmashell":
                    continue
            with open(os.path.join(base, "environ"), "rb") as f:
                raw = f.read()
        except OSError:
            continue
        env = dict(item.split("=", 1) for item in raw.decode("utf-8", "replace").split("\0") if "=" in item)
        if os.path.normpath(env.get("XDG_CONFIG_HOME", "")) == os.path.normpath(config_dir):
            return env
    return {}


def settings_command(env):
    """System Settings' keyboard page in the running desktop, with the desktop's own environment
    (its Wayland socket, bus and config folder), as its menu would start it."""
    return (["env", "-i"] + [f"{k}={v}" for k, v in sorted(env.items()) if ENV_NAME.fullmatch(k)]
            + ["systemsettings", "kcm_keyboard"])


def reload_command(bus):
    """The command that tells KWin on that bus to load kxkbrc again."""
    return ["gdbus", "emit", "--address", bus, "--object-path", "/Layouts",
            "--signal", "org.kde.keyboard.reloadConfig"]


def reload(env=None, run=subprocess.run):
    """Tell the running desktop's KWin to load its keyboard layout again. False when the desktop
    isn't running (it reads kxkbrc when it starts) or the signal didn't go."""
    bus = (desktop_env() if env is None else env).get("DBUS_SESSION_BUS_ADDRESS")
    if not bus:
        return False
    try:
        return run(reload_command(bus), capture_output=True, timeout=2).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False
