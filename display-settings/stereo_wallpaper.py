"""Stereo wallpapers (DEE-317): a picture per eye behind the windows on the Frametop screens.

KWin draws the wallpaper and the windows into one buffer per screen, so ft-screens can't swap
the wallpaper on its own. Instead Plasma's wallpaper is one solid key colour (KEY, magenta), and
ft-screens' hand cutter (screens/handrender.cpp, screens/wallimage.h) puts each eye's picture
where it sees that colour, or that colour darkened by a window's shadow.

This module turns the pictures someone picks (a left and a right, or one side-by-side picture)
into the binary PPMs ft-screens reads, ~/.local/share/frametop/wallpaper/left.ppm and
right.ppm, and makes the Plasma scripts that set the key colour and put the old wallpaper back
(saved in plasma-before.json next to them).

Display Settings writes the pictures itself (in the dev container: prepare() needs PySide6)
and runs the rest on the host, where the Frametop desktop's bus is (system Python, no Qt):

  stereo_wallpaper.py on       ft-screens shows the pictures, then Plasma gets the key colour,
                               then STEREO_WALLPAPER=1 (frametop.conf)
  stereo_wallpaper.py off      Plasma gets its wallpaper back, then ft-screens stops, then
                               STEREO_WALLPAPER=0
  stereo_wallpaper.py reload   ft-screens reads the pictures again (with it on)
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "layout"))
import ft_layout  # noqa: E402  (frametop.conf, the Frametop desktop's environment; puts conf/ on the path)
import ft_store  # noqa: E402
import ftipc  # noqa: E402

FT_SCREENS = "\0ft_screens"

KEY = (255, 0, 255)
KEY_TEXT = ",".join(map(str, KEY))
KEY_HEX = "#%02x%02x%02x" % KEY   # how Plasma's scripting may read the colour back (a QColor)
# Each eye's picture at most this wide or tall (ft-screens takes up to 4096: wallimage::kMaxSide);
# a Frametop screen is at most 2560 pixels wide.
MAX_SIDE = 2560


def folder():
    return os.path.expanduser("~/.local/share/frametop/wallpaper")


def paths():
    d = folder()
    return os.path.join(d, "left.ppm"), os.path.join(d, "right.ppm")


def saved_path():
    return os.path.join(folder(), "plasma-before.json")


def ready():
    """Both eyes' pictures are there."""
    return all(os.path.isfile(p) for p in paths())


def ppm(image):
    """A QImage -> binary PPM bytes (P6, 8 bits), scaled down to MAX_SIDE."""
    from PySide6.QtCore import QSize, Qt
    from PySide6.QtGui import QImage
    if image.width() > MAX_SIDE or image.height() > MAX_SIDE:
        image = image.scaled(QSize(MAX_SIDE, MAX_SIDE), Qt.KeepAspectRatio, Qt.SmoothTransformation)
    image = image.convertToFormat(QImage.Format_RGB888)
    w, h, line = image.width(), image.height(), image.bytesPerLine()
    data = bytes(image.constBits())
    rows = b"".join(data[y * line:y * line + w * 3] for y in range(h))   # rows are padded to 4 bytes
    return b"P6\n%d %d\n255\n" % (w, h) + rows


def split(image):
    """One side-by-side picture (left eye on the left) -> (left, right)."""
    half = image.width() // 2
    return image.copy(0, 0, half, image.height()), image.copy(half, 0, half, image.height())


def prepare(left, right=None):
    """Write the eyes' PPMs from picture files: `left` and `right`, or `left` alone as one
    side-by-side picture. The right eye's is scaled to the left's size. Returns "" or what went
    wrong (nothing is written then)."""
    from PySide6.QtCore import Qt
    from PySide6.QtGui import QImage
    a = QImage(left)
    if a.isNull():
        return f"Couldn't read {os.path.basename(left)} as a picture."
    if right:
        b = QImage(right)
        if b.isNull():
            return f"Couldn't read {os.path.basename(right)} as a picture."
    else:
        if a.width() < 2:
            return "That picture is too small to split into two eyes."
        a, b = split(a)
    if b.size() != a.size():
        b = b.scaled(a.size(), Qt.IgnoreAspectRatio, Qt.SmoothTransformation)
    os.makedirs(folder(), exist_ok=True)
    lp, rp = paths()
    ft_store.write_bytes(lp, ppm(a))
    ft_store.write_bytes(rp, ppm(b))
    return ""


# Plasma's desktop scripting (org.kde.PlasmaShell.evaluateScript), run in the Frametop desktop.
# Each desktop's wallpaper: plugin, the image plugin's Image, the colour plugin's Color.
_READ = """function was(d) {
    d.currentConfigGroup = ["Wallpaper", "org.kde.image", "General"];
    var image = d.readConfig("Image", "");
    d.currentConfigGroup = ["Wallpaper", "org.kde.color", "General"];
    var color = String(d.readConfig("Color", ""));
    return {plugin: d.wallpaperPlugin, image: String(image), color: color};
}
"""


