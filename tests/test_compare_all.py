#!/usr/bin/env python3
"""Four-method comparison uses one immutable workload and strict accounting."""
import contextlib
import copy
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT / "baseline"))
import cluster as c
import benchmark as b
import benchmark_mixed as mixed
import compare_all
import compare_mixed

ARBOR_CONFIG = ROOT / "config/three_layer_locality.json"
AHL_CONFIG = ROOT / "config/ahl_two_layer_locality.json"
METHODS = {"arbor", "saguaro", "sharper", "ahl"}


class FourMethodComparison(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.binary = self.folder / "node"
        self.binary.write_bytes(b"mock executable, never launched")
        self.output = self.folder / "results"
        self.seen = []

    def replay(self, config, shared, folder, method, **kwargs):
        cfg, workload = c.validate(c.read(config)), c.read(shared)
        digest = hashlib.sha256(Path(shared).read_bytes()).hexdigest()
        expected = mixed.expectations(cfg, workload, method)
        self.seen.append(dict(method=method, config=cfg, path=Path(shared), sha=digest,
                              workload=workload, folder=Path(folder), expected=expected))
        return dict(method=method, status="PASS", expected=expected,
                    client=dict(executed_transactions=expected["transactions"],
                                requests=expected["requests"], completed_requests=expected["requests"],
                                elapsed_s=1), config_fingerprint=b.config_fingerprint(cfg),
                    workload_sha256=digest, input_workload_sha256=digest,
                    node_metrics=dict(bytes_sent=100, network_failures=0),
                    completed_tps=200 if method == "arbor" else 100,
                    avg_latency_s=.1, p50_s=.05, p95_s=.2, p99_s=.3, failure_reasons=[])

    def main(self, args=(), replay=None):
        with mock.patch.object(c, "binary_for_method", return_value=self.binary), \
                mock.patch.object(mixed, "run", side_effect=replay or self.replay), \
                contextlib.redirect_stdout(io.StringIO()):
            return compare_all.main(["--skip-build", "--output-dir", str(self.output), *args])

    def test_all_methods_replay_one_file_and_arbor_reference_clusters(self):
        rc = self.main(["--count", "100", "--rate", "1000", "--batch", "10", "--repeat", "2"])
        self.assertEqual(rc, 0)
        self.assertEqual(len(self.seen), 8)
        for offset in (0, 4):
            self.assertEqual({row["method"] for row in self.seen[offset:offset + 4]}, METHODS)
        self.assertNotEqual([row["method"] for row in self.seen[:4]],
                            [row["method"] for row in self.seen[4:]])
        self.assertEqual(len({row["path"] for row in self.seen}), 1)
        self.assertEqual(len({row["sha"] for row in self.seen}), 1)
        workload = self.seen[0]["workload"]
        self.assertEqual(workload["locality"]["clusters"], {"5": [1, 2, 8], "6": [3, 4, 9]})
        for row in self.seen:
            self.assertEqual(row["workload"], workload)
            expected = row["expected"]
            self.assertEqual(expected["participants_per_transaction"], {"2": 90, "3": 10})
            self.assertEqual(expected["locality"]["cross_cluster_transactions"], 5)
            self.assertEqual(expected["locality"]["intra_cluster_transactions"], 95)
            if row["method"] == "ahl":
                self.assertEqual({leaf["parent"] for leaf in row["config"]["shards"]
                                  if leaf["parent"] is not None}, {7})
                self.assertEqual(expected["ordered"][7], 100)
            elif row["method"] in ("arbor", "saguaro"):
                self.assertEqual(expected["ordered"][7], 5)
                self.assertEqual(expected["ordered"][5] + expected["ordered"][6], 95)
            else:
                self.assertEqual(sum(expected["ordered"].values()), 0)
        report = c.read(self.output / "summary.json")
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(report["locality"]["cross_cluster_transactions"], 5)
        self.assertEqual(len(report["paired_aggregates"]), 3)
        for item in report["paired_aggregates"]:
            self.assertTrue(item["comparable"])
            self.assertEqual(item["arbor_over_" + item["baseline"] + "_tps"], 2)

    def test_leaf_ids_and_costs_remain_strictly_validated(self):
        source = c.read(AHL_CONFIG)
        mutations = ("leaf", "consensus", "execution", "intra_delay", "common_link")
        for index, mutation in enumerate(mutations):
            changed = copy.deepcopy(source)
            if mutation == "leaf":
                changed["shards"] = [row for row in changed["shards"] if row["id"] != 9]
                changed["network"]["shard_links"] = [row for row in changed["network"]["shard_links"]
                                                           if 9 not in row["shards"]]
            elif mutation in ("consensus", "execution"):
                field = "cross_shard_batch_size" if mutation == "consensus" else "fib_iterations"
                changed[mutation][field] += 1
            elif mutation == "intra_delay":
                changed["network"]["intra_shard_delay_ms"] += 1
            else:
                changed["network"]["shard_links"][0]["delay_ms"] += 1
            config = self.folder / f"bad-config-{index}.json"
            c.write(config, changed)
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                self.main(["--ahl-config", str(config), "--count", "100", "--repeat", "1"])
            self.assertEqual(self.seen, [])
            self.assertFalse(self.output.exists(), "validation must precede creation of results")

    def test_external_workload_is_preserved_and_all_four_methods_share_its_copy(self):
        cfg = c.validate(c.read(ARBOR_CONFIG))
        workload = compare_mixed.prepare_mixed_workload(cfg, 100, 1000, 10, 42, 30)
        original = json.dumps(workload, separators=(",", ":")).encode()
        source = self.folder / "source.json"
        source.write_bytes(original)
        self.assertEqual(self.main(["--workload", str(source), "--repeat", "1"]), 0)
        self.assertEqual(source.read_bytes(), original)
        self.assertEqual(len(self.seen), 4)
        self.assertEqual(len({row["path"] for row in self.seen}), 1)
        self.assertEqual(len({row["sha"] for row in self.seen}), 1)
        for row in self.seen:
            self.assertEqual(row["workload"], workload)
        report = c.read(self.output / "summary.json")
        self.assertEqual(report["input_workload_sha256"], hashlib.sha256(original).hexdigest())

    def test_external_workload_conflicts_with_every_generation_flag_before_launch(self):
        flags = (["--count", "100"], ["--rate", "1000"], ["--batch", "10"], ["--seed", "42"],
                 ["--uniform"], ["--cross-cluster-ratio", ".05"], ["--three-shard-ratio", ".10"])
        for flag in flags:
            with self.subTest(flag=flag), self.assertRaises((ValueError, SystemExit)):
                self.main(["--workload", str(self.folder / "does-not-exist.json"), *flag])
            self.assertEqual(self.seen, [])
            self.assertFalse(self.output.exists())

    def test_uniform_and_locality_ratio_are_mutually_exclusive(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises((ValueError, SystemExit)):
            self.main(["--uniform", "--cross-cluster-ratio", ".05", "--repeat", "1"])
        self.assertEqual(self.seen, [])
        self.assertFalse(self.output.exists())

    def test_failed_repeat_suppresses_entire_method_aggregate_and_pair_ratio(self):
        failed = []
        def replay(*args, **kwargs):
            result = self.replay(*args, **kwargs)
            if result["method"] == "saguaro" and not failed:
                failed.append(True)
                result.update(status="FAIL", failure_reasons=["2PC locks did not drain"])
                result.update({metric: None for metric in b.METRICS})
            return result
        self.assertEqual(self.main(["--count", "100", "--repeat", "2"], replay=replay), 2)
        report = c.read(self.output / "summary.json")
        self.assertEqual(report["status"], "FAIL")
        cases = [row for row in report["cases"] if row["method"] == "saguaro"]
        self.assertEqual(len(cases), 2)
        for metric in b.METRICS:
            self.assertIsNone(next(row for row in cases if row["status"] == "FAIL")[metric])
        aggregate = next(row for row in report["aggregates"] if row["method"] == "saguaro")
        for metric in b.METRICS:
            self.assertIsNone(aggregate["median_" + metric])
        pair = next(row for row in report["paired_aggregates"] if row["baseline"] == "saguaro")
        self.assertFalse(pair["comparable"])
        self.assertNotIn("arbor_over_saguaro_tps", pair)

    def test_actual_replay_hash_mismatch_cannot_produce_ratio(self):
        def replay(*args, **kwargs):
            result = self.replay(*args, **kwargs)
            if result["method"] == "saguaro":
                result["workload_sha256"] = "different replay bytes"
            return result
        self.assertEqual(self.main(["--count", "100", "--repeat", "1"], replay=replay), 2)
        report = c.read(self.output / "summary.json")
        pair = next(row for row in report["paired_aggregates"] if row["baseline"] == "saguaro")
        self.assertFalse(pair["comparable"])
        self.assertNotIn("arbor_over_saguaro_tps", pair)

    def test_partial_run_keeps_metrics_out_of_complete_aggregates(self):
        def replay(*args, **kwargs):
            if self.seen:
                raise KeyboardInterrupt("simulated cancellation after one case")
            return self.replay(*args, **kwargs)
        with self.assertRaises(KeyboardInterrupt):
            self.main(["--count", "100", "--repeat", "3"], replay=replay)
        report = c.read(self.output / "summary.json")
        self.assertNotEqual(report["status"], "PASS")
        for row in report["aggregates"]:
            for metric in b.METRICS:
                self.assertIsNone(row["median_" + metric])


if __name__ == "__main__":
    unittest.main(verbosity=2)
