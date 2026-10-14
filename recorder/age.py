"""age v1 encryption to X25519 recipients (https://age-encryption.org/v1), for the eye recorder's
export: each chunk and health.json is encrypted to the dataset's recipients (gaze/rec/recipients.txt),
so only the holders of those keys can read the images, wherever the files end up.

Written on the cryptography library (X25519, HKDF-SHA256, ChaCha20-Poly1305, HMAC-SHA256), which
SteamOS's Python has; the age command isn't on the Frame. recorder/tests/test_age.py checks it
against itself, and against the age tool (age 1.1.1 on the 7i) in both directions.

  encrypt_file(src, dst, recipients)    recipients: "age1..." strings
  decrypt_file(src, dst, identity)      identity: an "AGE-SECRET-KEY-1..." string (tests, tools)
  read_recipients(path)                 a recipients file: one "age1..." a line, # comments
  shape(path, recipients)               None if path looks like a whole age file to that many keys

Only X25519 recipients, no passphrases or plugins, no armor.
"""
import base64
import hmac
import hashlib
import os

VERSION_LINE = b"age-encryption.org/v1"
CHUNK = 64 * 1024
TAG = 16
X25519_INFO = b"age-encryption.org/v1/X25519"
RECIPIENT_HRP, IDENTITY_HRP = "age", "age-secret-key-"


def _crypto():
    try:
        from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
        from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
        from cryptography.hazmat.primitives.kdf.hkdf import HKDF
        from cryptography.hazmat.primitives import hashes, serialization
    except ImportError:
        raise RuntimeError("encrypting needs Python's cryptography library (SteamOS's /usr/bin/python3 has it)")
    return X25519PrivateKey, X25519PublicKey, ChaCha20Poly1305, HKDF, hashes, serialization


# ------------------------------------------------------------------------------------------
# Bech32 (BIP 173), which age's keys are written in

_CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l"


def _polymod(values):
    gen = (0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3)
    chk = 1
    for v in values:
        b = chk >> 25
        chk = (chk & 0x1ffffff) << 5 ^ v
        for i in range(5):
            chk ^= gen[i] if (b >> i) & 1 else 0
    return chk


def _expand(hrp):
    return [ord(c) >> 5 for c in hrp] + [0] + [ord(c) & 31 for c in hrp]


def _bits(data, frm, to, pad):
    acc = bits = 0
    out = []
    for v in data:
        acc = (acc << frm) | v
        bits += frm
        while bits >= to:
            bits -= to
            out.append((acc >> bits) & ((1 << to) - 1))
    if pad and bits:
        out.append((acc << (to - bits)) & ((1 << to) - 1))
    elif not pad and (bits >= frm or (acc << (to - bits)) & ((1 << to) - 1)):
        raise ValueError("bad padding")
    return out


def bech32_encode(hrp, data):
    d = _bits(data, 8, 5, True)
    mod = _polymod(_expand(hrp) + d + [0] * 6) ^ 1
    return hrp + "1" + "".join(_CHARSET[x] for x in d + [(mod >> 5 * (5 - i)) & 31 for i in range(6)])


def bech32_decode(s):
    """(hrp, bytes). Raises ValueError."""
    if s.lower() != s and s.upper() != s:
        raise ValueError("mixed case")
    s = s.lower()
    pos = s.rfind("1")
    if pos < 1 or pos + 7 > len(s):
        raise ValueError("not bech32")
    hrp, rest = s[:pos], s[pos + 1:]
    if any(c not in _CHARSET for c in rest):
        raise ValueError("not bech32")
    d = [_CHARSET.index(c) for c in rest]
    if _polymod(_expand(hrp) + d) != 1:
        raise ValueError("bad checksum")
    return hrp, bytes(_bits(d[:-6], 5, 8, False))


def parse_recipient(s):
    hrp, key = bech32_decode(s.strip())
    if hrp != RECIPIENT_HRP or len(key) != 32:
        raise ValueError("not an age X25519 recipient: %r" % s[:12])
    return key


def parse_identity(s):
    hrp, key = bech32_decode(s.strip())
    if hrp != IDENTITY_HRP or len(key) != 32:
        raise ValueError("not an age X25519 identity")
    return key


