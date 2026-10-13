#!/usr/bin/env python3
"""Bounded single groups, fail-fast evidence and applied ancestor order auditing."""
import contextlib
import copy
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import benchmark as b
import benchmark_mixed as mixed
import benchmark_quick as suite
import cluster as c


class QuickBenchmark(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory();self.addCleanup(temp.cleanup)
        self.folder = Path(temp.name);self.output = self.folder / "results"
        self.binary = self.folder / "node";self.binary.write_bytes(b"mock executable")
        self.seen = []

    def replay(self, config, shared, output, method, **kwargs):
        cfg, workload = c.validate(c.read(config)), c.read(shared)
        expected, sha = mixed.expectations(cfg, workload, method), suite.w.digest(shared)
        self.seen.append(dict(method=method, cfg=cfg, shared=Path(shared), sha=sha, output=Path(output),
                              guard=kwargs["guard"]))
        return dict(method=method, status="PASS", valid_performance_sample=True, expected=expected,
            config_fingerprint=b.config_fingerprint(cfg), input_workload_sha256=sha, workload_sha256=sha,
            client=dict(executed_transactions=expected["transactions"], elapsed_s=1, workload=workload,
                        timings=[dict(id="fixture", latency_s=.1)], completed_tps=200),
            client_result=str(Path(output) / "client.json"), run_dir=str(Path(output) / "run"),
            node_metrics={}, completed_tps=200, avg_latency_s=.1, p50_s=.05, p95_s=.2, p99_s=.3,
            failure_reasons=[], leaf_profile_delta=[dict(shard=1, replica=0, wait_root_ns=23)])

    def run_suite(self, args=(), replay=None, output=None):
        with mock.patch.object(c, "binary_for_method", return_value=self.binary), \
                mock.patch.object(suite.w, "source_hashes", return_value={"source/main.cpp": "fixture-source"}), \
                mock.patch.object(mixed, "run", side_effect=replay or self.replay), \
                mock.patch.object(suite, "audit_leaf_order", return_value=dict(status="PASS")), \
                contextlib.redirect_stdout(io.StringIO()):
            return suite.main(["--skip-build", "--output-dir", str(output or self.output), *args])

    def test_default_is_one_shared_group_per_phase_with_fresh_state(self):
        self.assertEqual(self.run_suite(), 0)
        self.assertEqual(suite.DEFAULT_CASES, ("lower_first", "saguaro", "sharper", "ahl"))
        self.assertEqual(len(self.seen), 8)
        groups = {}
        for row in self.seen:
            groups.setdefault(row["shared"], []).append(row)
            self.assertEqual(row["cfg"]["consensus"]["view_timeout_ms"], 2000)
            self.assertEqual(row["cfg"]["consensus"]["cross_shard_batch_wait_ms"], 100)
            self.assertEqual(row["guard"].no_progress_seconds, 15)
            self.assertLessEqual(row["guard"].deadline - row["guard"].started, 60)
        self.assertEqual(len(groups), 2)
        original = c.validate(c.read(ROOT / "config/three_layer_locality.json"))
        for rows in groups.values():
            self.assertEqual(len(rows), 4)
            self.assertEqual(len({row["sha"] for row in rows}), 1)
            arbor = [row["cfg"] for row in rows if row["method"] == "arbor"]
            self.assertEqual(len(arbor), 1)
            self.assertEqual(arbor[0]["consensus"]["ordering_window_blocks"], 1)
            self.assertIs(arbor[0]["consensus"]["ancestor_streaming"], True)
            self.assertEqual(arbor[0]["consensus"]["ancestor_order"], "lower_first")
            self.assertNotIn("ancestor_lower_blocks_per_root", arbor[0]["consensus"])
            self.assertNotIn("ancestor_leaf_batch_blocks", arbor[0]["consensus"])
            for row in rows:
                if row["method"] != "arbor":
                    self.assertEqual(len(c.ARBOR_WINDOW_FIELDS), 6)
                    self.assertTrue(all(field not in row["cfg"]["consensus"] for field in c.ARBOR_WINDOW_FIELDS))
                if row["method"] in ("saguaro", "sharper"):
                    self.assertEqual(row["cfg"], original)
        self.assertEqual([row["method"] for row in self.seen[4:]], ["saguaro", "sharper", "ahl", "arbor"])
        self.assertEqual(len({row["output"] for row in self.seen}), 8)
        report = c.read(self.output / "summary.json")
        self.assertEqual(report["schema_version"], 2)
        self.assertEqual(report["settings"]["cases"], list(suite.DEFAULT_CASES))
        self.assertEqual(report["status"], "PASS")
        self.assertEqual([row["count"] for row in report["cases"]], [200] * 4 + [1000] * 4)
        self.assertEqual([row["case"] for row in report["cases"][4:]], ["saguaro", "sharper", "ahl", "lower_first"])
        self.assertEqual(report["cases"][0]["leaf_profile_delta"][0]["wait_root_ns"], 23)
        self.assertNotIn("workload", report["cases"][0]["client"])
        self.assertTrue((self.output / "summary.csv").is_file())
        for costs in report["costs"].values():
            self.assertNotIn("consensus.cross_shard_batch_wait_ms", costs["protocol_difference_allowlist"])

    def test_batch_wait_override_is_confined_to_arbor_and_records_actual_comparison(self):
        self.assertEqual(self.run_suite(["--phase", "correctness", "--cases", *suite.CASES,
            "--arbor-cross-shard-batch-wait-ms", "50"]), 0)
        self.assertEqual(len(self.seen), len(suite.CASES))
        for row in self.seen:
            self.assertEqual(row["cfg"]["consensus"]["cross_shard_batch_wait_ms"],
                             50 if row["method"] == "arbor" else 100)
            self.assertEqual(row["cfg"]["consensus"]["cross_shard_batch_size"], 100)
            self.assertEqual(row["cfg"]["consensus"]["view_timeout_ms"], 2000)
        report = c.read(self.output / "summary.json")
        actual_barrier = self.seen[0]["cfg"]
        original_barrier = copy.deepcopy(actual_barrier)
        original_barrier["consensus"]["cross_shard_batch_wait_ms"] = 100
        expected_allowlist = ["consensus." + field for field in suite.w.WINDOW_FIELDS]
        expected_allowlist.append("consensus.cross_shard_batch_wait_ms")
        for method, costs in report["costs"].items():
            with self.subTest(method=method):
                self.assertEqual(costs["protocol_difference_allowlist"], expected_allowlist)
                self.assertEqual(costs["arbor_batch_wait_override"],
                                 dict(configured_ms=100, effective_ms=50, baseline_ms=100))
                self.assertEqual(costs["actual_arbor_cross_shard_batch_wait_ms"], 50)
                self.assertEqual(costs["baseline_cross_shard_batch_wait_ms"], 100)
                self.assertEqual(costs["actual_config_fingerprints"]["arbor"],
                                 b.config_fingerprint(actual_barrier))
                self.assertNotEqual(costs["actual_config_fingerprints"]["arbor"],
                                    b.config_fingerprint(original_barrier))
                baseline = next(row["cfg"] for row in self.seen if row["method"] == method)
                self.assertEqual(costs["actual_config_fingerprints"][method],
                                 b.config_fingerprint(baseline))
                self.assertEqual(costs["shared_cost_validation"],
                    json.loads(json.dumps(suite.w.validate_costs(original_barrier, baseline, method)
                                          ["shared_cost_validation"])))

    def test_equal_batch_wait_override_does_not_claim_a_protocol_difference(self):
        self.assertEqual(self.run_suite(["--phase", "correctness",
            "--arbor-cross-shard-batch-wait-ms", "100"]), 0)
        self.assertTrue(all(row["cfg"]["consensus"]["cross_shard_batch_wait_ms"] == 100
                            for row in self.seen))
        for costs in c.read(self.output / "summary.json")["costs"].values():
            self.assertNotIn("consensus.cross_shard_batch_wait_ms", costs["protocol_difference_allowlist"])

    def test_batch_wait_override_rejects_out_of_range_and_non_integer_values_before_run(self):
        parse_args = suite.argparse.ArgumentParser.parse_args
        for invalid in (-1, 10001, True, 100.0, "100"):
            with self.subTest(value=invalid):
                def mutated_args(parser, *args, **kwargs):
                    parsed = parse_args(parser, *args, **kwargs)
                    parsed.arbor_cross_shard_batch_wait_ms = invalid
                    return parsed
                with mock.patch.object(suite.argparse.ArgumentParser, "parse_args", new=mutated_args):
                    with self.assertRaises(ValueError):
                        self.run_suite()
                self.assertEqual(self.seen, [])
        self.assertFalse(self.output.exists())

    def test_changed_batch_wait_requires_fresh_correctness_evidence(self):
        self.assertEqual(self.run_suite(["--phase", "correctness", "--cases", "lower_first"]), 0)
        self.seen.clear()
        with self.assertRaisesRegex(ValueError, "config_sha256"):
            self.run_suite(["--phase", "performance", "--cases", "lower_first",
                "--arbor-cross-shard-batch-wait-ms", "50", "--correctness-report",
                str(self.output / "summary.json")], output=self.folder / "performance")
        self.assertEqual(self.seen, [])

    def test_uniform_profiles_preserve_legacy_configuration_and_distinct_fingerprints(self):
        for name in ("three_layer_lower_first", "three_layer_locality", "ahl_two_layer_locality"):
            with self.subTest(profile=name):
                current_path = ROOT / "config" / (name + ".json")
                legacy_path = ROOT / "config" / (name + "_wait600.json")
                current_bytes, legacy_bytes = current_path.read_bytes(), legacy_path.read_bytes()
                current_raw, legacy_raw = c.read(current_path), c.read(legacy_path)
                current, legacy = c.validate(current_raw), c.validate(legacy_raw)
                self.assertEqual(current["consensus"]["cross_shard_batch_wait_ms"], 100)
                self.assertEqual(legacy["consensus"]["cross_shard_batch_wait_ms"], 600)
                self.assertNotEqual(b.config_fingerprint(current), b.config_fingerprint(legacy))
                expected = copy.deepcopy(legacy)
                expected["consensus"]["cross_shard_batch_wait_ms"] = 100
                self.assertEqual(current, expected)
                for config in (current_raw, legacy_raw):
                    baseline = suite.w.baseline_config(config)
                    self.assertEqual(baseline["consensus"]["cross_shard_batch_wait_ms"],
                                     config["consensus"]["cross_shard_batch_wait_ms"])
                self.assertEqual(current_path.read_bytes(), current_bytes)
                self.assertEqual(legacy_path.read_bytes(), legacy_bytes)

    def test_invalid_case_stops_remaining_cases_and_blanks_partial_tps(self):
        def replay(*args, **kwargs):
            result = self.replay(*args, **kwargs)
            if len(self.seen) == 5:
                result.update(status="INVALID", valid_performance_sample=False,
                              failure_reasons=["unexpected_view_change"], guard_evidence=dict(view=1))
            return result
        self.assertEqual(self.run_suite(replay=replay), 2)
        self.assertEqual(len(self.seen), 5)
        report = c.read(self.output / "summary.json")
        self.assertEqual(report["status"], "INVALID")
        invalid = report["cases"][4]
        self.assertEqual(invalid["guard_evidence"], dict(view=1))
        self.assertTrue(all(invalid[metric] is None for metric in b.METRICS))
        self.assertNotIn("completed_tps", invalid["client"])
        self.assertTrue(all(row["status"] == "NOT_RUN" for row in report["cases"][5:]))

    def test_modified_workload_is_invalid_without_retry(self):
        def replay(config, shared, *args, **kwargs):
            result = self.replay(config, shared, *args, **kwargs)
            shared.write_text(shared.read_text() + "\n")
            return result
        self.assertEqual(self.run_suite(replay=replay), 2)
        self.assertEqual(len(self.seen), 1)
        report = c.read(self.output / "summary.json")
        self.assertIsNone(report["cases"][0]["completed_tps"])
        self.assertIn("actual replay differs", report["cases"][0]["failure_reasons"][0])

    def test_matching_correctness_can_be_reused_without_state_reuse(self):
        self.assertEqual(self.run_suite(["--phase", "correctness"]), 0)
        self.seen.clear()
        output = self.folder / "performance"
        self.assertEqual(self.run_suite(["--phase", "performance", "--correctness-report",
                                        str(self.output / "summary.json")], output=output), 0)
        self.assertEqual(len(self.seen), 4)
        self.assertTrue(all(row["output"].is_relative_to(output.resolve()) for row in self.seen))
        report = c.read(output / "summary.json")
        self.assertIn("reused_correctness", report)

    def test_selected_subset_keeps_baselines_first_and_reuses_matching_evidence(self):
        self.assertEqual(self.run_suite(["--phase", "correctness", "--cases", "lower_first", "ahl"]), 0)
        self.seen.clear()
        output = self.folder / "performance-subset"
        self.assertEqual(self.run_suite(["--phase", "performance", "--cases", "lower_first", "ahl",
            "--correctness-report", str(self.output / "summary.json")], output=output), 0)
        report = c.read(output / "summary.json")
        self.assertEqual([row["case"] for row in report["cases"]], ["ahl", "lower_first"])
        self.assertEqual([row["method"] for row in self.seen], ["ahl", "arbor"])
        self.assertEqual(report["settings"]["cases"], ["lower_first", "ahl"])
        self.assertEqual(len(report["reused_correctness"]["cases"]), 2)

    def test_all_cases_remain_explicitly_available_without_extra_groups(self):
        self.assertEqual(self.run_suite(["--cases", *suite.CASES]), 0)
        self.assertEqual(len(self.seen), 18)
        for phase in ("correctness", "performance"):
            rows = [row for row in self.seen if row["output"].parent.name == phase]
            self.assertEqual(len(rows), 9)
            self.assertEqual(len({row["sha"] for row in rows}), 1)
            arbor = [copy.deepcopy(row["cfg"]) for row in rows if row["method"] == "arbor"]
            self.assertEqual([cfg["consensus"].pop("ancestor_streaming") for cfg in arbor],
                             [False, True, True, True, True, True])
            self.assertEqual(arbor[2]["consensus"].pop("ancestor_order"), "lower_first")
            self.assertEqual(arbor[3]["consensus"].pop("ancestor_order"), "lower_first")
            self.assertEqual(arbor[4]["consensus"].pop("ancestor_order"), "lower_first")
            self.assertEqual(arbor[5]["consensus"].pop("ancestor_order"), "lower_first")
            self.assertEqual(arbor[3]["consensus"].pop("ancestor_lower_blocks_per_root"), 2)
            self.assertEqual(arbor[4]["consensus"].pop("ancestor_lower_blocks_per_root"), 10)
            self.assertEqual(arbor[5]["consensus"].pop("ancestor_lower_blocks_per_root"), 2)
            self.assertEqual(arbor[5]["consensus"].pop("ancestor_leaf_batch_blocks"), 2)
            self.assertTrue(all(cfg == arbor[0] for cfg in arbor))

    def test_weighted_case_needs_its_own_matching_correctness_evidence(self):
        self.assertEqual(self.run_suite(["--phase", "correctness", "--cases", "lower_first"]), 0)
        self.seen.clear()
        with self.assertRaisesRegex(ValueError, "config_sha256"):
            self.run_suite(["--phase", "performance", "--cases", "lower_10",
                            "--correctness-report", str(self.output / "summary.json")],
                           output=self.folder / "performance")
        self.assertEqual(self.seen, [])

    def test_ten_to_one_correctness_cannot_authorize_untested_two_to_one(self):
        self.assertEqual(self.run_suite(["--phase", "correctness", "--cases", "lower_10"]), 0)
        self.seen.clear()
        with self.assertRaisesRegex(ValueError, "config_sha256"):
            self.run_suite(["--phase", "performance", "--cases", "lower_2",
                            "--correctness-report", str(self.output / "summary.json")],
                           output=self.folder / "performance")
        self.assertEqual(self.seen, [])

    def test_separate_two_to_one_correctness_cannot_authorize_bundle_submission(self):
        self.assertEqual(self.run_suite(["--phase", "correctness", "--cases", "lower_2"]), 0)
        self.seen.clear()
        with self.assertRaisesRegex(ValueError, "config_sha256"):
            self.run_suite(["--phase", "performance", "--cases", "lower_bundle",
                            "--correctness-report", str(self.output / "summary.json")],
                           output=self.folder / "performance")
        self.assertEqual(self.seen, [])

    def test_duplicate_case_selection_is_rejected_without_run(self):
        with self.assertRaisesRegex(ValueError, "distinct"):
            self.run_suite(["--cases", "lower_first", "lower_first"])
        self.assertEqual(self.seen, [])

    def test_subset_correctness_cannot_authorize_untested_configuration(self):
        self.assertEqual(self.run_suite(["--phase", "correctness", "--cases", "streaming"]), 0)
        self.seen.clear()
        with self.assertRaisesRegex(ValueError, "config_sha256"):
            self.run_suite(["--phase", "performance", "--cases", "streaming", "lower_first",
                            "--correctness-report", str(self.output / "summary.json")],
                           output=self.folder / "performance")
        self.assertEqual(self.seen, [])

    def test_reuse_rejects_changed_binary_before_running_any_case(self):
        self.assertEqual(self.run_suite(["--phase", "correctness"]), 0)
        self.seen.clear();self.binary.write_bytes(b"changed executable")
        with self.assertRaisesRegex(ValueError, "binary_sha256"):
            self.run_suite(["--phase", "performance", "--correctness-report",
                            str(self.output / "summary.json")], output=self.folder / "performance")
        self.assertEqual(self.seen, [])

    def test_reuse_rejects_modified_correctness_input_before_running(self):
        self.assertEqual(self.run_suite(["--phase", "correctness"]), 0)
        self.seen.clear()
        workload = self.output / "correctness/shared-workload.json"
        workload.write_text(workload.read_text() + "\n")
        with self.assertRaisesRegex(ValueError, "workload evidence"):
            self.run_suite(["--phase", "performance", "--correctness-report",
                            str(self.output / "summary.json")], output=self.folder / "performance")
        self.assertEqual(self.seen, [])

    def test_interruption_preserves_completed_cases_and_marks_report_invalid(self):
        def replay(*args, **kwargs):
            if self.seen:
                raise KeyboardInterrupt()
            return self.replay(*args, **kwargs)
        with self.assertRaises(KeyboardInterrupt):
            self.run_suite(replay=replay)
        report = c.read(self.output / "summary.json")
        self.assertEqual(report["status"], "INVALID")
        self.assertEqual(report["cases"][0]["status"], "PASS")
        self.assertTrue(any("KeyboardInterrupt" in reason for reason in report["failure_reasons"]))
        self.assertTrue(all(row["completed_tps"] is None for row in report["cases"][1:]))

    def test_no_case_starts_when_budget_is_nearly_used(self):
        with mock.patch.object(suite.time, "monotonic", side_effect=[0, 0, 280, 280, 280]):
            self.assertEqual(self.run_suite(), 2)
        report = c.read(self.output / "summary.json")
        self.assertEqual(report["status"], "BUDGET_EXHAUSTED")
        self.assertEqual(self.seen, [])
        self.assertTrue(all(row["status"] == "NOT_RUN" for row in report["cases"]))

    def test_large_performance_counts_are_rejected(self):
        with self.assertRaises(ValueError):
            self.run_suite(["--count", "10000"])
        self.assertEqual(self.seen, [])


class BundleConfiguration(unittest.TestCase):
    def setUp(self):
        self.raw = c.read(ROOT / "config/three_layer_lower_bundle.json")

    def test_bundle_capacity_and_legacy_default_are_separate(self):
        validated = c.validate(self.raw)
        self.assertEqual(validated["consensus"]["ancestor_leaf_batch_blocks"], 2)
        legacy = c.validate(c.read(ROOT / "config/three_layer_locality.json"))
        self.assertNotIn("ancestor_leaf_batch_blocks", legacy["consensus"])
        single = copy.deepcopy(self.raw)
        single["consensus"]["ancestor_leaf_batch_blocks"] = 1
        self.assertEqual(c.validate(single)["consensus"]["ancestor_leaf_batch_blocks"], 1)

    def test_bundle_rejects_incompatible_mode_and_insufficient_capacity(self):
        for key, value in (("ancestor_leaf_batch_blocks", True), ("ancestor_leaf_batch_blocks", 0),
                           ("ancestor_leaf_batch_blocks", 3), ("ancestor_streaming", False),
                           ("ancestor_order", "root_first"), ("ancestor_lower_blocks_per_root", 1),
                           ("ancestor_lower_blocks_per_root", 10), ("batch_size", 199)):
            with self.subTest(key=key, value=value):
                cfg = copy.deepcopy(self.raw);cfg["consensus"][key] = value
                with self.assertRaises(ValueError):
                    c.validate(cfg)
        self.raw["consensus"]["batch_size"] = 200
        self.assertEqual(c.validate(self.raw)["consensus"]["batch_size"], 200)


class WeightedStreamSchedule(unittest.TestCase):
    def test_two_child_blocks_then_root_and_next_cycle(self):
        expected = [(5, 1), (5, 2), (7, 1), (5, 3), (5, 4), (7, 2)]
        self.assertEqual([suite.stream_slot([5, 7], position, True, 2)
                          for position in range(1, 7)], expected)
        self.assertEqual(suite.stream_position([5, 7], 7, 1, True, 2), 3)
        self.assertEqual(suite.stream_position([5, 7], 5, 3, True, 2), 4)

    def test_four_layers_get_two_blocks_per_lower_ancestor_before_root(self):
        expected = [(5, 1), (7, 1), (5, 2), (7, 2), (9, 1), (5, 3)]
        self.assertEqual([suite.stream_slot([5, 7, 9], position, True, 2)
                          for position in range(1, 7)], expected)

    def test_two_to_one_durable_cursor_resumes_after_first_root(self):
        self.assertEqual(suite.stream_slot([5, 7], 3, True, 2), (7, 1))
        self.assertEqual(suite.stream_slot([5, 7], 3 + 1, True, 2), (5, 3))

    def test_ten_child_blocks_then_root_and_next_cycle(self):
        expected = [(5, block) for block in range(1, 11)] + [(7, 1), (5, 11)]
        self.assertEqual([suite.stream_slot([5, 7], position, True, 10)
                          for position in range(1, 13)], expected)
        self.assertEqual(suite.stream_position([5, 7], 7, 1, True, 10), 11)
        self.assertEqual(suite.stream_position([5, 7], 5, 11, True, 10), 12)

    def test_four_layers_get_ten_blocks_per_lower_ancestor(self):
        expected = [(co, block) for block in range(1, 11) for co in (5, 7)] + [(9, 1), (5, 11)]
        self.assertEqual([suite.stream_slot([5, 7, 9], position, True, 10)
                          for position in range(1, 23)], expected)

    def test_slot_inverse_round_trip_and_one_to_one_compatibility(self):
        for ancestors, lower_first in (([5, 7], True), ([5, 7, 9], True), ([9], True),
                                       ([7, 5], False), ([9, 7, 5], False)):
            for ratio in (1, 2, 10):
                for position in range(1, 300):
                    coordinator, block = suite.stream_slot(ancestors, position, lower_first, ratio)
                    self.assertEqual(suite.stream_position(ancestors, coordinator, block, lower_first, ratio), position)
                    if ratio == 1:
                        self.assertEqual((coordinator, block),
                                         (ancestors[(position - 1) % len(ancestors)], 1 + (position - 1) // len(ancestors)))

    def test_bundle_pairs_stay_with_each_lower_ancestor_in_four_layers(self):
        expected = [(5, 1), (5, 2), (7, 1), (7, 2), (9, 1), (5, 3), (5, 4)]
        self.assertEqual([suite.stream_slot([5, 7, 9], position, True, 2, 2)
                          for position in range(1, 8)], expected)
        for ancestors in ([5, 7], [5, 7, 9], [5, 7, 9, 11], [7]):
            for position in range(1, 300):
                coordinator, block = suite.stream_slot(ancestors, position, True, 2, 2)
                self.assertEqual(suite.stream_position(ancestors, coordinator, block, True, 2, 2), position)

    def test_bundle_schedule_rejects_unsupported_mode(self):
        for lower_first, ratio, batch in ((False, 2, 2), (True, 1, 2), (True, 10, 2), (True, 2, 3)):
            with self.subTest(lower_first=lower_first, ratio=ratio, batch=batch):
                with self.assertRaises(ValueError):
                    suite.stream_slot([5, 7], 1, lower_first, ratio, batch)
                with self.assertRaises(ValueError):
                    suite.stream_position([5, 7], 5, 1, lower_first, ratio, batch)

    def test_root_only_has_one_block_per_slot(self):
        self.assertEqual([suite.stream_slot([7], position, True, 10) for position in range(1, 12)],
                         [(7, block) for block in range(1, 12)])


class AppliedOrderAudit(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory();self.addCleanup(temp.cleanup)
        self.run = Path(temp.name)
        self.cfg = dict(replicas_per_shard=1, shards=[dict(id=7, parent=None), dict(id=5, parent=7),
                                                    dict(id=1, parent=5)], consensus=dict(ordering_window_blocks=1))
        self.node = self.run / "shard1/node0";self.node.mkdir(parents=True)

    @staticmethod
    def proof(co, block, business=False):
        requests = [dict(body=dict(txs=[dict(participants=[1, 2])]))] if business else []
        return dict(proposal=dict(body=dict(shard=co, seq=block, value=dict(cst_round=block, requests=requests))))

    def write(self, values, cursor):
        (self.node / "commits.jsonl").write_text("".join(json.dumps(dict(certificate=dict(proposal=dict(body=dict(value=value)))))
                                                        + "\n" for value in values))
        c.write(self.node / "status.json", dict(ancestor_stream_cursor=cursor))

    def test_streaming_accepts_early_root_and_requires_child_before_next_root(self):
        root, child, next_root = self.proof(7, 1, True), self.proof(5, 1), self.proof(7, 2, True)
        self.write([dict(cst_frontier=[root], cst_orders=[root]),
                    dict(cst_frontier=[child, next_root], cst_orders=[next_root])], 3)
        self.assertEqual(suite.audit_leaf_order(self.run, self.cfg, "streaming")["status"], "PASS")
        self.write([dict(cst_frontier=[root], cst_orders=[root]),
                    dict(cst_frontier=[next_root], cst_orders=[next_root])], 2)
        with self.assertRaisesRegex(ValueError, "skipped or reordered"):
            suite.audit_leaf_order(self.run, self.cfg, "streaming")

    def test_streaming_rejects_certified_skip_over_business(self):
        root, child = self.proof(7, 1, True), self.proof(5, 1, True)
        self.write([dict(cst_frontier=[root, child], cst_orders=[child])], 2)
        with self.assertRaisesRegex(ValueError, "skipped applicable"):
            suite.audit_leaf_order(self.run, self.cfg, "streaming")

    def test_lower_first_accepts_early_child_and_requires_root_before_next_child(self):
        self.cfg["consensus"].update(ancestor_streaming=True, ancestor_order="lower_first")
        child, root, next_child = self.proof(5, 1, True), self.proof(7, 1), self.proof(5, 2, True)
        self.write([dict(cst_frontier=[child], cst_orders=[child]),
                    dict(cst_frontier=[root, next_child], cst_orders=[next_child])], 3)
        self.assertEqual(suite.audit_leaf_order(self.run, self.cfg, "lower_first")["status"], "PASS")
        self.write([dict(cst_frontier=[child], cst_orders=[child]),
                    dict(cst_frontier=[next_child], cst_orders=[next_child])], 2)
        with self.assertRaisesRegex(ValueError, "skipped or reordered"):
            suite.audit_leaf_order(self.run, self.cfg, "lower_first")

    def test_lower_first_rejects_old_root_first_journal(self):
        self.cfg["consensus"].update(ancestor_streaming=True, ancestor_order="lower_first")
        root = self.proof(7, 1, True)
        self.write([dict(cst_frontier=[root], cst_orders=[root])], 1)
        with self.assertRaisesRegex(ValueError, "skipped or reordered"):
            suite.audit_leaf_order(self.run, self.cfg, "lower_first")

    def test_lower_first_rejects_skipped_child_business(self):
        self.cfg["consensus"].update(ancestor_streaming=True, ancestor_order="lower_first")
        child, root = self.proof(5, 1, True), self.proof(7, 1, True)
        self.write([dict(cst_frontier=[child, root], cst_orders=[root])], 2)
        with self.assertRaisesRegex(ValueError, "skipped applicable"):
            suite.audit_leaf_order(self.run, self.cfg, "lower_first")

    def test_lower_first_handles_deeper_ancestors_in_nearest_first_order(self):
        self.cfg["shards"] = [dict(id=9, parent=None), dict(id=7, parent=9),
                              dict(id=5, parent=7), dict(id=1, parent=5)]
        self.cfg["consensus"].update(ancestor_streaming=True, ancestor_order="lower_first")
        near, middle, root, next_near = (self.proof(5, 1, True), self.proof(7, 1),
                                       self.proof(9, 1), self.proof(5, 2, True))
        self.write([dict(cst_frontier=[near], cst_orders=[near]),
                    dict(cst_frontier=[middle, root, next_near], cst_orders=[next_near])], 4)
        self.assertEqual(suite.audit_leaf_order(self.run, self.cfg, "lower_first")["status"], "PASS")

    def test_configuration_flag_determines_audit_order(self):
        self.cfg["consensus"].update(ancestor_streaming=True, ancestor_order="lower_first")
        child = self.proof(5, 1, True)
        self.write([dict(cst_frontier=[child], cst_orders=[child])], 1)
        self.assertEqual(suite.audit_leaf_order(self.run, self.cfg, "streaming")["status"], "PASS")

    def test_two_to_one_journal_accepts_child_one_two_root_one_and_resumes_child_three(self):
        self.cfg["consensus"].update(ancestor_streaming=True, ancestor_order="lower_first",
                                    ancestor_lower_blocks_per_root=2)
        proofs = [self.proof(co, block, True) for co, block in
                  ((5, 1), (5, 2), (7, 1), (5, 3), (5, 4), (7, 2))]
        self.write([dict(cst_frontier=[proof], cst_orders=[proof]) for proof in proofs[:3]], 3)
        self.assertEqual(suite.audit_leaf_order(self.run, self.cfg, "lower_2")["status"], "PASS")
        self.write([dict(cst_frontier=[proof], cst_orders=[proof]) for proof in proofs], 6)
        self.assertEqual(suite.audit_leaf_order(self.run, self.cfg, "lower_2")["business_blocks"], 6)

    def test_two_to_one_journal_cannot_skip_child_two_or_root_one(self):
        self.cfg["consensus"].update(ancestor_streaming=True, ancestor_order="lower_first",
                                    ancestor_lower_blocks_per_root=2)
        for positions in (((5, 1), (7, 1)), ((5, 1), (5, 2), (5, 3))):
            proofs = [self.proof(co, block, True) for co, block in positions]
            self.write([dict(cst_frontier=[proof], cst_orders=[proof]) for proof in proofs], len(proofs))
            with self.assertRaisesRegex(ValueError, "skipped or reordered"):
                suite.audit_leaf_order(self.run, self.cfg, "lower_2")

    def test_two_to_one_journal_four_layers_each_lower_gets_two_blocks(self):
        self.cfg["shards"] = [dict(id=9, parent=None), dict(id=7, parent=9),
                              dict(id=5, parent=7), dict(id=1, parent=5)]
        self.cfg["consensus"].update(ancestor_streaming=True, ancestor_order="lower_first",
                                    ancestor_lower_blocks_per_root=2)
        proofs = [self.proof(co, block, index == 4) for index, (co, block) in enumerate(
                  ((5, 1), (7, 1), (5, 2), (7, 2), (9, 1)))]
        self.write([dict(cst_frontier=proofs, cst_orders=[proofs[-1]])], 5)
        self.assertEqual(suite.audit_leaf_order(self.run, self.cfg, "lower_2")["status"], "PASS")

    def test_two_to_one_journal_still_rejects_skipped_applicable_business(self):
        self.cfg["consensus"].update(ancestor_streaming=True, ancestor_order="lower_first",
                                    ancestor_lower_blocks_per_root=2)
        child, root = self.proof(5, 1, True), self.proof(7, 1, True)
        self.write([dict(cst_frontier=[child, self.proof(5, 2), root], cst_orders=[root])], 3)
        with self.assertRaisesRegex(ValueError, "skipped applicable"):
            suite.audit_leaf_order(self.run, self.cfg, "lower_2")

    def test_ten_to_one_accepts_child_ten_before_root_one_business(self):
        self.cfg["consensus"].update(ancestor_streaming=True, ancestor_order="lower_first",
                                    ancestor_lower_blocks_per_root=10)
        proofs = [self.proof(5, block, True) for block in range(1, 11)] + [self.proof(7, 1, True),
                                                                                 self.proof(5, 11, True)]
        self.write([dict(cst_frontier=[proof], cst_orders=[proof]) for proof in proofs], 12)
        audit = suite.audit_leaf_order(self.run, self.cfg, "lower_10")
        self.assertEqual(audit["status"], "PASS")
        self.assertEqual(audit["business_blocks"], 12)

    def test_ten_to_one_rejects_missing_tenth_child_and_missing_root(self):
        self.cfg["consensus"].update(ancestor_streaming=True, ancestor_order="lower_first",
                                    ancestor_lower_blocks_per_root=10)
        for skip_child in (True, False):
            proofs = [self.proof(5, block, True) for block in range(1, 10 if skip_child else 11)]
            proofs += [self.proof(7, 1, True) if skip_child else self.proof(5, 11, True)]
            self.write([dict(cst_frontier=[proof], cst_orders=[proof]) for proof in proofs], len(proofs))
            with self.assertRaisesRegex(ValueError, "skipped or reordered"):
                suite.audit_leaf_order(self.run, self.cfg, "lower_10")

    def test_ten_to_one_retains_certified_skip_business_check(self):
        self.cfg["consensus"].update(ancestor_streaming=True, ancestor_order="lower_first",
                                    ancestor_lower_blocks_per_root=10)
        proofs = [self.proof(5, block, block == 3) for block in range(1, 11)] + [self.proof(7, 1, True)]
        self.write([dict(cst_frontier=proofs, cst_orders=[proofs[-1]])], 11)
        with self.assertRaisesRegex(ValueError, "skipped applicable"):
            suite.audit_leaf_order(self.run, self.cfg, "lower_10")

    def test_ten_to_one_full_prefix_preserves_cursor_across_cycle_boundary(self):
        self.cfg["consensus"].update(ancestor_streaming=True, ancestor_order="lower_first",
                                    ancestor_lower_blocks_per_root=10)
        prefix = [self.proof(*suite.stream_slot([5, 7], position, True, 10)) for position in range(1, 33)]
        root, next_child = self.proof(7, 3), self.proof(5, 31, True)
        self.write([dict(cst_frontier=prefix, cst_stream_progress=True),
                    dict(cst_frontier=[root, next_child], cst_orders=[next_child])], 34)
        self.assertEqual(suite.audit_leaf_order(self.run, self.cfg, "lower_10")["status"], "PASS")
        c.write(self.node / "status.json", dict(ancestor_stream_cursor=33))
        with self.assertRaisesRegex(ValueError, "durable cursor"):
            suite.audit_leaf_order(self.run, self.cfg, "lower_10")

    def test_ten_to_one_four_layers_mirror_nearest_first_schedule(self):
        self.cfg["shards"] = [dict(id=9, parent=None), dict(id=7, parent=9),
                              dict(id=5, parent=7), dict(id=1, parent=5)]
        self.cfg["consensus"].update(ancestor_streaming=True, ancestor_order="lower_first",
                                    ancestor_lower_blocks_per_root=10)
        proofs = [self.proof(*suite.stream_slot([5, 7, 9], position, True, 10), position == 21)
                  for position in range(1, 22)]
        self.write([dict(cst_frontier=proofs, cst_orders=[proofs[-1]])], 21)
        self.assertEqual(suite.audit_leaf_order(self.run, self.cfg, "lower_10")["status"], "PASS")

    def bundle_config(self):
        self.cfg["consensus"].update(ancestor_streaming=True, ancestor_order="lower_first",
            ancestor_lower_blocks_per_root=2, ancestor_leaf_batch_blocks=2)

    def test_bundle_applies_two_lower_business_blocks_in_one_slot_then_root(self):
        self.bundle_config()
        child1, child2, root1 = self.proof(5, 1, True), self.proof(5, 2, True), self.proof(7, 1, True)
        self.write([dict(cst_frontier=[child1, child2], cst_orders=[child1, child2]),
                    dict(cst_frontier=[root1], cst_orders=[root1])], 3)
        audit = suite.audit_leaf_order(self.run, self.cfg, "lower_bundle")
        self.assertEqual(audit["status"], "PASS")
        self.assertEqual(audit["business_blocks"], 3)
        self.assertEqual(audit["business_pbft_slots"], 2)
        self.assertEqual(audit["bundled_pbft_slots"], 1)

    def test_bundle_rejects_early_first_block_and_tampered_business_proof(self):
        self.bundle_config()
        child1, child2 = self.proof(5, 1, True), self.proof(5, 2, True)
        self.write([dict(cst_frontier=[child1], cst_orders=[child1])], 1)
        with self.assertRaisesRegex(ValueError, "closes early"):
            suite.audit_leaf_order(self.run, self.cfg, "lower_bundle")
        bad = copy.deepcopy(child2);bad["proposal"]["body"]["seq"] = 88
        self.write([dict(cst_frontier=[child1, child2], cst_orders=[child1, bad])], 2)
        with self.assertRaisesRegex(ValueError, "differs"):
            suite.audit_leaf_order(self.run, self.cfg, "lower_bundle")

    def test_bundle_rejects_skip_duplicate_or_reversed_business_orders(self):
        self.bundle_config()
        child1, child2 = self.proof(5, 1, True), self.proof(5, 2, True)
        for orders in ([child2], [child1, child1], [child2, child1]):
            self.write([dict(cst_frontier=[child1, child2], cst_orders=orders)], 2)
            with self.assertRaises(ValueError):
                suite.audit_leaf_order(self.run, self.cfg, "lower_bundle")

    def test_bundle_rejects_cross_pair_root_and_coordinator_merges(self):
        self.bundle_config()
        child1, child2, root1 = self.proof(5, 1, True), self.proof(5, 2), self.proof(7, 1, True)
        self.write([dict(cst_frontier=[child1, child2, root1], cst_orders=[child1, root1])], 3)
        with self.assertRaisesRegex(ValueError, "skipped applicable"):
            suite.audit_leaf_order(self.run, self.cfg, "lower_bundle")
        self.cfg["shards"] = [dict(id=9, parent=None), dict(id=7, parent=9),
                              dict(id=5, parent=7), dict(id=1, parent=5)]
        proofs = [self.proof(co, block, block == 2) for co, block in ((5, 1), (5, 2), (7, 1), (7, 2))]
        self.write([dict(cst_frontier=proofs, cst_orders=[proofs[1], proofs[3]])], 4)
        with self.assertRaisesRegex(ValueError, "crosses coordinator"):
            suite.audit_leaf_order(self.run, self.cfg, "lower_bundle")
        self.cfg["shards"] = [dict(id=7, parent=None), dict(id=5, parent=7), dict(id=1, parent=5)]
        proofs = [self.proof(*suite.stream_slot([5, 7], position, True, 2, 2), position in (2, 5))
                  for position in range(1, 6)]
        self.write([dict(cst_frontier=proofs, cst_orders=[proofs[1], proofs[4]])], 5)
        with self.assertRaisesRegex(ValueError, "crosses coordinator"):
            suite.audit_leaf_order(self.run, self.cfg, "lower_bundle")

    def test_bundle_four_layers_pairs_each_lower_before_root_without_ancestor_barrier(self):
        self.bundle_config()
        self.cfg["shards"] = [dict(id=9, parent=None), dict(id=7, parent=9),
                              dict(id=5, parent=7), dict(id=1, parent=5)]
        near1, near2 = self.proof(5, 1, True), self.proof(5, 2, True)
        self.write([dict(cst_frontier=[near1, near2], cst_orders=[near1, near2])], 2)
        self.assertEqual(suite.audit_leaf_order(self.run, self.cfg, "lower_bundle")["status"], "PASS")
        middle1, middle2, root = self.proof(7, 1), self.proof(7, 2, True), self.proof(9, 1, True)
        self.write([dict(cst_frontier=[near1, near2], cst_orders=[near1, near2]),
                    dict(cst_frontier=[middle1, middle2], cst_orders=[middle2]),
                    dict(cst_frontier=[root], cst_orders=[root])], 5)
        self.assertEqual(suite.audit_leaf_order(self.run, self.cfg, "lower_bundle")["status"], "PASS")

    def test_bundle_empty_prefix_and_single_applicable_second_block(self):
        self.bundle_config()
        child1, child2, root1 = self.proof(5, 1), self.proof(5, 2), self.proof(7, 1)
        child3, child4 = self.proof(5, 3), self.proof(5, 4, True)
        self.write([dict(cst_frontier=[child1, child2, root1, child3, child4], cst_orders=[child4])], 5)
        self.assertEqual(suite.audit_leaf_order(self.run, self.cfg, "lower_bundle")["business_blocks"], 1)

    def test_bundle_31_empty_control_prefix_allows_complete_pair_after_boundary(self):
        self.bundle_config()
        empty = [self.proof(*suite.stream_slot([5, 7], position, True, 2, 2)) for position in range(1, 32)]
        second = self.proof(*suite.stream_slot([5, 7], 32, True, 2, 2), True)
        # Position 31 is Child_21; its partner at 32 may carry business. The
        # partial empty prefix can be committed before that partner is ready.
        self.write([dict(cst_frontier=empty, cst_stream_progress=True),
                    dict(cst_frontier=[second], cst_orders=[second])], 32)
        self.assertEqual(suite.audit_leaf_order(self.run, self.cfg, "lower_bundle")["status"], "PASS")
        self.write([dict(cst_frontier=empty[:30], cst_stream_progress=True),
                    dict(cst_frontier=[self.proof(5, 21, True), self.proof(5, 22, True)],
                         cst_orders=[self.proof(5, 21, True), self.proof(5, 22, True)])], 32)
        self.assertEqual(suite.audit_leaf_order(self.run, self.cfg, "lower_bundle")["status"], "PASS")
        empty[0] = self.proof(5, 1, True)
        self.write([dict(cst_frontier=empty, cst_stream_progress=True)], 31)
        with self.assertRaisesRegex(ValueError, "skipped applicable"):
            suite.audit_leaf_order(self.run, self.cfg, "lower_bundle")

    def test_bundle_31_empty_prefix_does_not_force_a_33_proof_business_frontier(self):
        self.bundle_config()
        ancestors = [5, 7, 9, 11, 13]
        self.cfg["shards"] = [dict(id=1, parent=5), dict(id=5, parent=7), dict(id=7, parent=9),
                              dict(id=9, parent=11), dict(id=11, parent=13), dict(id=13, parent=None)]
        empty = [self.proof(*suite.stream_slot(ancestors, position, True, 2, 2)) for position in range(1, 32)]
        pair = [self.proof(*suite.stream_slot(ancestors, position, True, 2, 2), True) for position in (32, 33)]
        self.write([dict(cst_frontier=empty, cst_stream_progress=True),
                    dict(cst_frontier=pair, cst_orders=pair)], 33)
        self.assertEqual(suite.audit_leaf_order(self.run, self.cfg, "lower_bundle")["status"], "PASS")
        self.write([dict(cst_frontier=empty + pair, cst_orders=pair)], 33)
        with self.assertRaisesRegex(ValueError, "malformed"):
            suite.audit_leaf_order(self.run, self.cfg, "lower_bundle")

    def test_barrier_requires_complete_ancestor_frontier_and_manifest_head(self):
        root, child = self.proof(7, 1, True), self.proof(5, 1, True)
        for proof in (root, child):
            proof["proposal"]["body"]["value"]["cst_window_manifest"] = {"1": dict(orders=[dict(block=1)])}
        self.write([dict(cst_frontier=[root, child], cst_orders=[root]),
                    dict(cst_frontier=[root, child], cst_orders=[child])], 0)
        self.assertEqual(suite.audit_leaf_order(self.run, self.cfg, "barrier")["status"], "PASS")
        self.write([dict(cst_frontier=[root, child], cst_orders=[child])], 0)
        with self.assertRaisesRegex(ValueError, "skipped manifest"):
            suite.audit_leaf_order(self.run, self.cfg, "barrier")
        self.write([dict(cst_frontier=[root], cst_orders=[root])], 0)
        with self.assertRaisesRegex(ValueError, "not fully closed"):
            suite.audit_leaf_order(self.run, self.cfg, "barrier")


if __name__ == "__main__":
    unittest.main(verbosity=2)
