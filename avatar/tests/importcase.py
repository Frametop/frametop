"""What the importer's tests share: the built program (avatar/build.sh) run on a made-up file,
and what it printed. A test is skipped where the program isn't built, or is older than its
sources."""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE.parent))
import ftmodel  # noqa: E402

# FT_MODEL_IMPORT: another build of the program to test (one with sanitizers, say)
TOOL = Path(os.environ.get('FT_MODEL_IMPORT') or REPO / 'avatar/build/ft-model-import')
FUZZ = int(os.environ.get('FT_MODEL_IMPORT_FUZZ') or 120)   # damaged copies tried of each file
SOURCES = [*(REPO / 'avatar/import').glob('*'), *(REPO / 'avatar').glob('modelfile.*'), *(REPO / 'avatar').glob('bodyfile.*'),
           *(REPO / 'avatar').glob('sharemark.*'), REPO / 'avatar/modelbytes.h', REPO / 'avatar/vec.h']


class ImportCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not TOOL.exists():
            raise unittest.SkipTest('avatar/build/ft-model-import is not built (avatar/build.sh)')
        if any(src.stat().st_mtime > TOOL.stat().st_mtime for src in SOURCES):
            raise unittest.SkipTest('avatar/build/ft-model-import is older than its sources (avatar/build.sh)')
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = Path(cls.tmp.name)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def source(self, name, data):
        path = self.dir / name
        path.write_bytes(data if isinstance(data, bytes) else data.encode())
        return path

    def run_tool(self, path, *args):
        """(exit status, {key: value} of what it printed)."""
        done = subprocess.run([str(TOOL), str(path), '--to', str(self.dir / 'out'), *args],
                              capture_output=True, text=True, timeout=120)
        said = dict(line.split(' ', 1) for line in done.stdout.splitlines() if ' ' in line)
        return done.returncode, said

    def imported(self, name, data, *args, body=False):
        status, said = self.run_tool(self.source(name, data), *args)
        self.assertEqual(status, 0, said)
        return ftmodel.read(said['file'], body=body), said

    def refused(self, name, data, *args):
        status, said = self.run_tool(self.source(name, data), *args)
        self.assertEqual(status, 1, said)
        self.assertIn('error', said)
        return said['error']
