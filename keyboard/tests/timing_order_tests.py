"""Check cross-process deadline relationships without importing or running the bridge."""
from pathlib import Path
import math
import re
import sys
import unittest


NUMBER = r"((?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?)(?=\s*(?:[,;):]|\bor\b|\band\b|$))"
JS = "bridge/steam_bridge.js"
PY = "bridge/native_bridge.py"
CPP = "src/native_replacement.cpp"
VR = "src/vr.cpp"
GUARD = "src/native_restore_guard.cpp"

# Match the actual use sites, not arbitrary occurrences of .7 or .6. A source
# refactor must update these anchors explicitly rather than silently skipping a check.
SPECS = (
    ("Steam lease", JS, r"\bLEASE_MS\s*=\s*" + NUMBER, 1, .001),
    ("request acknowledgement", JS, r"\bREQUEST_MS\s*=\s*" + NUMBER, 1, .001),
    ("CDP call", PY, r"def evaluate\(self,\s*expression,\s*timeout\s*=\s*" + NUMBER, 1, 1),
    ("bridge install", PY, r"\bINSTALL_TIMEOUT\s*=\s*" + NUMBER, 1, 1),
    ("active poll", PY, r"\bACTIVE_POLL\s*=\s*" + NUMBER, 1, 1),
    ("idle poll", PY, r"\bIDLE_POLL\s*=\s*" + NUMBER, 1, 1),
    ("idle inspection", JS, r"\bIDLE_INSPECT_MS\s*=\s*" + NUMBER, 1, .001),
    ("inspection watchdog", JS, r"timer\s*=\s*root\.setInterval\([\s\S]*?\},\s*" + NUMBER, 1, .001),
    ("helper startup budget", CPP, r"install_until_\s*=\s*now\s*\+\s*" + NUMBER, 2, 1),
    ("bridge receipt freshness", PY, r"time\.monotonic\(\)\s*-\s*last_parent\s*>\s*" + NUMBER, 1, 1),
    ("bridge frame freshness", PY, r"time\.monotonic\(\)\s*-\s*frame_sent\s*<\s*" + NUMBER, 1, 1),
    ("text confirmation", CPP, r"now\s*-\s*pending_since_\s*>\s*" + NUMBER, 1, 1),
    ("key confirmation", CPP, r"now\s*-\s*native_since_\s*>\s*" + NUMBER, 1, 1),
    ("helper reply freshness", CPP, r"now\s*-\s*last_reply_\s*>\s*" + NUMBER, 1, 1),
    ("overlay hide", VR, r"monotonic_seconds\(\)\s*-\s*replacement_progress\.load\(\)\s*>\s*" + NUMBER, 1, 1),
    ("guard frame freshness", GUARD, r"now\(\)\s*-\s*sent\s*<\s*" + NUMBER, 1, 1),
    ("native guard", GUARD, r"deadline\s*=\s*sent\s*\+\s*" + NUMBER, 2, 1),
    ("parent recovery", PY, r"if\s+now\s*-\s*frame_sent\s*<\s*" + NUMBER, 1, 1),
    ("slow-work ceiling", PY, r"min\(slow_until,\s*frame_sent\s*\+\s*" + NUMBER, 1, 1),
)


def extract_values(sources):
    values = {}
    for label, source, pattern, count, scale in SPECS:
        matches = re.findall(pattern, sources[source], re.MULTILINE)
        if len(matches) != count:
            raise AssertionError(f"{label}: expected {count} timing literal(s) in {source}, found {len(matches)}; update the source anchor")
        values[label] = tuple(float(value) * scale for value in matches)
        if any(not math.isfinite(value) or value <= 0 for value in values[label]):
            raise AssertionError(f"{label}: deadlines must be positive and finite")
    return values


