"""The avatars Frametop Settings offers (Appearance), without Qt: the models in your folder that
are whole avatars (a model file with a body: ft-model-import writes one for a VRM, or a glTF or
FBX with a humanoid skeleton), and the mirror that shows you the one you wear.

ft-avatars draws both: each avatar's picture (`--thumb`), kept in the cache folder until its
file changes, and the mirror (`--mirror`), which runs while the mirror is wanted and listens on
@ft_avatars.
"""
import os

import ftmodel
import hand_models

AVATARS = os.path.join(hand_models.REPO, "screens", "build", "ft-avatars")
SOCKET = "ft_avatars"
THUMB_PX = 256


def path(models_folder, name):
    return os.path.join(models_folder, name + ".ftmodel")


def listed(models_folder):
    """Your avatars, by name: a dict each (name, title, detail, own: it can be removed). A file that can't be read, or
    has hands and no body, isn't one. Reads every model file in the folder: not for a UI's thread."""
    out = []
    try:
        files = sorted(f for f in os.listdir(models_folder) if f.endswith(".ftmodel"))
    except OSError:
        return out
    for f in files:
        name = f[:-len(".ftmodel")]
        try:
            info = ftmodel.read(os.path.join(models_folder, f), hands=False)
        except (OSError, ftmodel.BadModel):
            continue
        if not info.has_body:
            continue
        parts = [p for p in ("by " + info.author if info.author else "", info.format.upper() if info.format else "") if p]
        out.append({"name": name, "title": info.name or name, "detail": ", ".join(parts), "own": True})
    return out


def mirror_command(models_folder, name):
    """The program that stands the mirror in the room with this avatar in it."""
    return [AVATARS, "--mirror", path(models_folder, name)]


def thumb_command(models_folder, name, out):
    return [AVATARS, "--thumb", out, path(models_folder, name), "--size", str(THUMB_PX)]
