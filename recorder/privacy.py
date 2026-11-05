"""What identifies a Frame in the files a recording keeps: the factory calibration's
(/persist/xrservice.json) serials, UUIDs, MACs and ids, and strings that look like serial
numbers. The hand recorder strips them before saving calibration.json; both recorders'
validate.py check nothing of the kind is left.

Standard library only; runs on Linux and Windows (the maintainer's PC)."""
import re

ID_TOKENS = {"serial", "sn", "uuid", "guid", "mac", "id", "ids", "identifier"}


def key_identifies(key):
    k = str(key)
    if re.search(r"serial|uuid", k, re.I):
        return True
    tokens = re.findall(r"[A-Z]+(?![a-z])|[A-Z]?[a-z]+|\d+", k)
    return any(t.lower() in ID_TOKENS for t in tokens)


def value_identifies(value):
    """A string holding something like a serial number: a run of 8 or more letters and digits
    with at least 4 digits and a letter (sensor names like og01a1b are shorter)."""
    if not isinstance(value, str):
        return False
    for run in re.findall(r"[A-Za-z0-9]+", value):
        if len(run) >= 8 and sum(c.isdigit() for c in run) >= 4 and any(c.isalpha() for c in run):
            return True
    return False


def strip_calibration(xrservice_json_dict):
    """/persist/xrservice.json without what identifies the unit: keys naming a serial, sn,
    uuid, mac or id, and string values that look like serial numbers. Returns (cleaned dict,
    removed key paths such as "cameras[0].serial")."""
    removed = []

    def walk(o, path):
        if isinstance(o, dict):
            out = {}
            for k, v in o.items():
                p = "%s.%s" % (path, k) if path else str(k)
                if key_identifies(k) or value_identifies(v):
                    removed.append(p)
                    continue
                out[k] = walk(v, p)
            return out
        if isinstance(o, list):
            out = []
            for i, v in enumerate(o):
                p = "%s[%d]" % (path, i)
                if value_identifies(v):
                    removed.append(p)
                    out.append(None)   # keep the other items' positions
                    continue
                out.append(walk(v, p))
            return out
        return o

    return walk(xrservice_json_dict, ""), removed
