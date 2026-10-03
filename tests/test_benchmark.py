#!/usr/bin/env python3
"""Validate benchmark decisions and comparison accounting without a cluster."""
import copy
import importlib.util
from pathlib import Path
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


c = module("cluster_for_benchmark_tests", ROOT / "scripts/cluster.py")
b = module("benchmark_under_test", ROOT / "scripts/benchmark.py")


def configuration():
    raw = c.read(ROOT / "config/two_layer.json")
    return c.validate(raw)


def workload_case(mode="cross"):
    return {"mode": mode, "count": 128, "rate": 500, "batch": 8,
            "repeat": 1, "seed": 42, "shard": 1 if mode == "intra" else None,
            "participants": [] if mode == "intra" else [1, 2]}


def client_result(case):
    return {"requests": 16, "completed_requests": 16,
            "executed_transactions": case["count"], "ordered_only_transactions": 0,
            "duplicate_transactions": 0, "errors": 0, "elapsed_s": 2.0,
            "completed_tps": 64.0, "avg_latency_s": 0.12,
            "p50_s": 0.10, "p95_s": 0.25, "p99_s": 0.31, "timings": []}


def status_rows(case, completed=True, cfg=None):
    """One transaction executes at each replica, but counts once in client TPS."""
    rows = []
    cfg = cfg or configuration()
    parent, leaves = c.topology(cfg)
    coordinator = c.lca(cfg, case["participants"]) if case["mode"] == "cross" else None
    for shard in parent:
        leaf_count = case["count"] if completed and (
            case["mode"] == "cross" and shard in case["participants"]
            or case["mode"] == "intra" and shard == case["shard"]
        ) else 0
        root_count = case["count"] if completed and case["mode"] == "cross" and shard == coordinator else 0
        for replica in range(4):
            rows.append({"shard": shard, "replica": replica, "ready": True, "alive": True,
                         "view": 0, "view_changes": 0, "changing_view": False,
                         "executed_transactions": leaf_count,
                         "ordered_cst_transactions": root_count,
                         "leaf_ordered_cst_transactions": leaf_count if case["mode"] == "cross" else 0,
                         "completed_cst_transactions": root_count,
                         "applied_batches": 4 if leaf_count or root_count else 0,
                         "state_digest": f"state-{shard}", "chain_digest": f"chain-{shard}",
                         "kv_digest": f"kv-{shard}", "pending_requests": 0,
                         "dedup_waiting_requests": 0, "pending_cst_batches": 0,
                         "staged_cst_batches": 0, "network_queue": 0,
                         "network_buffered_bytes": 0, "network_failures": 0,
                         "network_queue_errors": 0, "inbox_dropped": 0,
                         "messages_sent": 140 if completed else 100,
                         "bytes_sent": 1800 if completed else 1000,
                         "network_connect_attempts": 7 if completed else 3,
                         "network_connections_reused": 30 if completed else 10})
    return rows


