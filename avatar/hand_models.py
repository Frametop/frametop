"""The hand models Frametop Settings offers (Appearance), without Qt: which there are, what to
call them, a model's picture, and what an import said.

ft-handview is the one that knows the models (`ft-handview --models`: the bundled ones, the ones
ft-model-import put in your avatars folder, and the two it builds in code), so the list comes
from it. Each model's picture is one ft-handview draws (`--thumb`), kept in the cache folder
until the model's file changes.
"""
import os
import re

import ftmodel

REPO = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
HANDVIEW = os.path.join(REPO, "screens", "build", "ft-handview")
IMPORTER = os.path.join(REPO, "avatar", "build", "ft-model-import")
THUMB_PX = 192
MAX_NAME = 31   # modelfile.h's kMaxName

# What the app calls the models that come with Frametop (screens/assets/hands/README.md).
TITLES = {
    "asset": "Generic", "babylon": "Babylon", "mrtk3": "Mixed Reality 3", "godot": "Godot XR",
    "mrtk2": "Mixed Reality 2", "robot": "Robot", "capsules": "Tubes",
}
# Shown while ft-handview isn't built (a checkout without screens/build): the models every build has.
FALLBACK = [(name, "built" if name in ("robot", "capsules") else "bundled") for name in TITLES]
STYLES = [("ghost", "Ghost (see-through)"), ("solid", "Solid")]
SOURCES = "Models (*.vrm *.glb *.gltf *.fbx)"


def folder(environ=None):
    """Your models' folder, as modelfile.cpp's Folder() finds it."""
    env = os.environ if environ is None else environ
    if env.get("FT_AVATARS_DIR"):
        return env["FT_AVATARS_DIR"]
    data = env.get("XDG_DATA_HOME") or os.path.join(env.get("HOME", ""), ".local", "share")
    return os.path.join(data, "frametop", "avatars")


def parse_models(text):
    """`ft-handview --models`'s lines as (name, kind) pairs: kind is bundled, own or built."""
    out = []
    for line in text.splitlines():
        name, _, kind = line.partition("\t")
        if name and kind in ("bundled", "own", "built"):
            out.append((name, kind))
    return out


def describe(name, kind, models_folder):
    """A model as the page shows it: its title, where it came from, whether it can be removed."""
    item = {"name": name, "kind": kind, "title": TITLES.get(name, name), "detail": "", "own": kind == "own"}
    if kind == "own":
        try:
            info = ftmodel.read(os.path.join(models_folder, name + ".ftmodel"), hands=False)
        except (OSError, ftmodel.BadModel):
            return item
        item["title"] = info.name or name
        parts = [p for p in ("by " + info.author if info.author else "", info.format.upper() if info.format else "") if p]
        item["detail"] = ", ".join(parts)
    return item


def slug(text):
    """A model's name from any text, as modelfile.cpp's NameFrom makes it: letters and digits,
    lower case, the rest as "-"."""
    name = re.sub(r"[^a-z0-9_]+", "-", "".join(c.lower() if "A" <= c <= "Z" else c for c in text)).strip("-")
    return name[:MAX_NAME].rstrip("-") or "model"


def free_name(wanted, reserved):
    """The name to import under: `wanted`, or with a number after it when a model that isn't yours
    has that name (importing over your own model of the same name replaces it)."""
    name, n = wanted, 1
    while name in reserved:
        n += 1
        tail = f"-{n}"
        name = wanted[:MAX_NAME - len(tail)] + tail
    return name


def parse_import(stdout):
    """ft-model-import's lines as {key: value}: ok, file, name, share, left, right, or error."""
    return dict(line.split(" ", 1) for line in stdout.splitlines() if " " in line)


def import_message(said, source):
    """(text, is an error) for what an import printed."""
    if "ok" not in said:
        return f"Couldn't import {os.path.basename(source)}: {said.get('error', 'the importer stopped')}", True
    notes = [f"{side} hand {said[side]}" for side in ("left", "right") if "mirrored" in said.get(side, "")]
    return f"Imported {said.get('name', said['ok'])}" + (f" ({notes[0]})" if notes else ""), False


def stamp(name, kind, models_folder):
    """What a model's picture depends on: its file for one of yours, else the program that draws it."""
    path = os.path.join(models_folder, name + ".ftmodel") if kind == "own" else HANDVIEW
    try:
        return os.stat(path).st_mtime_ns
    except OSError:
        return 0


def thumb_path(cache, name, kind, models_folder):
    return os.path.join(cache, f"{name}-{stamp(name, kind, models_folder)}.png")


def stale_thumbs(cache, name, keep):
    """A model's older pictures in the cache: every NAME-STAMP.png but `keep`."""
    old = re.compile(re.escape(name) + r"-\d+\.png")
    try:
        return [os.path.join(cache, f) for f in os.listdir(cache)
                if old.fullmatch(f) and os.path.join(cache, f) != keep]
    except OSError:
        return []
