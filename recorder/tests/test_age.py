#!/usr/bin/python3
"""recorder/age.py: age v1 to X25519 recipients. data/age-vector.age was made by age 1.1.1 (on
the 7i, 2026-10-14) for data/age-vector.key, a test-only identity; its plaintext is the pattern
below. Real age also decrypted this module's files to both of the dataset's keys then (not
repeated here: it needs the 7i). Needs the cryptography library: SteamOS's /usr/bin/python3.

  /usr/bin/python3 recorder/tests/test_age.py
"""
import io
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", ".."))
from recorder import age  # noqa: E402

try:
    age._crypto()
    HAVE = True
except RuntimeError:
    HAVE = False


def pattern(n):
    return bytes((i * 7 + i // 256) % 256 for i in range(n))


@unittest.skipUnless(HAVE, "no cryptography library (run with SteamOS's /usr/bin/python3)")
class AgeTest(unittest.TestCase):
    def test_real_age_vector(self):
        with open(os.path.join(HERE, "data", "age-vector.key")) as f:
            ident = [line.strip() for line in f if line.startswith("AGE-SECRET-KEY-1")][0]
        with open(os.path.join(HERE, "data", "age-vector.age"), "rb") as f:
            data = f.read()
        self.assertEqual(age.decrypt_bytes(data, ident), pattern(65537))

    def test_round_trip_sizes(self):
        a, ra = age.keygen()
        b, rb = age.keygen()
        for n in (0, 1, age.CHUNK - 1, age.CHUNK, age.CHUNK + 1, 2 * age.CHUNK, 3 * age.CHUNK + 5):
            d = os.urandom(n)
            e = age.encrypt_bytes(d, [ra, rb])
            self.assertEqual(age.decrypt_bytes(e, a), d, n)
            self.assertEqual(age.decrypt_bytes(e, b), d, n)
            chunks = max(1, -(-n // age.CHUNK))
            self.assertEqual(len(e) - e.index(b"\n---") - len(e.split(b"\n---")[1].split(b"\n")[0]) - 5,
                             16 + n + chunks * age.TAG, "header, nonce, then the chunks with their tags: %d" % n)

    def test_not_for_others(self):
        _, r = age.keygen()
        other, _ = age.keygen()
        with self.assertRaises(ValueError):
            age.decrypt_bytes(age.encrypt_bytes(b"x", [r]), other)

    def test_tampering(self):
        i, r = age.keygen()
        e = bytearray(age.encrypt_bytes(os.urandom(age.CHUNK + 10), [r]))
        for where in (len(e) - 3, e.index(b"\n---") - 3, len(e) - age.CHUNK):
            bad = bytearray(e)
            bad[where] ^= 1
            with self.assertRaises(ValueError, msg=where):
                age.decrypt_bytes(bytes(bad), i)
        with self.assertRaises(ValueError):
            age.decrypt_bytes(bytes(e[:-(age.TAG + 10)]), i)   # the last chunk cut off: chunk 0 isn't marked last

    def test_keys(self):
        i, r = age.keygen()
        self.assertTrue(r.startswith("age1") and i.startswith("AGE-SECRET-KEY-1"))
        self.assertEqual(len(age.parse_recipient(r)), 32)
        with self.assertRaises(ValueError):
            age.parse_recipient(r[:-1] + ("q" if r[-1] != "q" else "p"))   # checksum
        with self.assertRaises(ValueError):
            age.parse_recipient(i)

    def test_files_and_recipients(self):
        rec = age.read_recipients(os.path.join(HERE, "..", "..", "gaze", "rec", "recipients.txt"))
        self.assertEqual(len(rec), 2)
        with tempfile.TemporaryDirectory() as d:
            src, dst = os.path.join(d, "a"), os.path.join(d, "a.age")
            with open(src, "wb") as f:
                f.write(b"hello")
            self.assertEqual(age.encrypt_file(src, dst, rec), 5)
            self.assertFalse(os.path.exists(dst + ".tmp"))
            with open(dst, "rb") as f:
                self.assertTrue(f.read().startswith(b"age-encryption.org/v1\n-> X25519 "))

    def test_stream(self):
        i, r = age.keygen()
        out = io.BytesIO()
        self.assertEqual(age.encrypt_stream(io.BytesIO(b"abc"), out, [r]), 3)
        back = io.BytesIO()
        self.assertEqual(age.decrypt_stream(io.BytesIO(out.getvalue()), back, i), 3)
        self.assertEqual(back.getvalue(), b"abc")


if __name__ == "__main__":
    unittest.main()
