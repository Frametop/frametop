#!/usr/bin/env python3
"""Offline checks of the build's pinned downloads (DEE-309):

- every download in a build file (build.sh, deps.sh, Makefile, Containerfile, CMakeLists.txt,
  scripts/openvr.sh, scripts/stb.sh, install.sh) is checked by a sha256 or fetched by a full
  commit, and pip installs require hashes, apart from the dev container's venvs listed below;
- scripts/stb.sh keeps a matching download, refuses one that doesn't match and a header it has
  no pin for, and doesn't fetch a good copy again (curl is a made-up one on PATH);
- the OpenVR header vendored with Full Keyboard is the one scripts/openvr.sh pins.

No network: the made-up curl copies files from a temp folder.

  pack/test/pins-test.py
"""
import hashlib
import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
PRUNE = {'.git', '.worktrees', 'build', '.venv', 'node_modules', '__pycache__', 'third_party'}
BUILD_NAMES = {'build.sh', 'deps.sh', 'Makefile', 'Containerfile', 'CMakeLists.txt'}
EXTRA = ['scripts/openvr.sh', 'scripts/stb.sh', 'install.sh']
# pip installs without hashes, on purpose: the dev container's venvs, from exact == pins. The
# image and releases install the same versions from uv.lock, with its hashes. (Open in DEE-309:
# hashing these needs every platform's wheel, the 7i's too.)
UNHASHED_PIP = {('gaze/tracker/build.sh', 'requirements.txt'), ('voice/build.sh', 'requirements.txt')}
COMMIT = re.compile(r'\b[0-9a-f]{40}\b')
WINDOW = 8   # lines after a download that its check must be in


def build_files():
    out = [REPO / p for p in EXTRA]
    for dirpath, dirs, files in os.walk(REPO):
        dirs[:] = [d for d in dirs if d not in PRUNE]
        out += [Path(dirpath) / f for f in files if f in BUILD_NAMES]
    return sorted(set(p for p in out if p.is_file()))


def findings(path, text):
    """What isn't pinned in one build file: (line number, reason) each."""
    rel = str(path.relative_to(REPO))
    lines = text.splitlines()
    commits = {m.group(1) for m in re.finditer(r'^\s*(?:export\s+)?(\w+)\s*\??=\s*([0-9a-f]{40})\b', text, re.M)}
    out = []
    for i, line in enumerate(lines):
        code = line.split('#', 1)[0] if not line.lstrip().startswith('#') else ''
        after = '\n'.join(lines[i:i + WINDOW])
        if re.search(r'\bcurl\s+-', code) and '/dev/null' not in code:   # (a probe keeps nothing)
            if 'sha256' not in after.lower():
                out.append((i + 1, 'curl download without a sha256 check'))
        if re.search(r'\bgit\b.*\bclone\b', code):
            out.append((i + 1, 'git clone (fetch a full commit instead)'))
        if re.search(r'\bgit\b.*\bfetch\b', code) and not COMMIT.search(code) \
                and not any(f'${v}' in code or f'${{{v}}}' in code for v in commits):
            out.append((i + 1, 'git fetch without a full commit'))
        if re.search(r'\bpip\b.*\binstall\b', code) and '--require-hashes' not in code:
            if not any(rel == f and what in code for f, what in UNHASHED_PIP):
                out.append((i + 1, 'pip install without --require-hashes'))
        if re.search(r'FetchContent_Declare|ExternalProject_Add|file\s*\(\s*DOWNLOAD', code) \
                and not re.search(r'URL_HASH\s+SHA256|EXPECTED_HASH\s+SHA256|GIT_TAG\s+[0-9a-f]{40}', after):
            out.append((i + 1, 'CMake download without a sha256 or commit'))
    return out


class Scan(unittest.TestCase):
    def test_every_download_is_pinned(self):
        files = build_files()
        self.assertGreater(len(files), 15, 'the search for build files found too few')
        bad = []
        for path in files:
            for n, why in findings(path, path.read_text(errors='replace')):
                bad.append(f'{path.relative_to(REPO)}:{n}: {why}')
        self.assertEqual(bad, [])

    def test_the_scan_catches_what_it_should(self):
        p = REPO / 'x/build.sh'
        self.assertEqual([w for _, w in findings(p, 'curl -fsSL https://a/b -o b\nmake\n')],
                         ['curl download without a sha256 check'])
        self.assertEqual(findings(p, 'curl -fsSL -o b https://a/b\necho "$S  b" | sha256sum -c\n'), [])
        self.assertEqual(len(findings(p, 'git clone --depth 1 --branch v1 https://a/b.git\n')), 1)
        self.assertEqual(len(findings(p, 'git -C s fetch -q --depth 1 https://a/b.git v1\n')), 1)
        self.assertEqual(findings(p, 'c=' + 'a' * 40 + '\ngit -C s fetch -q https://a/b.git $c\n'), [])
        self.assertEqual(len(findings(p, 'pip install -r other.txt\n')), 1)
        self.assertEqual(findings(p, 'pip install --require-hashes -r r.txt\n'), [])
        self.assertEqual(findings(p, '# curl -o x https://a/b, in a comment\n'), [])


