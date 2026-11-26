#!/usr/bin/env python3
"""Hide what a debug report's reader doesn't need, before the user sends it (debug-report.sh).

Usage: debug-mask.py FILE [HOSTNAME]  -> the masked text on stdout

Hidden: the values of anything named like a token, secret, password, credential, cookie, auth or
API key, wherever it is on a line (KEY=value, KEY: value, "key": value with a nested object or
list too, --key value, and query parameters), Authorization headers, Bearer and Basic
credentials, private keys, the command lines of the input relay's command: actions, passwords in
URLs, email addresses, IPv4 addresses but 127.x.x.x (shown as <lan-ip>, <tailnet-ip> or <ip>; a
version after "version", "v" or "distrobox:" stays), IPv6 addresses but ::1, Tailscale names,
Wi-Fi network names, Bluetooth addresses, the headset's serial number, and the hostname at the
start of journal lines. Colours go. An unquoted value ends at a space: "password: two words"
keeps "words".
Runs on the host's system Python: standard library only, 3.12's syntax.
"""
import re
import sys

NAME = r"[A-Za-z0-9_.-]*(?:token|secret|passw|credential|cookie|auth(?!or)|bearer|api_?key|private_?key)[A-Za-z0-9_.-]*"
HIDDEN = "<hidden>"
CLOSE = {"{": "}", "[": "]"}


def _value_end(text, i):
    """Where the value starting at i ends: a quoted string, a {...} or [...] block (nested, with
    strings in it; one that doesn't close ends with its line), or up to whitespace or a , } ] & ;
    that follows it."""
    if i >= len(text):
        return i
    c = text[i]
    if c in "\"'":
        j = i + 1
        while j < len(text) and text[j] != c and text[j] != "\n":
            j += 2 if text[j] == "\\" else 1
        return min(j + 1, len(text))
    if c in CLOSE:
        depth, j, quote = 0, i, None
        while j < len(text):
            ch = text[j]
            if quote:
                if ch == "\\":
                    j += 1
                elif ch == quote:
                    quote = None
            elif ch == '"':
                quote = ch
            elif ch in CLOSE:
                depth += 1
            elif ch in "}]":
                depth -= 1
                if depth == 0:
                    return j + 1
            j += 1
        line_end = text.find("\n", i)
        return len(text) if line_end < 0 else line_end
    m = re.compile(r"[^\s,}\]&;\"']+").match(text, i)
    return m.end() if m else i


def hide_named(text):
    """NAME=value, NAME: value, "name": value and --name value, anywhere on a line."""
    out, pos = [], 0
    for m in re.finditer(r"(?i)(?<![A-Za-z0-9_.-])" + NAME + r"[\"']?\s*[=:]\s*|(?<!\S)--?" + NAME + r"[ \t]+(?!-)",
                         text):
        if m.start() < pos:
            continue
        end = _value_end(text, m.end())
        if end == m.end():
            continue
        quoted = text[m.end()] in "\"'{["
        out.append(text[pos:m.end()])
        out.append(f'"{HIDDEN}"' if quoted else HIDDEN)
        pos = end
    out.append(text[pos:])
    return "".join(out)


def _ipv4(m):
    a, b, c, d = (int(x) for x in m.group(0).split("."))
    if max(a, b, c, d) > 255 or a == 127:
        return m.group(0)
    if re.search(r"(?i)(version|\bv|distrobox:?)\s*$", m.string[max(0, m.start() - 12):m.start()]):
        return m.group(0)   # a version, such as "distrobox: 1.8.2.5"
    if a == 10 or (a == 172 and 16 <= b <= 31) or (a, b) == (192, 168):
        return "<lan-ip>"
    if a == 100 and 64 <= b <= 127:
        return "<tailnet-ip>"   # (Tailscale's range)
    return "<ip>"


def mask(text, hostname=""):
    text = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", text)                               # colours
    if hostname:   # journal lines: "Nov 25 17:56:30 HOST unit[pid]: ..."
        text = re.sub(r"(?m)^([A-Z][a-z]{2} [ \d]\d [\d:]{8}) " + re.escape(hostname) + r" ", r"\1 <host> ", text)
    text = re.sub(r"-----BEGIN [A-Z ]*PRIVATE KEY-----.*?(-----END [A-Z ]*PRIVATE KEY-----|\Z)",
                  "<hidden private key>", text, flags=re.S)
    text = re.sub(r"(?i)\b(authorization\s*:\s*)[^\n\"']+", r"\1" + HIDDEN, text)
    text = re.sub(r"\bcommand:(?:\\.|[^\"\\\n])+", "command:" + HIDDEN, text)          # the relay's command: actions
    text = re.sub(r"(?i)\b(Bearer|Basic)\s+[A-Za-z0-9._~+/=-]{6,}", r"\1 " + HIDDEN, text)
    text = hide_named(text)
    text = re.sub(r"(?i)([?&;](?:[^=&\s\"']*(?:token|key|pin|sig|secret|pass|auth|code)[^=&\s\"']*)=)[^&\s\"']+",
                  r"\1" + HIDDEN, text)
    text = re.sub(r"://[^/\s:@]+:[^/\s@]+@", "://" + HIDDEN + "@", text)               # user:pw@host
    text = re.sub(r"(?<![A-Za-z0-9._%+-])[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*\.(?!(?:service|scope|socket|target|timer|slice|"
                  r"mount|path|device|swap)\b)[A-Za-z]{2,}\b", "<email>", text)       # (not user@1000.service)
    text = re.sub(r"(?<![A-Za-z0-9.-])[A-Za-z0-9.-]+\.ts\.net\b", "<tailnet-name>", text)
    text = re.sub(r"(?i)\b(ssid)(\s*[=:]\s*|\s+)('[^'\n]*'|\"[^\"\n]*\"|\S+)", r"\1\2" + HIDDEN, text)
    text = re.sub(r"\b(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}\b", "xx:xx:xx:xx:xx:xx", text)
    text = re.sub(r"cv\.[A-Z0-9]{8,}", "cv.<serial>", text)
    text = re.sub(r"(?<![\w.])\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}(?!\w|\.\d)", _ipv4, text)
    # IPv6: three or more colons, or a "::", between hex groups (not a time like 17:56:30)
    text = re.sub(r"(?<![\w:.])(?=[0-9a-fA-F:]*::|(?:[0-9a-fA-F]{1,4}:){3})(?!::1(?![\w:]))"
                  r"[0-9a-fA-F]{0,4}(?::[0-9a-fA-F]{0,4}){2,7}(?![\w:])", "<ipv6>", text)
    return text


if __name__ == "__main__":
    with open(sys.argv[1], encoding="utf-8", errors="replace") as f:
        sys.stdout.write(mask(f.read(), sys.argv[2] if len(sys.argv) > 2 else ""))
