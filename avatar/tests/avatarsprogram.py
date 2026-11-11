"""What the tests of ft-avatars share: the built program (screens/build.sh), and the rule that a
test is skipped where it isn't built, is older than its sources, or has no GPU to draw on."""
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TOOL = REPO / 'screens/build/ft-avatars'
SOURCES = [*(REPO / 'avatar/view').glob('*'), *(REPO / 'avatar/pose').glob('*'), *(REPO / 'avatar/room').glob('*'),
           *(REPO / 'avatar/abstract').glob('*'), *(REPO / 'avatar').glob('bodyfile.*'), REPO / 'avatar/vec.h',
           *(REPO / 'together/proto').glob('*.h'), REPO / 'together/local/datagrams.h']


def built(gpu):
    """Skips the test class that calls it from setUpClass unless the program is there to run (and, with `gpu`, to draw)."""
    if not TOOL.exists():
        raise unittest.SkipTest('screens/build/ft-avatars is not built (screens/build.sh)')
    if any(src.stat().st_mtime > TOOL.stat().st_mtime for src in SOURCES):
        raise unittest.SkipTest('screens/build/ft-avatars is older than its sources (screens/build.sh)')
    if gpu and not Path('/dev/dri/renderD128').exists():
        raise unittest.SkipTest('no GPU to draw on')