def keygen():
    """(identity, recipient) strings: a new key pair (for tests)."""
    priv_cls, _, _, _, _, ser = _crypto()
    k = priv_cls.generate()
    raw = k.private_bytes(ser.Encoding.Raw, ser.PrivateFormat.Raw, ser.NoEncryption())
    pub = k.public_key().public_bytes(ser.Encoding.Raw, ser.PublicFormat.Raw)
    return bech32_encode(IDENTITY_HRP, raw).upper(), bech32_encode(RECIPIENT_HRP, pub)


def read_recipients(path):
    out = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                parse_recipient(line)
                out.append(line)
    if not out:
        raise ValueError("%s has no recipients" % path)
    return out


# ------------------------------------------------------------------------------------------
# The format

def _b64(b):
    return base64.b64encode(b).rstrip(b"=")


def _unb64(s):
    if b"=" in s or b"\r" in s or b"\n" in s:
        raise ValueError("bad base64")
    out = base64.b64decode(s + b"=" * (-len(s) % 4), validate=True)
    if _b64(out) != s:
        raise ValueError("non-canonical base64")
    return out


def _hkdf(ikm, salt, info):
    _, _, _, hkdf, hashes, _ = _crypto()
    return hkdf(algorithm=hashes.SHA256(), length=32, salt=salt, info=info).derive(ikm)


def _wrap(lines):
    """A stanza body: base64 in 64-column lines, the last one shorter (empty if need be)."""
    out = [lines[i:i + 64] for i in range(0, len(lines), 64)]
    if not out or len(out[-1]) == 64:
        out.append(b"")
    return b"\n".join(out) + b"\n"


def header(recipients, file_key):
    priv_cls, pub_cls, aead, _, _, ser = _crypto()
    h = VERSION_LINE + b"\n"
    for r in recipients:
        rk = parse_recipient(r)
        e = priv_cls.generate()
        epub = e.public_key().public_bytes(ser.Encoding.Raw, ser.PublicFormat.Raw)
        shared = e.exchange(pub_cls.from_public_bytes(rk))
        if shared == bytes(32):
            raise ValueError("a recipient key gives a zero shared secret")
        wrap_key = _hkdf(shared, epub + rk, X25519_INFO)
        body = aead(wrap_key).encrypt(bytes(12), file_key, None)
        h += b"-> X25519 " + _b64(epub) + b"\n" + _wrap(_b64(body))
    h += b"---"
    mac = hmac.new(_hkdf(file_key, b"", b"header"), h, hashlib.sha256).digest()
    return h + b" " + _b64(mac) + b"\n"


def _nonce(counter, last):
    return counter.to_bytes(11, "big") + (b"\x01" if last else b"\x00")


def encrypt_stream(src, dst, recipients):
    """Encrypt the file object src into dst. Returns the plaintext's size."""
    _, _, aead, _, _, _ = _crypto()
    if not recipients:
        raise ValueError("no recipients")
    file_key = os.urandom(16)
    dst.write(header(recipients, file_key))
    nonce = os.urandom(16)
    dst.write(nonce)
    box = aead(_hkdf(file_key, nonce, b"payload"))
    size, counter = 0, 0
    chunk = src.read(CHUNK)
    while True:
        nxt = src.read(CHUNK) if len(chunk) == CHUNK else b""
        last = not nxt
        dst.write(box.encrypt(_nonce(counter, last), chunk, None))
        size += len(chunk)
        if last:
            return size
        counter += 1
        chunk = nxt


def encrypt_file(src, dst, recipients):
    """src to dst (written as dst.tmp, then moved). Returns the plaintext's size."""
    tmp = dst + ".tmp"
    with open(src, "rb") as fi, open(tmp, "wb") as fo:
        n = encrypt_stream(fi, fo, recipients)
        fo.flush()
        os.fsync(fo.fileno())
    os.replace(tmp, dst)
    return n


def encrypt_bytes(data, recipients):
    import io
    out = io.BytesIO()
    encrypt_stream(io.BytesIO(data), out, recipients)
    return out.getvalue()


