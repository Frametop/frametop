"""ft_vrsettings: SteamVR's settings, read and written the way Frametop's tools do it.

While SteamVR runs, its settings are asked of and set through vrcmd (SteamVR rewrites its file
from memory, so an edit there would be lost); while it's down, its steamvr.vrsettings file is
read and written directly. The callers own the vrcmd runner and the "is SteamVR up" check (and
their tests replace them), so these take the vrcmd output or the file's path. Standard library
only. Users: gaze/eye-rate.py, pointer/dashpatch/ft-dashpatch.py.
"""
import json
import re
import sys
from pathlib import Path

# The first that exists is SteamVR's (as gaze/gazecal.py reads them).
FILES = (Path.home() / ".config/openvr/config/steamvr.vrsettings",
         Path.home() / ".steam/steam/config/steamvr.vrsettings")


def settings_file(files=FILES):
    """The settings file SteamVR uses: the first of files that exists, else the first."""
    return next((p for p in files if p.exists()), files[0])


def vrcmd_bool(output, section, key):
    """A bool setting from vrcmd --settings-bool's output ("section.key=true"), or None."""
    m = re.search(rf"^{section}\.{key}=(\w+)", output, re.M)
    return None if not m else m.group(1) in ("true", "1")


def vrcmd_int(output, section, key):
    """An int setting from vrcmd --settings-int's output ("section.key=8089"), or None."""
    m = re.search(rf"^{section}\.{key}=(-?\d+)", output, re.M)
    return int(m.group(1)) if m else None


def bool_arg(on):
    """vrcmd --set-settings-bool's value: 1 or 0, since vrcmd reads "true" as 0."""
    return "1" if on else "0"


def read_section(path, section):
    """The file's section ({} without one), as the file has it; None when the file can't be read
    or isn't a JSON object."""
    try:
        return json.loads(Path(path).read_text()).get(section, {})
    except (OSError, ValueError, AttributeError):
        return None


def write_values(path, section, values):
    """Set values ({key: value}) in the file's section, keeping the rest, by a temporary file and
    a rename. A missing file starts empty; one that can't be read is left alone (an error on
    stderr). True when written."""
    path = Path(path)
    try:
        d = json.loads(path.read_text())
    except FileNotFoundError:
        d = {}
    except (OSError, ValueError) as e:
        print(f"can't read {path}: {e}", file=sys.stderr)
        return False
    d.setdefault(section, {}).update(values)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".frametop-tmp")
    tmp.write_text(json.dumps(d, indent=3) + "\n")
    tmp.replace(path)
    return True