class ResultAccounting(unittest.TestCase):
    def setUp(self):
        self.cfg = configuration()
        self.case = workload_case()
        self.result = client_result(self.case)
        self.before = status_rows(self.case, completed=False)
        self.after = status_rows(self.case)

    def summary(self, **overrides):
        arguments = {"case": self.case, "cfg": self.cfg, "client_rc": 0,
                     "result": self.result, "before": self.before,
                     "after": self.after, "drained": True}
        arguments.update(overrides)
        return b.summarize_case(**arguments)

    def assert_failure(self, summary):
        self.assertEqual(summary["status"], "FAIL")
        self.assertTrue(summary["failure_reasons"])
        for field in ["completed_tps", "avg_latency_s", "p50_s", "p95_s", "p99_s"]:
            self.assertIsNone(summary[field], field)

    def test_cross_shard_tps_counts_client_transactions_once(self):
        self.assertTrue(b.settled(self.case, self.cfg, self.after))
        summary = self.summary()
        self.assertEqual(summary["status"], "PASS")
        self.assertEqual(summary["failure_reasons"], [])
        self.assertEqual(summary["executed_transactions"], 128)
        self.assertAlmostEqual(summary["completed_tps"], 64.0)
        self.assertAlmostEqual(summary["p95_s"], 0.25)
        # Traffic is summed across processes, execution is never replica-summed.
        self.assertEqual(summary["node_metrics"]["messages_sent"], 480)
        self.assertEqual(summary["node_metrics"]["bytes_sent"], 9600)
        self.assertEqual(summary["node_metrics"]["network_connect_attempts"], 48)
        self.assertEqual(summary["node_metrics"]["network_connections_reused"], 240)
        self.assertEqual(summary["node_metrics"]["network_failures"], 0)
        self.assertEqual(summary["per_shard"]["1"]["executed_each_replica"], [128] * 4)
        self.assertEqual(summary["per_shard"]["2"]["executed_each_replica"], [128] * 4)
        self.assertEqual(summary["per_shard"]["5"]["executed_each_replica"], [0] * 4)

    def test_direct_ack_completion_needs_no_coordinator_decision_counter(self):
        # The coordinator's completed cache derives from both leaf ACK QCs;
        # the simplified protocol has no DECISION/FINALIZE consensus stages.
        self.assertTrue(all("decided_cst_batches" not in row for row in self.after))
        self.assertTrue(b.settled(self.case, self.cfg, self.after))
        self.assertEqual(self.summary()["status"], "PASS")

    def test_committed_order_waiting_for_dependency_is_not_completed_throughput(self):
        rows = copy.deepcopy(self.after)
        for row in rows:
            if row["shard"] == 2:
                row.update({"staged_cst_batches": 1, "applied_batches": 0,
                            "leaf_ordered_cst_transactions": 0, "executed_transactions": 0})
            elif row["shard"] == 5:
                row["completed_cst_transactions"] = 0
        result = copy.deepcopy(self.result)
        result.update({"completed_requests": 0, "executed_transactions": 0})
        self.assertFalse(b.settled(self.case, self.cfg, rows))
        self.assert_failure(self.summary(after=rows, result=result, client_rc=2, drained=False))

    def test_intra_shard_completion_does_not_require_other_leaf_to_execute(self):
        case = workload_case("intra")
        rows = status_rows(case)
        self.assertTrue(b.settled(case, self.cfg, rows))
        summary = self.summary(case=case, after=rows, before=status_rows(case, False))
        self.assertEqual(summary["status"], "PASS")
        self.assertEqual(summary["executed_transactions"], 128)

    def test_multilayer_empty_round_slots_are_not_business_transactions(self):
        cfg = c.validate(c.read(ROOT / "config/three_layer.json"))
        case = workload_case()
        case["participants"] = [1, 2, 3]
        rows = status_rows(case, cfg=cfg)
        for row in rows:
            if row["shard"] in (5, 6):
                # Every coordinator certifies a round close, including empty
                # closes. These consume PBFT slots but never business counts.
                row["applied_batches"] = 7
                row["cst_round"] = 7
                row["rounds_requested"] = 7
        self.assertTrue(b.settled(case, cfg, rows))
        result = self.summary(case=case, cfg=cfg, after=rows,
                              before=status_rows(case, False, cfg))
        self.assertEqual(result["status"], "PASS")
        self.assertEqual(result["executed_transactions"], case["count"])
        self.assertEqual(result["per_shard"]["4"]["executed_each_replica"], [0] * 4)
        self.assertEqual(result["per_shard"]["5"]["executed_each_replica"], [0] * 4)
        for sid, field in [(4, "executed_transactions"),
                           (5, "ordered_cst_transactions"),
                           (5, "completed_cst_transactions"),
                           (6, "executed_transactions")]:
            bad = copy.deepcopy(rows)
            for row in bad:
                if row["shard"] == sid:
                    row[field] = 1
            with self.subTest(shard=sid, field=field):
                self.assertFalse(b.settled(case, cfg, bad))

    def test_unrelated_coordinator_must_finish_requested_empty_close(self):
        cfg = c.validate(c.read(ROOT / "config/three_layer.json"))
        case = workload_case()
        case["participants"] = [1, 3]
        rows = status_rows(case, cfg=cfg)
        for row in rows:
            if row["shard"] == 6:
                row["cst_round"] = 4
                row["rounds_requested"] = 5
        self.assertFalse(b.settled(case, cfg, rows))
        result = self.summary(case=case, cfg=cfg, after=rows,
                              before=status_rows(case, False, cfg))
        self.assert_failure(result)
        self.assertTrue(any("未封闭轮次" in problem for problem in result["failure_reasons"]))
        for row in rows:
            if row["shard"] == 6:
                row["cst_round"] = 5
        self.assertTrue(b.settled(case, cfg, rows))

    def test_incomplete_client_is_failure_even_with_a_tps_number(self):
        result = copy.deepcopy(self.result)
        result["completed_requests"] = 10
        result["executed_transactions"] = 80
        result["completed_tps"] = 40.0
        self.assert_failure(self.summary(client_rc=2, result=result))

    def test_client_error_or_ordering_only_cannot_pass(self):
        for changes in [{"errors": 1}, {"ordered_only_transactions": 128, "executed_transactions": 0},
                        {"duplicate_transactions": 1, "executed_transactions": 127}]:
            with self.subTest(changes=changes):
                result = copy.deepcopy(self.result)
                result.update(changes)
                self.assert_failure(self.summary(result=result))

    def test_client_success_does_not_hide_unconverged_nodes(self):
        rows = copy.deepcopy(self.after)
        rows[3]["state_digest"] = "different-state"
        self.assertFalse(b.settled(self.case, self.cfg, rows))
        self.assert_failure(self.summary(after=rows))

    def test_all_replicas_and_all_phase_queues_must_settle(self):
        for field, value in [("alive", False), ("ready", False), ("changing_view", True),
                             ("pending_requests", 1), ("dedup_waiting_requests", 1),
                             ("pending_cst_batches", 1), ("staged_cst_batches", 1),
                             ("network_queue", 1), ("network_buffered_bytes", 4096)]:
            with self.subTest(field=field):
                rows = copy.deepcopy(self.after)
                rows[0][field] = value
                self.assertFalse(b.settled(self.case, self.cfg, rows))
                self.assert_failure(self.summary(after=rows))
        self.assertFalse(b.settled(self.case, self.cfg, self.after[:-1]))
        self.assert_failure(self.summary(drained=False))

    def test_missing_or_invalid_client_metrics_cannot_pass(self):
        for field in ["elapsed_s", "completed_tps", "avg_latency_s", "p50_s", "p95_s", "p99_s"]:
            with self.subTest(field=field, invalid="missing"):
                result = copy.deepcopy(self.result)
                del result[field]
                self.assert_failure(self.summary(result=result))
            for invalid in [float("nan"), float("inf"), -0.1, None, True, "1.0"]:
                with self.subTest(field=field, invalid=invalid):
                    result = copy.deepcopy(self.result)
                    result[field] = invalid
                    self.assert_failure(self.summary(result=result))
        for field in ["elapsed_s", "completed_tps"]:
            with self.subTest(field=field, invalid=0):
                result = copy.deepcopy(self.result)
                result[field] = 0
                self.assert_failure(self.summary(result=result))

    def test_execution_and_coordinator_completion_must_match_count(self):
        for shard, field, value in [(1, "executed_transactions", 127),
                                    (2, "executed_transactions", 129),
                                    (5, "completed_cst_transactions", 127)]:
            with self.subTest(shard=shard, field=field):
                rows = copy.deepcopy(self.after)
                for row in rows:
                    if row["shard"] == shard:
                        row[field] = value
                self.assertFalse(b.settled(self.case, self.cfg, rows))
                self.assert_failure(self.summary(after=rows))

    def test_each_shard_digests_and_applied_batches_must_agree(self):
        for field, value in [("chain_digest", "different-chain"), ("kv_digest", "different-kv"),
                             ("applied_batches", 99)]:
            with self.subTest(field=field):
                rows = copy.deepcopy(self.after)
                rows[5][field] = value
                self.assertFalse(b.settled(self.case, self.cfg, rows))