def _read_header(src):
    """(stanzas [(type, args, body)], the header bytes the MAC covers, mac)."""
    def line():
        b = src.readline(8192)
        if not b.endswith(b"\n"):
            raise ValueError("truncated header")
        return b[:-1]
    if line() != VERSION_LINE:
        raise ValueError("not an age v1 file")
    covered = VERSION_LINE + b"\n"
    stanzas = []
    while True:
        b = line()
        if b.startswith(b"--- "):
            covered += b"---"
            return stanzas, covered, _unb64(b[4:])
        if not b.startswith(b"-> "):
            raise ValueError("bad header line")
        covered += b + b"\n"
        args = b[3:].split(b" ")
        body = b""
        while True:
            part = line()
            covered += part + b"\n"
            if len(part) > 64:
                raise ValueError("bad stanza body")
            body += part
            if len(part) < 64:
                break
        stanzas.append((args[0], args[1:], _unb64(body)))


def decrypt_stream(src, dst, identity):
    priv_cls, pub_cls, aead, _, _, ser = _crypto()
    k = priv_cls.from_private_bytes(parse_identity(identity))
    me = k.public_key().public_bytes(ser.Encoding.Raw, ser.PublicFormat.Raw)
    stanzas, covered, mac = _read_header(src)
    file_key = None
    for typ, args, body in stanzas:
        if typ != b"X25519" or len(args) != 1 or len(body) != 32:
            continue
        epub = _unb64(args[0])
        shared = k.exchange(pub_cls.from_public_bytes(epub))
        try:
            file_key = aead(_hkdf(shared, epub + me, X25519_INFO)).decrypt(bytes(12), body, None)
            break
        except Exception:
            continue
    if file_key is None:
        raise ValueError("not encrypted to this identity")
    if not hmac.compare_digest(hmac.new(_hkdf(file_key, b"", b"header"), covered, hashlib.sha256).digest(), mac):
        raise ValueError("the header's MAC doesn't match")
    nonce = src.read(16)
    if len(nonce) != 16:
        raise ValueError("truncated")
    box = aead(_hkdf(file_key, nonce, b"payload"))
    counter, size = 0, 0
    chunk = src.read(CHUNK + TAG)
    while True:
        nxt = src.read(CHUNK + TAG) if len(chunk) == CHUNK + TAG else b""
        last = not nxt
        try:
            plain = box.decrypt(_nonce(counter, last), chunk, None)
        except Exception:
            raise ValueError("chunk %d doesn't decrypt (damaged or truncated)" % counter)
        if last and not plain and counter > 0:
            raise ValueError("an empty last chunk")
        dst.write(plain)
        size += len(plain)
        if last:
            return size
        counter += 1
        chunk = nxt


def decrypt_file(src, dst, identity):
    with open(src, "rb") as fi, open(dst, "wb") as fo:
        return decrypt_stream(fi, fo, identity)


HEADER_MAX = 8192   # an age header to a few X25519 recipients is far shorter


def shape(path, recipients):
    """None if path looks like a whole age v1 file to len(recipients) X25519 recipients, else
    what's wrong. Reads the header and the size only: nothing can be decrypted without a key."""
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        head = f.read(HEADER_MAX)
    end = head.find(b"\n---")
    if not head.startswith(VERSION_LINE + b"\n") or end < 0:
        return "not an age v1 file"
    mac_end = head.find(b"\n", end + 1)
    if mac_end < 0:
        return "its header is cut short"
    stanzas = [line for line in head[:end].split(b"\n") if line.startswith(b"-> ")]
    if any(not s.startswith(b"-> X25519 ") for s in stanzas):
        return "a recipient that isn't X25519"
    if recipients and len(stanzas) != len(recipients):
        return f"encrypted to {len(stanzas)} recipients, not {len(recipients)}"
    payload = size - (mac_end + 1) - 16
    if payload < TAG:
        return "cut short"
    whole, rest = divmod(payload, CHUNK + TAG)
    if (rest and rest < TAG) or (not rest and whole == 0) or (rest == TAG and whole):
        return "a size no age file has (cut short?)"
    return None


def decrypt_bytes(data, identity):
    import io
    out = io.BytesIO()
    decrypt_stream(io.BytesIO(data), out, identity)
    return out.getvalue()