def on_script():
    """Sets every desktop's wallpaper to the key colour, and prints what each had before as
    JSON ({desktop id: {plugin, image, color}})."""
    return _READ + """var before = {};
desktops().forEach(function (d) {
    before[d.id] = was(d);
    d.wallpaperPlugin = "org.kde.color";
    d.currentConfigGroup = ["Wallpaper", "org.kde.color", "General"];
    d.writeConfig("Color", "%s");
});
print(JSON.stringify(before));
""" % KEY_TEXT


def off_script(before):
    """Puts each desktop's wallpaper back as `before` had it; a desktop it doesn't know that
    still has the key colour gets Plasma's default picture."""
    return _READ + """var before = %s;
desktops().forEach(function (d) {
    var b = before[d.id];
    if (!b) {
        var now = was(d);
        var c = now.color.replace(/ /g, "").toLowerCase();
        if (now.plugin != "org.kde.color" || (c != "%s" && c != "%s")) return;
        b = {plugin: "org.kde.image", image: "", color: ""};
    }
    d.wallpaperPlugin = b.plugin;
    if (b.plugin == "org.kde.image" && b.image) {
        d.currentConfigGroup = ["Wallpaper", "org.kde.image", "General"];
        d.writeConfig("Image", b.image);
    }
    if (b.plugin == "org.kde.color" && b.color) {
        d.currentConfigGroup = ["Wallpaper", "org.kde.color", "General"];
        d.writeConfig("Color", b.color);
    }
});
print("ok");
""" % (json.dumps(before), KEY_TEXT, KEY_HEX)


def is_key(entry):
    color = str(entry.get("color", "")).replace(" ", "").lower()
    return entry.get("plugin") == "org.kde.color" and color in (KEY_TEXT, KEY_HEX)


def remember(printed):
    """Keep what on_script printed: each desktop's wallpaper before the key colour. A desktop
    that already had the key colour (turned on twice) keeps what was saved for it before.
    Returns False if the output isn't on_script's."""
    try:
        now = json.loads(printed)
    except ValueError:
        return False
    if not isinstance(now, dict):
        return False
    old = ft_store.load_json(saved_path(), default={})
    old = old if isinstance(old, dict) else {}
    for k, v in now.items():
        if isinstance(v, dict) and not is_key(v):
            old[str(k)] = v
    os.makedirs(folder(), exist_ok=True)
    ft_store.write_json(saved_path(), old)
    return True


def saved():
    data = ft_store.load_json(saved_path(), default={})
    return data if isinstance(data, dict) else {}


def plasma(js):
    """Runs a script in the Frametop desktop's Plasma: (ok, what it printed or why not)."""
    env = ft_layout.nested_env()
    if not env:
        return False, "the Frametop desktop isn't running"
    try:
        r = subprocess.run(["qdbus6", "org.kde.plasmashell", "/PlasmaShell", "org.kde.PlasmaShell.evaluateScript", js],
                           capture_output=True, text=True, env=env, timeout=10)
    except (OSError, subprocess.TimeoutExpired) as e:
        return False, str(e)
    if r.returncode != 0:
        return False, (r.stderr or r.stdout).strip() or f"qdbus6 exit {r.returncode}"
    return True, r.stdout.strip()


def ask_screens(text):
    return ftipc.ask(FT_SCREENS, text, timeout=2.0) or ""


def turn_on():
    if not ready():
        return "Pick the pictures first."
    reply = ask_screens("wallpaper on")
    if not reply.startswith("ok"):
        return f"ft-screens didn't take the pictures: {reply or 'no answer (is the desktop running?)'}"
    ok, out = plasma(on_script())
    if ok and remember(out):
        ft_layout.ft_conf.set_value("STEREO_WALLPAPER", "1", ft_layout.CONF_PATH)
        return ""
    # The script may have keyed some desktops before it failed (or timed out, or printed something
    # else): put back what's known, then stop ft-screens. If that fails too, ft-screens keeps going
    # and the switch shows on, so the key colour never shows bare and turning it off tries again.
    back, why = plasma(off_script(saved()))
    if not back:
        ft_layout.ft_conf.set_value("STEREO_WALLPAPER", "1", ft_layout.CONF_PATH)
        return (f"Plasma didn't take the key colour ({out}), and putting the wallpaper back failed ({why}): "
                "the stereo wallpaper stays on; turn it off to try again.")
    ask_screens("wallpaper off")
    return f"Plasma didn't take the key colour: {out}"


def turn_off():
    ok, out = plasma(off_script(saved()))
    if not ok:
        return f"Plasma didn't put the wallpaper back (the stereo wallpaper stays on): {out}"
    ask_screens("wallpaper off")
    ft_layout.ft_conf.set_value("STEREO_WALLPAPER", "0", ft_layout.CONF_PATH)
    return ""


def reload():
    if ft_layout.read_conf().get("STEREO_WALLPAPER", "0") != "1":
        return ""
    reply = ask_screens("wallpaper on")
    return "" if reply.startswith("ok") else f"ft-screens didn't take the pictures: {reply or 'no answer'}"


def main(argv):
    verbs = {"on": turn_on, "off": turn_off, "reload": reload}
    if len(argv) != 1 or argv[0] not in verbs:
        print("usage: stereo_wallpaper.py on|off|reload", file=sys.stderr)
        return 2
    err = verbs[argv[0]]()
    print(err or "ok")
    return 1 if err else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