class ParameterValidation(unittest.TestCase):
    def test_cross_benchmark_accepts_multilayer_and_multiple_participants(self):
        cfg = c.validate(c.read(ROOT / "config/three_layer.json"))
        for participants in ([1, 2], [1, 3], [1, 2, 3], [1, 2, 3, 4]):
            with self.subTest(participants=participants):
                b.validate_selection(cfg, "cross", None, participants)
                b.validate_selection(cfg, "all", 1, participants)
        b.validate_selection(cfg, "intra", 1, [])

    def test_four_layer_nonuniform_topology_selection(self):
        cfg = c.validate(c.read(ROOT / "config/four_layer.json"))
        for participants, coordinator in [([1, 2], 5), ([1, 3], 7),
                                           ([1, 8], 9), ([1, 3, 8], 9)]:
            with self.subTest(participants=participants):
                self.assertEqual(c.lca(cfg, participants), coordinator)
                b.validate_selection(cfg, "cross", None, participants)

    def test_selected_shards_must_be_valid_leaves(self):
        cfg = configuration()
        b.validate_selection(cfg, "cross", None, [1, 2])
        b.validate_selection(cfg, "intra", 1, [])
        for mode, shard, participants in [("intra", 5, []), ("intra", 99, []),
                                           ("cross", None, [1]), ("cross", None, [1, 5]),
                                           ("cross", None, [1, 1])]:
            with self.subTest(mode=mode, shard=shard, participants=participants), self.assertRaises(ValueError):
                b.validate_selection(cfg, mode, shard, participants)

    def test_fingerprint_ignores_port_and_runtime_keys_but_keeps_simulation_settings(self):
        cfg = configuration()
        changed = copy.deepcopy(cfg)
        changed.update({"base_port": 42000, "run_id": "another-run",
                        "client_private_key": "/temporary/private.pem",
                        "client_public_key": "random-key", "nodes": [{"port": 42000}]})
        self.assertEqual(b.config_fingerprint(cfg), b.config_fingerprint(changed))
        for group, field, value in [("consensus", "batch_size", 512),
                                    ("execution", "fib_iterations", 200),
                                    ("network", "intra_shard_delay_ms", 5)]:
            with self.subTest(group=group, field=field):
                other = copy.deepcopy(cfg)
                other[group][field] = value
                self.assertNotEqual(b.config_fingerprint(cfg), b.config_fingerprint(other))


