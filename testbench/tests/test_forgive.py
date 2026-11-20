"""steamvr-forgive: only the boot race's failed SteamVR session is forgiven (the loader's and
install.sh's guard against SteamOS's health check). No SteamVR or journal needed."""
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent.parent / 'steamvr-forgive'
FROG = 'frog:glasses:'
RACE = '[   14.211000] frame select_steamvr.sh[3998]: VR_Init failed with VRInitError_Init_HmdNotFound\n'
MARK = '[   {}.100000] frame steamvr-health-check[4089]: Marking as failure to launch due to SERVICE_RESULT: \'exit-code\'\n'


class Forgive(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.dir = Path(tmp.name)
        self.tracker, self.out, self.log = self.dir / 'tracker', self.dir / 'forgiven', self.dir / 'journal'
        self.log.write_text('')

    def run_it(self, mode, frogs=None, uptime=20, invocation='abc', journal=None):
        if frogs is not None:
            self.tracker.write_text(FROG * frogs)
        if journal is not None:
            self.log.write_text(journal)
        env = dict(os.environ, FRAME_TESTBENCH_TRACKER=str(self.tracker), FRAME_TESTBENCH_FORGIVEN=str(self.out),
                   FRAME_TESTBENCH_JOURNAL=str(self.log), FRAME_TESTBENCH_UPTIME=str(uptime))
        env.pop('INVOCATION_ID', None)
        if invocation:
            env['INVOCATION_ID'] = invocation
        r = subprocess.run(['bash', str(SCRIPT), mode], env=env, capture_output=True, text=True, timeout=10)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout.strip()

    def count(self):
        return int(self.run_it('count'))

    def test_the_boot_race_is_forgiven_until_a_good_stop(self):
        self.run_it('stopped', frogs=1, journal=RACE)
        self.assertEqual(self.out.read_text().strip(), '1')
        self.assertEqual(self.count(), 0)
        self.run_it('stopped', frogs=2, uptime=900)          # a real failure later: it counts
        self.assertEqual(self.count(), 1)
        self.run_it('stopped', frogs=0, uptime=950)          # a good stop: the tracker cleared
        self.assertEqual(self.count(), 0)
        self.run_it('stopped', frogs=1, uptime=990, journal=RACE)   # a failure after that counts
        self.assertEqual(self.count(), 1)

    def test_not_the_race(self):
        for name, kwargs in (('late', dict(uptime=400, journal=RACE)), ('another error', dict(journal='')),
                             ('no invocation', dict(invocation='', journal=RACE))):
            with self.subTest(name):
                self.out.unlink(missing_ok=True)
                self.run_it('stopped', frogs=1, **kwargs)
                self.assertEqual(self.count(), 1)

    def test_only_the_first_stop_of_the_boot_can_be_the_race(self):
        self.run_it('stopped', frogs=0)
        self.run_it('stopped', frogs=1, journal=RACE)        # (a stop already wrote the file: not the boot's first)
        self.assertEqual(self.count(), 1)

    def test_scan_reads_the_boot_from_the_journal(self):
        self.run_it('scan', frogs=1, journal=RACE + MARK.format(15))
        self.assertEqual(self.count(), 0)
        self.run_it('scan', frogs=2, journal=RACE + MARK.format(15) + MARK.format(600))
        self.assertEqual(self.count(), 1)
        # a good stop since (the tracker holds fewer failures than the journal marked): nothing forgiven
        self.run_it('scan', frogs=1, journal=RACE + MARK.format(15) + MARK.format(600))
        self.assertEqual(self.count(), 1)
        self.run_it('scan', frogs=1, journal=MARK.format(15))           # no race
        self.assertEqual(self.count(), 1)
        self.run_it('scan', frogs=1, journal=RACE.replace('14.', '500.') + MARK.format(501))   # late
        self.assertEqual(self.count(), 1)
        self.run_it('scan', frogs=0, journal='')
        self.assertEqual(self.count(), 0)

    def test_count_trusts_no_more_than_the_tracker_holds(self):
        self.tracker.write_text(FROG)
        for text, want in (('5\n', 1), ('junk', 1), ('1\n', 0)):
            with self.subTest(text):
                self.out.write_text(text)
                self.assertEqual(self.count(), want)
        self.out.unlink()
        self.assertEqual(self.count(), 1)


if __name__ == '__main__':
    unittest.main()
