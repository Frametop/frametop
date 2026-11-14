"""The avatar this headset's person has on, as the Together page needs it, without Qt: which one
frametop.conf names (AVATAR_MODEL, set on the Appearance page), what its file says of it, and
the command that marks it as one the others may have (ft-model-import --allow-share).

What the facts mean for the page is roompage.avatar's. The model's file is read again only when
it changed or another is worn. Reading one reads a whole model file: not for a UI's thread.
"""
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
for part in ("conf", "avatar"):
    sys.path.insert(0, os.path.join(REPO, part))
import avatar_models  # noqa: E402  (where a model's file is)
import ft_conf  # noqa: E402  (frametop.conf's grammar)
import ftmodel  # noqa: E402  (a model file's facts)
import hand_models  # noqa: E402  (the folder, the importer)

NAME = re.compile(r"[a-z0-9_-]{1,31}")   # modelfile.h's NameOk


def _stamp(path):
    try:
        st = os.stat(path)
        return st.st_mtime_ns, st.st_size, st.st_ino
    except OSError:
        return None


class Worn:
    """The avatar its person has on, and what its file says."""

    def __init__(self, conf=None, folder=None):
        self._conf = conf or ft_conf.PATH
        self._folder = folder or hand_models.folder()
        self._seen, self._facts = None, None

    def look(self):
        """{"name", "title", "path", "body", "may_share", "vrm"}, or None when no avatar is on.
        body is False for a file that isn't there, can't be read or has hands only."""
        name = ft_conf.read(self._conf, warn=lambda *_: None).get("AVATAR_MODEL", "")
        if not NAME.fullmatch(name):
            return None
        path = avatar_models.path(self._folder, name)
        seen = (path, _stamp(path))
        if seen != self._seen:
            facts = {"name": name, "title": name, "path": path, "body": False, "may_share": False, "vrm": False}
            try:
                info = ftmodel.read(path, hands=False)
                facts.update(title=info.name or name, body=info.has_body, may_share=info.may_share, vrm=info.format.startswith("vrm"))
            except (OSError, ftmodel.BadModel):
                pass
            self._seen, self._facts = seen, facts
        return dict(self._facts)


def allow_command(path):
    """Marks the model in the file as one its person lets the others in a room have."""
    return [hand_models.IMPORTER, "--allow-share", path]


def allowed(code, stdout):
    """(whether the mark is on, what to tell its person) from what the command printed."""
    said = hand_models.parse_import(stdout)
    if code == 0 and said.get("share") == "yes":
        return True, f"The people in your rooms may have {said.get('name') or 'your avatar'} now"
    return False, "Your avatar stays on this headset: " + said.get("error", "the importer stopped")
