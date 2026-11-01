"""testgate's hook (scripts/testgate/testgate.py puts this folder on PYTHONPATH): in a test file
it runs, count the unittest tests that ran and the skips with their reasons, and write them to
the file FT_TEST_REPORT names when the test process ends, with how many tests the file
itself defines (unittest's loader on __main__), so testgate can tell a file that defines tests
but never runs them (no unittest.main()). Without FT_TEST_REPORT it does
nothing, and it takes the variable out of the environment, so a Python a test starts doesn't
report over its parent's."""
import os

_report = os.environ.pop("FT_TEST_REPORT", None)
if _report:
    import atexit
    import json
    import unittest

    _pid = os.getpid()
    _seen = {"run": 0, "skipped": [], "skipped_tests": []}
    _start, _skip = unittest.TestResult.startTest, unittest.TestResult.addSkip

    def _start_test(self, test):
        _seen["run"] += 1
        return _start(self, test)

    def _add_skip(self, test, reason):
        _seen["skipped"].append(reason)
        _seen["skipped_tests"].append(getattr(test, "test_case", test).id())   # a subtest's is its test's
        return _skip(self, test, reason)

    def _defined():
        """How many tests __main__ defines, as unittest.main() would find them."""
        import sys
        try:
            return unittest.defaultTestLoader.loadTestsFromModule(sys.modules["__main__"]).countTestCases()
        except Exception:   # a module the loader can't read: not counted
            return 0

    def _write():
        if os.getpid() == _pid:   # not a forked child's exit
            with open(_report, "w") as f:
                json.dump(dict(_seen, defined=_defined()), f)

    unittest.TestResult.startTest = _start_test
    unittest.TestResult.addSkip = _add_skip
    atexit.register(_write)
