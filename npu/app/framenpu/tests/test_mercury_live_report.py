"""Live summaries count observations and requests separately; raw poses stay local."""

from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from mercury_live_report import summarize


class LiveReportTests(unittest.TestCase):
    def test_latency_cpu_time_and_request_count_are_not_double_counted(self):
        lines = [
            "F,1000000000,1020000000,left",
            "F,5000000000,5020000000,left",
            "F,5000000000,5020000000,right",
            "F,6000000000,6020000000,left",
        ]
        for i in range(2):
            fields = [
                "R",
                str(i),
                "left" if i == 0 else "right",
                "0",
                "5000000000",
                "5005000000",
                "5010000000",
                "5040000000",
                "5045000000",
                "16",
                "2",
                "2",
                "mono",
                "0",
                "0",
                "0",
                "1",
                "1",
                "framenpu",
                "2",
                "10",
            ]
            lines.append(",".join(fields))
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "private.csv"
            path.write_text("\n".join(lines))
            report = summarize(path, 3)
        self.assertEqual(report["observations"], 2)
        self.assertEqual(report["requests"], 1)
        self.assertEqual(report["backend_observations"], {"framenpu": 2})
        self.assertEqual(report["worker_wall_ms"]["median"], 30)
        self.assertEqual(report["worker_thread_cpu_ms_per_crop"]["median"], 8)
        self.assertEqual(report["capture_to_collect_ms"]["median"], 45)
        self.assertEqual(report["by_crop_count"]["2"]["dsp_body_ms"]["median"], 10)
        self.assertNotIn("left", str(report))  # no per-person pose/side records exported

    def test_stage_ages_split_capture_age_and_count_repeated_captures(self):
        lines = ["F,1000000000,1020000000,left", "F,9000000000,9020000000,left"]
        # (id, t_cap, t_sub, t0, t1, t_col, path, seq, t_read, t_ready)
        for i, (cap, path, seq) in enumerate(
            ((5000000000, "frame", 7), (5100000000, "early", 8), (5100000000, "early", 8))
        ):
            sub = cap + 30000000
            fields = [
                "R",
                str(i),
                "left",
                "0",
                str(cap),
                str(sub),
                str(sub + 1000000 + i),
                str(sub + 30000000 + i),
                str(sub + 33000000 + i),
                "16",
                "2",
                "1",
                "mono",
                "0",
                "0",
                "0",
                "1",
                "1",
                "framenpu",
                "1",
                "5",
                path,
                str(seq),
                str(cap + 20000000),
                str(cap + 25000000),
            ]
            lines.append(",".join(fields))
        lines.append("Z,6000000000,stall")
        lines.append(
            "R,9,left,0,5200000000,5210000000,5211000000,5230000000,5231000000,"
            "16,2,1,mono,0,0,0,1,1,cpu,1,0"
        )  # older log row: outer stages only
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "private.csv"
            path.write_text("\n".join(lines))
            report = summarize(path, 3)
        st = report["request_stage_ms"]
        self.assertEqual(st["capture_to_read"]["median"], 20)
        self.assertEqual(st["read_to_flow_done"]["median"], 5)
        self.assertEqual(st["flow_done_to_submit"]["median"], 5)
        self.assertEqual(st["submit_paths"], {"frame": 1, "early": 2})
        self.assertEqual(st["repeated_capture_requests"], 1)
        self.assertEqual(st["frame_age_at_submit"]["median"], 30)
        self.assertEqual(report["power_releases"], {"stall": 1})
        with tempfile.TemporaryDirectory() as tmp:  # sequence numbers restart per run
            runs = [Path(tmp) / "a.csv", Path(tmp) / "b.csv"]
            for run in runs:
                run.write_text("\n".join(lines[:3]))
            pooled = summarize(runs, 3)
        self.assertEqual(pooled["request_stage_ms"]["repeated_capture_requests"], 0)

    def test_no_observations_does_not_claim_success_or_zero_latency(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "private.csv"
            path.write_text("P,1000,profile\n")
            report = summarize(path)
        self.assertIn("error", report)
        self.assertNotIn("capture_to_collect_ms", report)


if __name__ == "__main__":
    unittest.main()
