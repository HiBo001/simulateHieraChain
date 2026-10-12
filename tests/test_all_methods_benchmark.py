#!/usr/bin/env python3
"""Performance reports must preserve fair inputs and reject partial evidence."""
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
import benchmark_all_methods as suite
import benchmark_mixed as mixed
import cluster as c


class AllMethodsBenchmark(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.folder = Path(temp.name)
        self.binary = self.folder / "node"
        self.binary.write_bytes(b"fixture binary, never executed")
        self.output = self.folder / "results"
        self.seen = []

    def replay(self, config, shared, output, method, **kwargs):
        cfg, workload = c.validate(c.read(config)), c.read(shared)
        expected = mixed.expectations(cfg, workload, method)
        sha = hashlib.sha256(Path(shared).read_bytes()).hexdigest()
        self.seen.append(dict(method=method, config=cfg, path=shared, sha=sha, workload=workload))
        return dict(method=method, status="PASS", expected=expected,
            client=dict(executed_transactions=expected["transactions"], requests=expected["requests"],
                        completed_requests=expected["requests"], elapsed_s=1),
            config_fingerprint=b.config_fingerprint(cfg), workload_sha256=sha,
            input_workload_sha256=sha, node_metrics=dict(bytes_sent=100),
            completed_tps=200 if method == "arbor" else 100, avg_latency_s=.1,
            p50_s=.05, p95_s=.2, p99_s=.3, failure_reasons=[])

    def run_suite(self, args=(), replay=None):
        with mock.patch.object(c, "binary_for_method", return_value=self.binary), \
                mock.patch.object(mixed, "run", side_effect=replay or self.replay), \
                contextlib.redirect_stdout(io.StringIO()):
            return suite.main(["--skip-build", "--output-dir", str(self.output), *args])

    def test_matrix_keeps_shared_inputs_and_original_baseline_parameters(self):
        self.assertEqual(self.run_suite(["--count", "100", "--rates", "100,1000",
            "--cross-cluster-ratios", "0,0.5,1", "--repeat", "2"]), 0)
        blocks = {}
        for row in self.seen:
            blocks.setdefault(row["path"], []).append(row)
        self.assertEqual(len(blocks), 12)
        self.assertEqual(len(self.seen), 48)
        for rows in blocks.values():
            self.assertEqual({row["method"] for row in rows}, set(suite.METHODS))
            self.assertEqual(len({row["sha"] for row in rows}), 1)
            self.assertEqual(len(rows), 4)
            self.assertIn(rows[0]["workload"]["locality"]["cross_cluster_transactions"], (0, 50, 100))
            for row in rows:
                if row["method"] == "arbor":
                    self.assertEqual(row["config"]["consensus"]["ordering_window_blocks"], 1)
                else:
                    self.assertNotIn("ordering_window_blocks", row["config"]["consensus"])
                    self.assertNotIn("empty_block_interval_ms", row["config"]["consensus"])
        report = c.read(self.output / "summary.json")
        self.assertTrue(all(row["median_arbor_over_baseline_tps"] == 2 for row in report["paired_aggregates"]))
        self.assertEqual({row["seed"] for row in report["cases"]}, {42, 43})

    def test_legacy_mode_does_not_inject_window_fields(self):
        self.assertEqual(self.run_suite(["--window-blocks", "0"]), 0)
        self.assertTrue(all("ordering_window_blocks" not in row["config"]["consensus"] for row in self.seen))

    def test_failed_repeat_suppresses_entire_method_group_and_paired_ratio(self):
        def replay(*args, **kwargs):
            row = self.replay(*args, **kwargs)
            if row["method"] == "saguaro" and len(self.seen) < 5:
                row.update(status="FAIL", failure_reasons=["unfinished 2PC"])
            return row
        self.assertEqual(self.run_suite(["--repeat", "2"], replay), 2)
        report = c.read(self.output / "summary.json")
        failed = next(row for row in report["aggregates"] if row["method"] == "saguaro")
        self.assertIsNone(failed["median_completed_tps"])
        pair = next(row for row in report["paired_aggregates"] if row["baseline"] == "saguaro")
        self.assertFalse(pair["comparable"])
        self.assertIsNone(pair["median_arbor_over_baseline_tps"])
        self.assertEqual(report["status"], "FAIL")

    def test_interruption_keeps_partial_evidence_without_complete_estimates(self):
        def replay(*args, **kwargs):
            if self.seen:
                raise KeyboardInterrupt()
            return self.replay(*args, **kwargs)
        with self.assertRaises(KeyboardInterrupt):
            self.run_suite(["--repeat", "2"], replay)
        report = c.read(self.output / "summary.json")
        self.assertEqual(len(report["cases"]), 1)
        self.assertNotEqual(report["status"], "PASS")
        self.assertTrue(all(row["median_completed_tps"] is None for row in report["aggregates"]))

    def test_dry_run_and_bad_timeout_never_start_nodes_or_create_results(self):
        self.assertEqual(self.run_suite(["--dry-run", "--profile", "full", "--count", "100"]), 0)
        self.assertFalse(self.seen)
        self.assertFalse(self.output.exists())
        with self.assertRaises(ValueError):
            self.run_suite(["--count", "10000", "--rates", "100", "--timeout", "100"])
        self.assertFalse(self.seen)
        self.assertFalse(self.output.exists())

    def test_actual_input_mismatch_is_a_failure(self):
        def replay(*args, **kwargs):
            row = self.replay(*args, **kwargs)
            if row["method"] == "sharper":
                row["workload_sha256"] = "wrong"
            return row
        self.assertEqual(self.run_suite([], replay), 2)
        report = c.read(self.output / "summary.json")
        self.assertIsNone(next(row for row in report["aggregates"] if row["method"] == "sharper")["median_completed_tps"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
