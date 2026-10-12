#!/usr/bin/env python3
"""Parameter experiments preserve exact inputs, fair costs and failed evidence."""
import contextlib
import copy
import hashlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import cluster as c
import benchmark as b
import benchmark_mixed as mixed
import benchmark_ordering_windows as windows


class OrderingWindowBenchmark(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.binary = self.folder / "node"
        self.binary.write_bytes(b"mock node; never executed")
        self.output = self.folder / "results"
        self.seen = []

    def replay(self, config, shared, folder, method, **kwargs):
        cfg = c.validate(c.read(config))
        workload = c.read(shared)
        expected = mixed.expectations(cfg, workload, method)
        sha = hashlib.sha256(Path(shared).read_bytes()).hexdigest()
        self.seen.append(dict(method=method, config=cfg, path=Path(shared), sha=sha,
                              workload=workload, folder=Path(folder)))
        return dict(method=method, status="PASS", expected=expected,
                    client=dict(executed_transactions=expected["transactions"],
                        requests=expected["requests"], completed_requests=expected["requests"], elapsed_s=1,
                        workload=workload, timings=[dict(id="example", latency_s=.1)]),
                    client_result=str(Path(folder) / "client.json"),
                    config_fingerprint=b.config_fingerprint(cfg), workload_sha256=sha,
                    input_workload_sha256=sha, node_metrics=dict(bytes_sent=100),
                    completed_tps=200 if method == "arbor" else 100,
                    avg_latency_s=.1, p50_s=.05, p95_s=.2, p99_s=.3, failure_reasons=[])

    def main(self, args=(), replay=None):
        with mock.patch.object(c, "binary_for_method", return_value=self.binary), \
                mock.patch.object(mixed, "run", side_effect=replay or self.replay), \
                contextlib.redirect_stdout(io.StringIO()):
            default_matrix = [] if "--parameter-pairs" in args else ["--m-values", "1,2", "--empty-ms-values", "10"]
            return windows.main(["--skip-build", "--count", "100", *default_matrix,
                "--output-dir", str(self.output), *args])

    def test_one_shared_file_per_rate_seed_block_and_baselines_only_once(self):
        self.assertEqual(self.main(["--rates", "100,5000", "--repeat", "2"]), 0)
        self.assertEqual(len(self.seen), 20)
        grouped = {}
        for row in self.seen:
            grouped.setdefault(row["path"], []).append(row)
        self.assertEqual(len(grouped), 4)
        for rows in grouped.values():
            self.assertEqual([row["method"] for row in rows].count("arbor"), 2)
            self.assertEqual({row["method"] for row in rows}, {"arbor", "saguaro", "sharper", "ahl"})
            self.assertEqual(len({row["sha"] for row in rows}), 1)
            locality = rows[0]["workload"]["locality"]
            self.assertEqual(locality["cross_cluster_transactions"], 5)
            self.assertEqual(locality["participants_per_transaction"], {"2": 90, "3": 10})
            for row in rows:
                if row["method"] == "arbor":
                    self.assertIn(row["config"]["consensus"]["ordering_window_blocks"], (1, 2))
                    self.assertEqual(row["config"]["consensus"]["empty_block_interval_ms"], 10)
                else:
                    self.assertNotIn("ordering_window_blocks", row["config"]["consensus"])
                    self.assertNotIn("empty_block_interval_ms", row["config"]["consensus"])
                if row["method"] == "ahl":
                    self.assertEqual({leaf["parent"] for leaf in row["config"]["shards"]
                                      if leaf["parent"] is not None}, {7})
        self.assertEqual(len({row["folder"] for row in self.seen}), 20)
        report = c.read(self.output / "summary.json")
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(len(report["paired_comparisons"]), 24)
        self.assertTrue(all(pair["comparable"] and pair["arbor_over_baseline_tps"] == 2
                            for pair in report["paired_comparisons"]))
        self.assertEqual(len(report["binary_sha256"]), 4)
        self.assertIn("source/main.cpp", report["source_sha256"])
        self.assertTrue((self.output / "cases.csv").is_file())
        self.assertTrue((self.output / "summary.md").is_file())

    def test_only_arbor_specific_parameters_are_cost_exceptions(self):
        source = c.read(ROOT / "config/three_layer_locality.json")
        arbor = c.validate(windows.parameter_config(source, 4, 20))
        baseline = c.validate(windows.baseline_config(source))
        context = windows.validate_costs(arbor, baseline, "saguaro")
        self.assertNotEqual(context["actual_config_fingerprints"]["arbor"],
                            context["actual_config_fingerprints"]["saguaro"])
        self.assertEqual(context["protocol_difference_allowlist"],
                         ["consensus.ordering_window_blocks", "consensus.empty_block_interval_ms"])
        for section, field in (("consensus", "cross_shard_batch_size"),
                               ("consensus", "batch_wait_ms"), ("execution", "fib_iterations"),
                               ("network", "intra_shard_delay_ms")):
            changed = copy.deepcopy(baseline)
            changed[section][field] += 1
            with self.subTest(section=section, field=field), self.assertRaises(ValueError):
                windows.validate_costs(arbor, changed, "saguaro")

    def test_failed_repeat_suppresses_complete_aggregate_and_pair(self):
        def replay(*args, **kwargs):
            row = self.replay(*args, **kwargs)
            if row["method"] == "saguaro" and sum(item["method"] == "saguaro" for item in self.seen) == 1:
                row.update(status="FAIL", failure_reasons=["unfinished 2PC"])
                # The runner must scrub numeric metrics even if an upstream
                # failed result accidentally includes successful-looking TPS.
            return row
        self.assertEqual(self.main(["--repeat", "2"], replay=replay), 2)
        report = c.read(self.output / "summary.json")
        failed = next(row for row in report["cases"] if row["status"] == "FAIL")
        aggregate = next(row for row in report["aggregates"] if row["method"] == "saguaro")
        for metric in b.METRICS:
            self.assertIsNone(failed[metric])
            self.assertIsNone(aggregate["median_" + metric])
        failed_pairs = [row for row in report["paired_comparisons"] if row["baseline"] == "saguaro" and row["repeat"] == 1]
        self.assertTrue(all(not row["comparable"] for row in failed_pairs))
        self.assertTrue(all("arbor_over_baseline_tps" not in row for row in failed_pairs))

    def test_actual_workload_mismatch_prevents_comparison(self):
        def replay(*args, **kwargs):
            row = self.replay(*args, **kwargs)
            if row["method"] == "sharper":
                row["workload_sha256"] = "not the common input"
            return row
        self.assertEqual(self.main(replay=replay), 2)
        report = c.read(self.output / "summary.json")
        self.assertTrue(all(not pair["comparable"] for pair in report["paired_comparisons"] if pair["baseline"] == "sharper"))
        sharper = next(row for row in report["cases"] if row["method"] == "sharper")
        self.assertIsNone(sharper["completed_tps"])

    def test_confirmation_uses_independent_seeds_and_separate_count(self):
        self.assertEqual(self.main(["--confirm-top", "1", "--repeat", "2", "--confirm-count", "200"]), 0)
        report = c.read(self.output / "summary.json")
        self.assertEqual(len(report["selected_candidates"]), 1)
        screen = [row for row in report["cases"] if row["phase"] == "screen"]
        confirm = [row for row in report["cases"] if row["phase"] == "confirm"]
        self.assertEqual(len(screen), 5)
        self.assertEqual(len(confirm), 8)
        self.assertEqual({row["seed"] for row in screen}, {42})
        self.assertEqual({row["seed"] for row in confirm}, {1042, 1043})
        self.assertEqual({row["count"] for row in confirm}, {200})
        self.assertEqual(len({(row["m"], row["empty_ms"]) for row in confirm if row["method"] == "arbor"}), 1)

    def test_interruption_retains_partial_evidence_without_complete_estimates(self):
        def replay(*args, **kwargs):
            if self.seen:
                raise KeyboardInterrupt("simulated user interruption")
            return self.replay(*args, **kwargs)
        with self.assertRaises(KeyboardInterrupt):
            self.main(["--repeat", "3"], replay=replay)
        report = c.read(self.output / "summary.json")
        self.assertNotEqual(report["status"], "PASS")
        self.assertFalse(report["finished"])
        self.assertEqual(len(report["cases"]), 1)
        self.assertTrue(all(row["median_completed_tps"] is None for row in report["aggregates"]))

    def test_binary_change_during_replay_is_not_accepted(self):
        def replay(*args, **kwargs):
            row = self.replay(*args, **kwargs)
            self.binary.write_bytes(b"mutated binary")
            return row
        with self.assertRaises(ValueError):
            self.main(replay=replay)
        report = c.read(self.output / "summary.json")
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(report["cases"][0]["status"], "FAIL")
        self.assertIsNone(report["cases"][0]["completed_tps"])

    def test_invalid_rates_and_short_timeout_rejected_before_cluster(self):
        for args in (("--rates", "0,5000"), ("--m-values", "1,1"),
                     ("--rates", "nan"), ("--rates", "1", "--timeout", "10"),
                     ("--baseline-methods", "arbor")):
            with self.subTest(args=args), contextlib.redirect_stderr(io.StringIO()), self.assertRaises((ValueError, SystemExit)):
                self.main(args)
            self.assertFalse(self.output.exists())
            self.assertEqual(self.seen, [])

    def test_pareto_requires_tps_and_all_latency_dimensions(self):
        def point(m, tps, avg, p95, p99, status="PASS"):
            return dict(method="arbor", m=m, status=status, median_completed_tps=tps,
                        median_avg_latency_s=avg, median_p95_s=p95, median_p99_s=p99)
        rows = [point(1, 100, .2, .4, .5), point(2, 110, .1, .3, .4),
                point(4, 120, .2, .3, .4), point(8, 1000, 0, 0, 0, "FAIL")]
        self.assertEqual({row["m"] for row in windows.pareto_frontier(rows)}, {2, 4})

    def test_exact_pairs_do_not_generate_extra_cartesian_cases(self):
        self.assertEqual(self.main(["--parameter-pairs", "1:50,8:100", "--repeat", "2"]), 0)
        report = c.read(self.output / "summary.json")
        self.assertEqual(report["settings"]["parameter_pairs"], [[1, 50], [8, 100]])
        arbor = [row for row in report["cases"] if row["method"] == "arbor"]
        self.assertEqual(len(arbor), 4)
        self.assertEqual({(row["m"], row["empty_ms"]) for row in arbor}, {(1, 50), (8, 100)})

    def test_exact_pairs_exclude_explicit_cartesian_flags(self):
        for flag in (("--m-values", "1"), ("--empty-ms-values", "50")):
            with self.subTest(flag=flag), self.assertRaises(ValueError):
                self.main(["--parameter-pairs", "1:50", *flag])
            self.assertFalse(self.output.exists())
            self.assertEqual(self.seen, [])
        for text in ("1:50,1:50", "0:50", "1:0", "1", "1:50:2", "", "a:b"):
            with self.subTest(text=text), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                self.main(["--parameter-pairs", text])
            self.assertEqual(self.seen, [])

    def test_outer_summary_retains_only_scalar_client_metrics_and_evidence_path(self):
        self.assertEqual(self.main(), 0)
        report = c.read(self.output / "summary.json")
        for row in report["cases"]:
            self.assertNotIn("workload", row["client"])
            self.assertNotIn("timings", row["client"])
            self.assertEqual(row["client"]["executed_transactions"], 100)
            self.assertTrue(row["client_evidence"]["full_workload_and_transaction_timings_retained"])
            self.assertEqual(row["client_evidence"]["path"], row["client_result"])
        # Full input and timings are still available from the mocked producer
        # and the runner never deletes or rewrites per-case evidence files.
        self.assertTrue(all(row["workload"]["requests"] for row in self.seen))


if __name__ == "__main__":
    unittest.main(verbosity=2)
