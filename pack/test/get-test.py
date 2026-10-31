#!/usr/bin/env python3
"""Offline test of get.sh's release download: a downloaded Frametop.zip installs only
when it matches the release's SHA256SUMS. get.sh is sourced without its last line (main), curl is
a made-up one on PATH that serves files from a temp folder, and the zip's install-release.sh only
leaves a mark. No network, no install.

  pack/test/get-test.py
"""
import hashlib
import os
import subprocess
import tempfile
import unittest
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BASE = 'https://github.com/Frametop/frametop/releases/download'

# curl [-I] [-o FILE] URL: serves $SRC/TAG_NAME for .../TAG/NAME, logs each URL, fails (22) for
# a file that isn't there, as curl -f does
CURL = r'''#!/bin/sh
url=; out=
while [ $# -gt 0 ]; do
  case $1 in -o) out=$2; shift ;; -C|--proto) shift ;; -*) ;; *) url=$1 ;; esac
  shift
done
echo "$url" >>"$LOG"
rest=${url%/*}
f="$SRC/${rest##*/}_${url##*/}"
[ -f "$f" ] || exit 22
if [ -n "$out" ]; then cp "$f" "$out"; else cat "$f"; fi
'''


class GetRelease(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='ft-get-test-')
        t = Path(self.tmp.name)
        self.src, self.bin, self.home = t / 'src', t / 'bin', t / 'home'
        self.log, self.mark = t / 'curl.log', t / 'installed'
        for d in (self.src, self.bin, self.home):
            d.mkdir()
        (self.bin / 'curl').write_text(CURL)
        (self.bin / 'curl').chmod(0o755)
        self.zip = t / 'Frametop.zip'
        with zipfile.ZipFile(self.zip, 'w') as z:
            info = zipfile.ZipInfo('Frametop/install-release.sh')
            info.external_attr = 0o755 << 16
            z.writestr(info, '#!/bin/sh\necho "$@" >"$MARK"\n')
        self.sha = hashlib.sha256(self.zip.read_bytes()).hexdigest()

    def tearDown(self):
        self.tmp.cleanup()

    def publish(self, tag, sums=None, zip_bytes=None):
        (self.src / f'{tag}_Frametop.zip').write_bytes(zip_bytes or self.zip.read_bytes())
        if sums is not None:
            (self.src / f'{tag}_SHA256SUMS').write_text(sums)

    def install(self, version='', zip_arg=''):
        env = dict(os.environ, PATH=f'{self.bin}:{os.environ["PATH"]}', HOME=str(self.home),
                   SRC=str(self.src), LOG=str(self.log), MARK=str(self.mark))
        script = ('source <(sed \'$d\' "$1"); shift; '
                  'install_release stable "$1" "$2" "" 0 0 -- --yes')
        return subprocess.run(['bash', '-c', script, 'x', REPO / 'get.sh', version, zip_arg],
                              env=env, capture_output=True, text=True)

    def fetched(self):
        return self.log.read_text().splitlines() if self.log.exists() else []

    def cached_zips(self):
        cache = self.home / '.cache/frametop/release'
        return sorted(p.name for p in cache.glob('Frametop-*.zip*')) if cache.exists() else []

    def test_a_matching_zip_installs(self):
        self.publish('v1.2.3', f'{self.sha}  Frametop.zip\n{"0" * 64}  frametop-release.json\n')
        r = self.install('1.2.3')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.mark.read_text().split(), ['--yes'])
        self.assertIn("Checked Frametop.zip against the release's SHA256SUMS", r.stdout)
        self.assertEqual(self.fetched(), [f'{BASE}/v1.2.3/Frametop.zip', f'{BASE}/v1.2.3/SHA256SUMS'])

    def test_a_zip_that_does_not_match_is_refused_and_dropped(self):
        self.publish('v1.2.3', f'{"1" * 64}  Frametop.zip\n')
        r = self.install('1.2.3')
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("doesn't match the release's SHA256SUMS", r.stderr)
        self.assertFalse(self.mark.exists())
        self.assertEqual(self.cached_zips(), [])          # the next run downloads it again

    def test_no_sums_no_install(self):
        self.publish('v1.2.3')
        r = self.install('1.2.3')
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("couldn't fetch the release's checksums", r.stderr)
        self.assertFalse(self.mark.exists())

    def test_sums_without_the_zip_no_install(self):
        self.publish('v1.2.3', f'{self.sha}  frametop.framedrop.json\n')
        r = self.install('1.2.3')
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("doesn't list Frametop.zip", r.stderr)
        self.assertFalse(self.mark.exists())

    def test_a_zip_url_is_checked_too(self):
        self.publish('v9', f'{self.sha} *Frametop.zip\n')   # sha256sum -b's form
        r = self.install(zip_arg=f'{BASE}/v9/Frametop.zip')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.fetched()[-1], f'{BASE}/v9/SHA256SUMS')

    def test_a_local_zip_is_not(self):
        r = self.install(zip_arg=str(self.zip))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue(self.mark.exists())
        self.assertEqual(self.fetched(), [])


if __name__ == '__main__':
    unittest.main()
