"""Read-only measurement regressions use synthetic proc/sysfs trees, not Steam."""
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('measure_replacement', sys.argv.pop(1))
measure = importlib.util.module_from_spec(spec)
spec.loader.exec_module(measure)


class Measurement(unittest.TestCase):
    def test_counters_with_parentheses_and_private_argv(self):
        with tempfile.TemporaryDirectory() as directory:
            proc = Path(directory)
            pid = proc/'42'
            pid.mkdir()
            values = ['S', '1'] + ['0'] * 20
            values[11], values[12], values[19], values[21] = '20', '30', '1234', '8'
            (pid/'stat').write_text('42 (name with ) and spaces) ' + ' '.join(values))
            (pid/'status').write_text('voluntary_ctxt_switches:\t100\nnonvoluntary_ctxt_switches:\t2\n')
            (pid/'io').write_text('syscr: 300\nsyscw: 90\n')
            (pid/'cmdline').write_bytes(b'PRIVATE_SENTINEL')
            result = measure.counters(42, proc)
            self.assertEqual(result['ticks'], 50)
            self.assertEqual(result['start'], 1234)
            self.assertEqual(result['rss_bytes'], 8 * os.sysconf('SC_PAGE_SIZE'))
            self.assertEqual(result['syscw'], 90)
            self.assertNotIn('PRIVATE_SENTINEL', str(result))

    def test_cpu_units_and_counter_rates(self):
        before = {1: {'start': 5, 'ticks': 100, 'rss_bytes': 1, 'syscr': 10}}
        after = {1: {'start': 5, 'ticks': 500, 'rss_bytes': 20, 'syscr': 30}}
        result = measure.summarize({'helper': [1]}, before, after, 2, 100)['helper']
        self.assertTrue(result['valid'])
        self.assertEqual(result['cpu_percent_one_core'], 200)
        self.assertEqual(result['per_second']['syscr'], 10)
        self.assertEqual(result['rss_bytes_end'], 20)

    def test_reused_exited_and_regressed_processes_do_not_produce_metrics(self):
        before = {1: {'start': 5, 'ticks': 100, 'rss_bytes': 1}}
        for after in ({}, {1: {'start': 6, 'ticks': 200, 'rss_bytes': 1}},
                      {1: {'start': 5, 'ticks': 10, 'rss_bytes': 1}}):
            result = measure.summarize({'helper': [1]}, before, after, 1, 100)['helper']
            self.assertFalse(result['valid'])
            self.assertNotIn('cpu_percent_one_core', result)

    def test_battery_reads_only_fixed_numeric_fields(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            battery = root/'BAT0'
            battery.mkdir()
            (battery/'type').write_text('Battery')
            (battery/'status').write_text('Charging')
            (battery/'capacity').write_text('99')
            (battery/'serial_number').write_text('PRIVATE_SENTINEL')
            result = measure.battery(root)
            self.assertEqual(result['BAT0'], {'status': 'Charging', 'capacity': 99})
            self.assertNotIn('PRIVATE_SENTINEL', str(result))


if __name__ == '__main__':
    unittest.main()
