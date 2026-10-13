#!/usr/bin/env python3
"""Fair pairing, missing repeats and interrupted performance evidence."""
import contextlib
import hashlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import benchmark as b
import benchmark_mixed as mixed
import benchmark_streaming as suite
import cluster as c


class StreamingBenchmark(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory();self.addCleanup(temp.cleanup)
        self.folder = Path(temp.name);self.output = self.folder / "report"
        self.binary = self.folder / "node";self.binary.write_bytes(b"fixture binary")
        self.seen = []

    def replay(self, config, shared, folder, method, **kwargs):
        cfg, workload = c.validate(c.read(config)), c.read(shared)
        sha = hashlib.sha256(Path(shared).read_bytes()).hexdigest()
        expected = mixed.expectations(cfg, workload)
        strategy = "streaming" if cfg["consensus"]["ancestor_streaming"] else "barrier"
        self.seen.append(dict(cfg=cfg, sha=sha, shared=shared, strategy=strategy))
        return dict(method=method, status="PASS", expected=expected,
            config_fingerprint=b.config_fingerprint(cfg), workload_sha256=sha, input_workload_sha256=sha,
            client=dict(executed_transactions=expected["transactions"], requests=expected["requests"],
                        completed_requests=expected["requests"], elapsed_s=1),
            node_metrics={}, completed_tps=200 if strategy == "streaming" else 100,
            avg_latency_s=.1, p50_s=.05, p95_s=.2, p99_s=.3, failure_reasons=[])

    def run_suite(self, args=(), replay=None):
        with mock.patch.object(c, "binary_for_method", return_value=self.binary), \
                mock.patch.object(mixed, "run", side_effect=replay or self.replay), \
                contextlib.redirect_stdout(io.StringIO()):
            return suite.main(["--skip-build", "--count", "100", "--output-dir", str(self.output), *args])

    def test_shared_input_and_only_strategy_difference(self):
        self.assertEqual(self.run_suite(["--repeat", "2"]), 0)
        groups = {}
        for row in self.seen: groups.setdefault(row["shared"], []).append(row)
        self.assertEqual(len(groups), 4)
        for rows in groups.values():
            self.assertEqual(len(rows), 2);self.assertEqual(len({row["sha"] for row in rows}), 1)
            configs = []
            for row in rows:
                row["cfg"]["consensus"].pop("ancestor_streaming");configs.append(row["cfg"])
            self.assertEqual(configs[0], configs[1])
        report = c.read(self.output / "summary.json")
        self.assertTrue(all(row["median_streaming_over_barrier_tps"] == 2 for row in report["paired_aggregates"]))

    def test_failed_repeat_removes_numeric_aggregate_and_pair(self):
        def replay(*args, **kwargs):
            row = self.replay(*args, **kwargs)
            if self.seen[-1]["strategy"] == "streaming" and len(self.seen) < 3:
                row.update(status="FAIL", failure_reasons=["unfinished execution"])
            return row
        self.assertEqual(self.run_suite(["--repeat", "2", "--empty-ms-values", "50"], replay), 2)
        report = c.read(self.output / "summary.json")
        self.assertIsNone(next(row for row in report["aggregates"] if row["strategy"] == "streaming")["median_completed_tps"])
        self.assertIsNone(report["paired_aggregates"][0]["median_streaming_over_barrier_tps"])

    def test_interruption_preserves_only_partial_evidence(self):
        def replay(*args, **kwargs):
            if self.seen: raise KeyboardInterrupt()
            return self.replay(*args, **kwargs)
        with self.assertRaises(KeyboardInterrupt): self.run_suite([], replay)
        report = c.read(self.output / "summary.json")
        self.assertEqual(len(report["cases"]), 1)
        self.assertTrue(all(row["median_completed_tps"] is None for row in report["aggregates"]))
        self.assertNotEqual(report["status"], "PASS")


if __name__ == "__main__": unittest.main(verbosity=2)
