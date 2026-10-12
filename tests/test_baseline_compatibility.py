#!/usr/bin/env python3
"""Legacy baseline configurations remain unchanged when Arbor opts into windows.

These are configuration and mocked-runner checks. They never build binaries,
open sockets, generate signing keys, or launch a cluster.
"""
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

OLD_FIELDS = {"batch_size", "batch_wait_ms", "cross_shard_batch_size",
              "cross_shard_batch_wait_ms", "view_timeout_ms", "checkpoint_batches"}
WINDOW_FIELDS = {"ordering_window_blocks", "empty_block_interval_ms"}


def old_minimal():
    return {"shards": [{"id": 1, "parent": 7}, {"id": 2, "parent": 7},
                       {"id": 7, "parent": None}]}


class LegacyBaselineCompatibility(unittest.TestCase):
    def test_legacy_defaults_and_fingerprint_match_pre_window_fixture(self):
        raw = old_minimal()
        before = copy.deepcopy(raw)
        actual = c.validate(raw)
        expected = dict(shards=old_minimal()["shards"], replicas_per_shard=4,
            host="127.0.0.1", base_port=19000,
            consensus=dict(batch_size=32, batch_wait_ms=10, cross_shard_batch_size=32,
                cross_shard_batch_wait_ms=200, view_timeout_ms=2000, checkpoint_batches=16),
            execution=dict(fib_iterations=10000),
            network=dict(intra_shard_delay_ms=1, default_inter_shard_delay_ms=20,
                shard_links=[], trace=False, resolved_links={}))
        self.assertEqual(raw, before, "normalization must not edit the caller's source config")
        self.assertEqual(actual, expected)
        self.assertEqual(set(actual["consensus"]), OLD_FIELDS)
        # Recorded from the immutable pre-window cluster.py normalization and
        # its benchmark.py fingerprint; no external checkout is needed to test.
        self.assertEqual(b.config_fingerprint(actual),
            "10c5486b8f62d853b001757c5c2a2ca9b164b0a7dadebe744b6bc061b3d9de78")
        for method in ("arbor", "saguaro", "sharper", "ahl"):
            with self.subTest(method=method):
                selected = c.validate_method_config(actual, method)
                self.assertEqual(selected, expected)
                self.assertEqual(b.config_fingerprint(selected), b.config_fingerprint(expected))

    def test_existing_configurations_retain_exact_legacy_consensus_fields(self):
        for filename in ("two_layer.json", "three_layer_locality.json", "ahl_two_layer_locality.json"):
            with self.subTest(config=filename):
                raw = c.read(ROOT / "config" / filename)
                before = copy.deepcopy(raw)
                normalized = c.validate(raw)
                self.assertEqual(raw, before)
                self.assertEqual(set(normalized["consensus"]), OLD_FIELDS)
                self.assertEqual(normalized["consensus"], before["consensus"])
                for method in ("saguaro", "sharper"):
                    self.assertEqual(c.validate_method_config(normalized, method), normalized)
                if filename == "ahl_two_layer_locality.json":
                    self.assertEqual(c.validate_method_config(normalized, "ahl"), normalized)

    def test_arbor_preserves_explicit_window_parameters_without_injecting_others(self):
        for opt_in in ({"ordering_window_blocks": 1, "empty_block_interval_ms": 200},
                       {"ordering_window_blocks": 1}, {"empty_block_interval_ms": 200}):
            with self.subTest(opt_in=opt_in):
                raw = old_minimal()
                raw["consensus"] = copy.deepcopy(opt_in)
                normalized = c.validate(raw)
                arbor = c.validate_method_config(normalized, "arbor")
                self.assertEqual(set(arbor["consensus"]), OLD_FIELDS | set(opt_in))
                self.assertEqual({key: arbor["consensus"][key] for key in opt_in}, opt_in)
                self.assertEqual(raw["consensus"], opt_in)
                self.assertNotEqual(b.config_fingerprint(arbor),
                    b.config_fingerprint(c.validate(old_minimal())))

    def test_baselines_strip_only_window_fields_and_do_not_mutate_arbor_config(self):
        raw = old_minimal()
        raw.update(consensus=dict(batch_size=128, batch_wait_ms=7, cross_shard_batch_size=64,
            cross_shard_batch_wait_ms=600, view_timeout_ms=3000, checkpoint_batches=8,
            ordering_window_blocks=1, empty_block_interval_ms=200),
            execution=dict(fib_iterations=37),
            network=dict(intra_shard_delay_ms=2, default_inter_shard_delay_ms=11,
                shard_links=[dict(shards=[1, 2], delay_ms=17)], trace=False))
        arbor = c.validate(raw)
        original = copy.deepcopy(arbor)
        expected = copy.deepcopy(arbor)
        for field in WINDOW_FIELDS:
            expected["consensus"].pop(field)
        for method in ("saguaro", "sharper", "ahl"):
            with self.subTest(method=method):
                baseline = c.validate_method_config(arbor, method)
                self.assertEqual(baseline, expected)
                self.assertEqual(arbor, original)
                self.assertIsNot(baseline, arbor)
                baseline["consensus"]["cross_shard_batch_size"] = 1
                baseline["network"]["shard_links"][0]["delay_ms"] = 999
                baseline["execution"]["fib_iterations"] = 1
                self.assertEqual(arbor, original, "baseline snapshots must not share mutable Arbor cost settings")

    def test_method_filtering_preserves_ahl_topology_constraint(self):
        raw = c.read(ROOT / "config/three_layer_locality.json")
        raw["consensus"].update(ordering_window_blocks=1, empty_block_interval_ms=200)
        arbor = c.validate(raw)
        before = copy.deepcopy(arbor)
        with self.assertRaises(ValueError):
            c.validate_method_config(arbor, "ahl")
        self.assertEqual(arbor, before)

    def test_baseline_snapshot_helper_preserves_raw_legacy_payload(self):
        raw = c.read(ROOT / "config/three_layer_locality.json")
        legacy = copy.deepcopy(raw)
        raw["consensus"].update(ordering_window_blocks=1, empty_block_interval_ms=200)
        before = copy.deepcopy(raw)
        baseline = windows.baseline_config(raw)
        self.assertEqual(baseline, legacy, "baseline source snapshots must contain their pre-window settings")
        self.assertEqual(raw, before)
        self.assertEqual(baseline["consensus"]["cross_shard_batch_size"],
                         legacy["consensus"]["cross_shard_batch_size"])
        self.assertEqual(b.config_fingerprint(c.validate(baseline)),
                         b.config_fingerprint(c.validate(legacy)))
        baseline["consensus"]["cross_shard_batch_size"] = 1
        self.assertEqual(raw, before)
        minimal = old_minimal()
        self.assertEqual(windows.baseline_config(minimal), minimal,
                         "preparation must not inject optional defaults into raw snapshots")

    def test_parameter_runner_keeps_old_baseline_snapshots_and_default_client_batch_ten(self):
        with tempfile.TemporaryDirectory(prefix="baseline-compatibility-") as directory:
            folder = Path(directory)
            binary = folder / "mock-node"
            binary.write_bytes(b"mock executable provenance; never executed")
            legacy = c.read(ROOT / "config/three_layer_locality.json")
            source = copy.deepcopy(legacy)
            source["consensus"].update(ordering_window_blocks=1, empty_block_interval_ms=200)
            source_path = folder / "arbor-with-windows.json"
            c.write(source_path, source)
            original_bytes = source_path.read_bytes()
            ahl_path = ROOT / "config/ahl_two_layer_locality.json"
            original_ahl = c.read(ahl_path)
            output, captured = folder / "result", []

            def replay(config, shared, result_folder, method, **kwargs):
                cfg = c.validate(c.read(config))
                workload = c.read(shared)
                expected = mixed.expectations(cfg, workload, method)
                sha = hashlib.sha256(Path(shared).read_bytes()).hexdigest()
                captured.append(dict(method=method, cfg=cfg, workload=workload, sha=sha))
                return dict(method=method, status="PASS", expected=expected,
                    client=dict(executed_transactions=expected["transactions"], requests=expected["requests"],
                        completed_requests=expected["requests"], elapsed_s=1, workload=workload,
                        timings=[dict(id="mock", latency_s=.1)]),
                    client_result=str(Path(result_folder) / "client.json"),
                    config_fingerprint=b.config_fingerprint(cfg), workload_sha256=sha,
                    input_workload_sha256=sha, node_metrics={}, completed_tps=100,
                    avg_latency_s=.1, p50_s=.05, p95_s=.2, p99_s=.3, failure_reasons=[])

            with mock.patch.object(c, "binary_for_method", return_value=binary), \
                    mock.patch.object(mixed, "run", side_effect=replay), \
                    contextlib.redirect_stdout(io.StringIO()):
                rc = windows.main(["--config", str(source_path), "--ahl-config", str(ahl_path),
                    "--parameter-pairs", "1:200", "--count", "100", "--rates", "1000",
                    "--repeat", "1", "--skip-build", "--output-dir", str(output)])
            self.assertEqual(rc, 0)
            self.assertEqual(source_path.read_bytes(), original_bytes)
            report = c.read(output / "summary.json")
            self.assertEqual(report["settings"]["batch"], 10)
            self.assertEqual(len(captured), 4)
            self.assertEqual(len({row["sha"] for row in captured}), 1)
            snapshots = output / "config-snapshots"
            self.assertEqual(c.read(snapshots / "saguaro.json"), legacy)
            self.assertEqual(c.read(snapshots / "sharper.json"), legacy)
            self.assertEqual(c.read(snapshots / "ahl.json"), original_ahl)
            self.assertEqual(c.read(snapshots / "arbor-m1-empty200.json"), source)
            for row in captured:
                self.assertLessEqual(max(len(request["txs"]) for request in row["workload"]["requests"]), 10)
                if row["method"] != "arbor":
                    self.assertFalse(WINDOW_FIELDS & set(row["cfg"]["consensus"]))


if __name__ == "__main__":
    unittest.main(verbosity=2)
