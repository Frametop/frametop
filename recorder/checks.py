"""What the recorders' validate.py share: the report, SHA256SUMS against the files, JSON reading,
and the manifest's typed lookups and contributor checks. hands/rec/validate.py and
gaze/rec/validate.py both use these.

Standard library only; runs on Linux and Windows (the maintainer's PC), Python 3.12 or later."""
import hashlib
import os
import json
import re
import uuid

from recorder.files import Cancelled  # noqa: F401  (validate.Cancelled, as before)

SUM_RE = re.compile(r"^([0-9a-f]{64})  (\S.*)$")
CHUNK = 1 << 20
MAX_LISTED = 10   # removed paths and the like: list this many, then "and N more"


class Report:
    def __init__(self, path):
        self.path = path
        self.errors = []
        self.warnings = []
        self.summary = {}
        self.bytes = 0

    def error(self, text):
        self.errors.append(text)

    def warn(self, text):
        self.warnings.append(text)

    @property
    def ok(self):
        return not self.errors

    def as_dict(self):
        return {"path": self.path, "ok": self.ok, "errors": self.errors, "warnings": self.warnings,
                "summary": self.summary, "bytes": self.bytes}


def listed(items):
    items = list(items)
    more = len(items) - MAX_LISTED
    return ", ".join(items[:MAX_LISTED]) + (f" and {more} more" if more > 0 else "")


def is_uuid4(s):
    try:
        u = uuid.UUID(s)
    except (TypeError, ValueError, AttributeError):
        return False
    return u.version == 4 and str(u) == s


def sha256_file(path, step=None, cancelled=lambda: False):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(CHUNK):
            if cancelled():
                raise Cancelled()
            h.update(chunk)
            if step:
                step(len(chunk))
    return h.hexdigest()


def check_sums(root, files, report, step, cancelled):
    """SHA256SUMS at root: every file in files listed (but itself), each matching, none missing."""
    path = os.path.join(root, "SHA256SUMS")
    if not os.path.isfile(path):
        report.error("SHA256SUMS is missing")
        return
    sums = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            m = SUM_RE.match(line)
            if not m:
                report.error(f"SHA256SUMS line {n}: not \"<sha256>  <path>\"")
                continue
            digest, rel = m.groups()
            if rel.startswith("/") or ".." in rel.split("/") or rel == "SHA256SUMS":
                report.error(f"SHA256SUMS line {n}: a path that doesn't belong: {rel}")
                continue
            if rel in sums:
                report.error(f"SHA256SUMS lists {rel} twice")
            sums[rel] = digest
    present = set(files) - {"SHA256SUMS"}
    missing = sorted(set(sums) - present)
    if missing:
        report.error(f"in SHA256SUMS but missing: {listed(missing)}")
    unlisted = sorted(present - set(sums))
    if unlisted:
        report.error(f"not in SHA256SUMS: {listed(unlisted)}")
    bad = []
    for rel in sorted(set(sums) & present):
        if sha256_file(os.path.join(root, rel), step, cancelled) != sums[rel]:
            bad.append(rel)
    if bad:
        report.error(f"checksum mismatch: {listed(bad)}")


def read_json_file(path, rel, report):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except OSError as e:
        report.error(f"{rel}: can't read it: {e.strerror or e}")
    except ValueError as e:
        report.error(f"{rel}: not valid JSON: {e}")
    return None


def need(obj, key, types, where, report, required=True):
    """obj[key] if it's of the given type(s); an error otherwise."""
    if not isinstance(obj, dict) or key not in obj:
        if required:
            report.error(f"manifest: {where}{key} is missing")
        return None
    v = obj[key]
    allowed = types if isinstance(types, tuple) else (types,)
    if not isinstance(v, allowed) or (isinstance(v, bool) and bool not in allowed):
        report.error(f"manifest: {where}{key} has the wrong type ({type(v).__name__})")
        return None
    return v