class Stb(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='ft-pins-test-')
        t = Path(self.tmp.name)
        self.src, self.bin, self.inc, self.log = t / 'src', t / 'bin', t / 'include', t / 'curl.log'
        self.src.mkdir()
        self.bin.mkdir()
        # curl -fsSL URL -o FILE: copies src/NAME (the URL's last part), and logs the URL
        curl = self.bin / 'curl'
        curl.write_text('#!/bin/sh\nurl=; out=\nwhile [ $# -gt 0 ]; do case $1 in -o) out=$2; shift ;; -*) ;; '
                        '*) url=$1 ;; esac; shift; done\necho "$url" >>"$LOG"\ncp "$SRC/${url##*/}" "$out"\n')
        curl.chmod(0o755)

    def tearDown(self):
        self.tmp.cleanup()

    def pins(self):
        out = subprocess.run(['bash', '-c', '. "$1"; for h in stb_truetype.h stb_image_write.h stb_image.h; do '
                              'echo "$h $(stb_sha256 $h)"; done', 'x', REPO / 'scripts/stb.sh'],
                             check=True, capture_output=True, text=True).stdout
        return dict(line.split() for line in out.splitlines())

    def fetch(self, *names):
        env = dict(os.environ, PATH=f'{self.bin}:{os.environ["PATH"]}', SRC=str(self.src), LOG=str(self.log))
        return subprocess.run(['bash', '-c', 'set -e; . "$1"; shift; stb_fetch "$@"', 'x',
                               REPO / 'scripts/stb.sh', self.inc, *names], env=env, capture_output=True, text=True)

    def fetched(self):
        return self.log.read_text().splitlines() if self.log.exists() else []

    def test_pins_are_sha256s(self):
        pins = self.pins()
        self.assertEqual(set(pins), {'stb_truetype.h', 'stb_image_write.h', 'stb_image.h'})
        for name, pin in pins.items():
            self.assertRegex(pin, r'^[0-9a-f]{64}$', name)

    def test_a_download_that_does_not_match_is_refused(self):
        (self.src / 'stb_truetype.h').write_text('not the pinned header\n')
        r = self.fetch('stb_truetype.h')
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("doesn't match its pinned sha256", r.stderr)
        self.assertEqual(sorted(p.name for p in self.inc.iterdir()), [])   # nothing kept, no .part
        commit = re.search(r'stb_commit=(\w+)', (REPO / 'scripts/stb.sh').read_text()).group(1)
        self.assertEqual(self.fetched(), [f'https://raw.githubusercontent.com/nothings/stb/{commit}/stb_truetype.h'])

    def test_a_header_without_a_pin_is_refused(self):
        r = self.fetch('stb_vorbis.c')
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('no pinned sha256', r.stderr)
        self.assertEqual(self.fetched(), [])

    def test_a_good_copy_is_kept_and_a_bad_one_fetched_again(self):
        """A made-up pin (stb_sha256 overridden) stands for the real header's."""
        body = b'the header\n'
        (self.src / 'stb_image.h').write_bytes(body)
        pin = hashlib.sha256(body).hexdigest()
        env = dict(os.environ, PATH=f'{self.bin}:{os.environ["PATH"]}', SRC=str(self.src), LOG=str(self.log))
        run = ['bash', '-c', f'set -e; . "$1"; stb_sha256() {{ echo {pin}; }}; stb_fetch "$2" stb_image.h',
               'x', REPO / 'scripts/stb.sh', self.inc]
        subprocess.run(run, env=env, check=True, capture_output=True)
        self.assertEqual((self.inc / 'stb_image.h').read_bytes(), body)
        subprocess.run(run, env=env, check=True, capture_output=True)
        self.assertEqual(len(self.fetched()), 1)                     # kept: not fetched again
        (self.inc / 'stb_image.h').write_bytes(b'edited\n')
        subprocess.run(run, env=env, check=True, capture_output=True)
        self.assertEqual(len(self.fetched()), 2)                     # didn't match: fetched again
        self.assertEqual((self.inc / 'stb_image.h').read_bytes(), body)


class OpenVR(unittest.TestCase):
    def test_keyboard_vendored_header_is_the_pinned_one(self):
        text = (REPO / 'scripts/openvr.sh').read_text()
        pin = re.search(r'^openvr_fetch openvr\.h ([0-9a-f]{64})$', text, re.M).group(1)
        vendored = (REPO / 'keyboard/third_party/openvr/openvr.h').read_bytes()
        self.assertEqual(hashlib.sha256(vendored).hexdigest(), pin)

    def test_pinned_by_commit(self):
        text = (REPO / 'scripts/openvr.sh').read_text()
        commit = re.search(r'^openvr_commit=([0-9a-f]{40})\b', text, re.M).group(1)
        self.assertTrue(f'openvr/raw/{commit}/' in (REPO / 'pack/Containerfile').read_text(),
                        'pack/Containerfile fetches libopenvr_api.so at another commit')
        self.assertIn('$openvr_commit/headers', text)


if __name__ == '__main__':
    unittest.main()