class BaselineComparison(unittest.TestCase):
    def summaries(self, tps, status="PASS"):
        case = workload_case()
        case.update({"status": status, "failure_reasons": [] if status == "PASS" else ["incomplete"],
                     "config_fingerprint": b.config_fingerprint(configuration()),
                     "completed_tps": tps if status == "PASS" else None,
                     "p95_s": 0.25 if status == "PASS" else None})
        return case

    def compare(self, current, old):
        return b.compare_reports({"schema_version": 1, "cases": current},
                                 {"schema_version": 1, "cases": old})

    def test_only_successful_identical_workloads_have_speedup(self):
        comparison = self.compare([self.summaries(200)], [self.summaries(100)])
        self.assertEqual(len(comparison), 1)
        self.assertTrue(comparison[0]["comparable"])
        self.assertAlmostEqual(comparison[0]["new_tps"], 200)
        self.assertAlmostEqual(comparison[0]["old_tps"], 100)
        self.assertAlmostEqual(comparison[0]["tps_speedup"], 2)

    def test_changed_parameter_prevents_performance_comparison(self):
        for field, value in [("rate", 1000), ("count", 256), ("batch", 16),
                             ("seed", 43), ("participants", [1, 3]),
                             ("config_fingerprint", "different-config")]:
            with self.subTest(field=field):
                current = self.summaries(200)
                current[field] = value
                comparison = self.compare([current], [self.summaries(100)])
                self.assertEqual(len(comparison), 1)
                self.assertFalse(comparison[0]["comparable"])
                self.assertTrue(comparison[0]["reason"])

    def test_one_failed_repeat_invalidates_whole_group(self):
        good_new, failed_new = self.summaries(200), self.summaries(0, "FAIL")
        failed_new["repeat"] = 2
        old1, old2 = self.summaries(100), self.summaries(110)
        old2["repeat"] = 2
        comparison = self.compare([good_new, failed_new], [old1, old2])
        self.assertEqual(len(comparison), 1)
        self.assertFalse(comparison[0]["comparable"])
        self.assertTrue(comparison[0]["reason"])

    def test_repeated_trials_compare_medians(self):
        current = [self.summaries(tps) for tps in [200, 400, 900]]
        baseline = [self.summaries(tps) for tps in [100, 200, 300]]
        for trials in [current, baseline]:
            for index, trial in enumerate(trials, start=1):
                trial["repeat"] = index
        comparison = self.compare(current, baseline)[0]
        self.assertTrue(comparison["comparable"])
        self.assertAlmostEqual(comparison["new_tps"], 400)
        self.assertAlmostEqual(comparison["old_tps"], 200)
        self.assertAlmostEqual(comparison["tps_speedup"], 2)

    def test_failed_baseline_cannot_be_used_for_speedup(self):
        comparison = self.compare([self.summaries(200)], [self.summaries(0, "FAIL")])[0]
        self.assertFalse(comparison["comparable"])
        self.assertTrue(comparison["reason"])

    def test_different_machine_environment_cannot_be_used_for_speedup(self):
        comparison = b.compare_reports(
            {"schema_version": 1, "environment": {"platform": "new-machine"},
             "cases": [self.summaries(200)]},
            {"schema_version": 1, "environment": {"platform": "old-machine"},
             "cases": [self.summaries(100)]})[0]
        self.assertFalse(comparison["comparable"])
        self.assertTrue(comparison["reason"])

    def test_invalid_success_metrics_prevent_comparison(self):
        for field in ["completed_tps", "p95_s"]:
            for invalid in [float("nan"), float("inf"), -1, None]:
                for side in ["current", "baseline"]:
                    with self.subTest(field=field, invalid=invalid, side=side):
                        current, baseline = self.summaries(200), self.summaries(100)
                        (current if side == "current" else baseline)[field] = invalid
                        comparison = self.compare([current], [baseline])[0]
                        self.assertFalse(comparison["comparable"])
                        self.assertTrue(comparison["reason"])

    def test_baseline_validation_rejects_bad_structure(self):
        for report in [None, [], {}, {"schema_version": 2, "cases": []},
                       {"schema_version": 1, "cases": {}},
                       {"schema_version": 1, "cases": [None]},
                       {"schema_version": 1, "cases": [{"status": "PASS"}]}]:
            with self.subTest(report=report), self.assertRaises(ValueError):
                b.validate_report(report)
        row = self.summaries(100)
        row["status"] = "UNKNOWN"
        with self.assertRaises(ValueError):
            b.validate_report({"schema_version": 1, "cases": [row]})

    def test_baseline_validation_rejects_invalid_pass_metrics(self):
        for field in ["completed_tps", "p95_s"]:
            for invalid in [float("nan"), float("inf"), -1, None]:
                with self.subTest(field=field, invalid=invalid), self.assertRaises(ValueError):
                    row = self.summaries(100)
                    row[field] = invalid
                    b.validate_report({"schema_version": 1, "cases": [row]})
        b.validate_report({"schema_version": 1, "cases": [self.summaries(100)]})
        # A FAIL report remains a valid artifact; comparison must refuse its TPS.
        b.validate_report({"schema_version": 1, "cases": [self.summaries(0, "FAIL")]})


if __name__ == "__main__":
    unittest.main(verbosity=2)