def check_ordering(sources):
    values = extract_values(sources)

    def before(first, second):
        if max(values[first]) >= min(values[second]):
            raise AssertionError(f"timing rule broken: {first} < {second}; got {values[first]} and {values[second]} seconds")

    for earlier in ("bridge receipt freshness", "bridge frame freshness", "guard frame freshness",
                    "overlay hide", "request acknowledgement"):
        before(earlier, "Steam lease")
    for delivery in ("text confirmation", "key confirmation"):
        before("CDP call", delivery)
        before(delivery, "helper reply freshness")
    before("helper reply freshness", "Steam lease")
    before("bridge install", "helper startup budget")
    before("active poll", "idle poll")
    before("idle poll", "request acknowledgement")
    before("idle poll", "idle inspection")
    before("inspection watchdog", "idle inspection")
    before("idle inspection", "request acknowledgement")
    before("Steam lease", "native guard")
    before("native guard", "parent recovery")
    before("parent recovery", "slow-work ceiling")


class TimingOrdering(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent
        cls.sources = {source: (root / source).read_text() for _, source, _, _, _ in SPECS}

    def test_production_sources(self):
        check_ordering(self.sources)

    def test_later_steam_expiry_is_rejected(self):
        sources = dict(self.sources)
        lease_ms = max(extract_values(sources)["native guard"]) * 2000
        sources[JS] = re.sub(r"(LEASE_MS\s*=\s*)" + NUMBER,
                             lambda match: match[1] + str(lease_ms), sources[JS], flags=re.MULTILINE)
        with self.assertRaisesRegex(AssertionError, "Steam lease < native guard"):
            check_ordering(sources)

    def test_delivery_outliving_helper_is_rejected(self):
        sources = dict(self.sources)
        reply = extract_values(sources)["helper reply freshness"][0]
        sources[CPP] = re.sub(r"(now\s*-\s*pending_since_\s*>\s*)" + NUMBER,
                              lambda match: match[1] + str(reply), sources[CPP], flags=re.MULTILINE)
        with self.assertRaisesRegex(AssertionError, "text confirmation < helper reply freshness"):
            check_ordering(sources)

    def test_missing_and_ambiguous_anchors_are_rejected(self):
        for altered in (self.sources[JS].replace("LEASE_MS", "RENAMED_LEASE"),
                        self.sources[JS] + "\nconst LEASE_MS = 900;\n"):
            sources = dict(self.sources, **{JS: altered})
            with self.assertRaisesRegex(AssertionError, "Steam lease: expected 1 timing literal"):
                check_ordering(sources)

    def test_installation_must_fit_the_parent_startup_budget(self):
        sources = dict(self.sources)
        sources[PY] = re.sub(r"(INSTALL_TIMEOUT\s*=\s*)" + NUMBER,
                             lambda match: match[1] + "4", sources[PY], flags=re.MULTILINE)
        with self.assertRaisesRegex(AssertionError, "bridge install < helper startup budget"):
            check_ordering(sources)

    def test_idle_inspection_cannot_outlive_request_acknowledgement(self):
        sources = dict(self.sources)
        sources[JS] = re.sub(r"(IDLE_INSPECT_MS\s*=\s*)" + NUMBER,
                             lambda match: match[1] + "400", sources[JS], flags=re.MULTILINE)
        with self.assertRaisesRegex(AssertionError, "idle inspection < request acknowledgement"):
            check_ordering(sources)

    def test_both_guard_deadlines_are_checked(self):
        sources = dict(self.sources)
        early_guard = extract_values(sources)["Steam lease"][0] / 2
        sources[GUARD] = re.sub(r"(deadline\s*=\s*sent\s*\+\s*)" + NUMBER,
                               lambda match: match[1] + str(early_guard), sources[GUARD],
                               count=1, flags=re.MULTILINE)
        with self.assertRaisesRegex(AssertionError, "Steam lease < native guard"):
            check_ordering(sources)

    def test_changed_expressions_cannot_be_partially_parsed(self):
        sources = dict(self.sources)
        sources[JS] = re.sub(r"(LEASE_MS\s*=\s*)" + NUMBER,
                             lambda match: match[1] + match[2] + " * 2", sources[JS], flags=re.MULTILINE)
        with self.assertRaisesRegex(AssertionError, "Steam lease: expected 1 timing literal"):
            check_ordering(sources)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
