#!/usr/bin/env python3
"""Method dispatch and exact-workload comparison tests without live clusters."""
import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import cluster as c
import benchmark as b
import clean as cleaner

spec = importlib.util.spec_from_file_location("method_compare", ROOT / "baseline/compare.py")
comparison = importlib.util.module_from_spec(spec)
spec.loader.exec_module(comparison)


def case():
    return dict(mode="cross", count=25, rate=1000, batch=8, repeat=1, seed=42,
                shard=None, participants=[1, 2])


def result(method):
    return dict(case(), method=method, config_fingerprint="same-config", workload_sha256="same-workload",
                status="PASS", completed_tps=100 if method == "arbor" else 50,
                avg_latency_s=0.1, p50_s=0.05, p95_s=0.2, p99_s=0.3)


class MethodDispatch(unittest.TestCase):
    def test_binary_selection_and_unknown_method(self):
        self.assertEqual(c.binary_for_method(), c.BIN)
        self.assertEqual(c.binary_for_method("saguaro"), c.ROOT / "build/bin/saguaro_node")
        with self.assertRaises(ValueError):
            c.binary_for_method("typo")

    def test_legacy_manifest_is_arbor_and_explicit_mismatch_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            run = Path(folder)
            c.write(run / "manifest.json", {"nodes": []})
            self.assertEqual(c.run_binary(run), c.BIN)
            with self.assertRaises(ValueError):
                c.run_binary(run, "saguaro")
            c.write(run / "manifest.json", {"nodes": [], "method": "saguaro"})
            self.assertEqual(c.run_binary(run), c.binary_for_method("saguaro"))
            with self.assertRaises(ValueError):
                c.run_binary(run, "arbor")

    def test_only_matching_managed_binary_and_node_directory_are_owned(self):
        node = dict(pid=123, directory="/run/shard1/node0", binary=str(c.binary_for_method("saguaro")))
        command = node["binary"] + " node /run/config.json 1 0 " + node["directory"]
        with mock.patch.object(c.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, command, "")):
            self.assertTrue(c.is_our_process(node))
            self.assertFalse(c.is_our_process(dict(node, binary="/another/checkout/saguaro_node")))
            self.assertFalse(c.is_our_process(dict(node, directory="/other/run/shard1/node0")))
            self.assertFalse(c.is_our_process(dict(node, directory="/run/shard1/node")))

    def test_load_infers_saguaro_client_binary(self):
        with tempfile.TemporaryDirectory() as folder:
            run = Path(folder)
            cfg = c.validate(c.read(ROOT / "config/two_layer.json"))
            c.write(run / "config.json", cfg)
            c.write(run / "manifest.json", {"method": "saguaro", "nodes": []})
            output = run / "client.json"
            def finish(args, **kwargs):
                c.write(args[-1], {"executed_transactions": 25})
                return subprocess.CompletedProcess(args, 0)
            with mock.patch.object(c.subprocess, "run", side_effect=finish) as call:
                rc, _ = c.load(run, count=25, rate=1000, participants=[1, 2], batch=8, output=output)
            self.assertEqual(rc, 0)
            self.assertEqual(call.call_args[0][0][0], str(c.binary_for_method("saguaro")))
            self.assertEqual(sum(len(req["txs"]) for req in c.read(output.with_suffix(".workload.json"))["requests"]), 25)

    def test_clean_blocks_saguaro_active_client_and_comparator(self):
        pid = 987654
        command = str(ROOT / "build/bin/saguaro_node") + " client " + str(ROOT / "runtime/run/config.json")
        with self.assertRaises(ValueError):
            cleaner.assert_no_writers(ROOT, {pid: command})
        command = "python3 " + str(ROOT / "baseline/compare.py")
        with self.assertRaises(ValueError):
            cleaner.assert_no_writers(ROOT, {pid: command})
        with mock.patch.object(cleaner, "process_cwd", return_value=ROOT):
            with self.assertRaises(ValueError):
                cleaner.assert_no_writers(ROOT, {pid: "python3 baseline/compare.py --count 20"})


class ExactWorkload(unittest.TestCase):
    def setUp(self):
        self.cfg = c.validate(c.read(ROOT / "config/two_layer.json"))
        self.case = case()
        self.workload = c.prepare_workload(self.cfg, 25, 1000, 42, "same-ids", participants=[1, 2], batch=8, timeout=30)

    def test_identical_workload_hash_includes_ids_keys_values_and_rate(self):
        self.assertEqual(b.workload_fingerprint(self.workload), b.workload_fingerprint(copy.deepcopy(self.workload)))
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "workload.json"
            c.write(path, self.workload)
            import hashlib
            self.assertEqual(b.workload_fingerprint(self.workload), hashlib.sha256(path.read_bytes()).hexdigest())
        for field, value in (("id", "different-id"), ("value", 7), ("key", "account:1:changed")):
            changed = copy.deepcopy(self.workload)
            changed["requests"][0]["txs"][0][field] = value
            self.assertNotEqual(b.workload_fingerprint(self.workload), b.workload_fingerprint(changed))
        changed = copy.deepcopy(self.workload)
        changed["rate"] = 2000
        self.assertNotEqual(b.workload_fingerprint(self.workload), b.workload_fingerprint(changed))

    def test_reused_workload_must_match_case_and_have_unique_ids(self):
        b.validate_workload(self.case, self.cfg, self.workload, 30)
        for mutation in (
            lambda w: w.update(rate=1), lambda w: w.update(seed=1), lambda w: w.update(timeout_s=1),
            lambda w: w["requests"][0].update(target=1),
            lambda w: w["requests"][1].update(id=w["requests"][0]["id"]),
            lambda w: w["requests"][1]["txs"][0].update(id=w["requests"][0]["txs"][0]["id"]),
            lambda w: w["requests"][0]["txs"][0].update(participants=[1, 3]),
            lambda w: w["requests"].pop(),
        ):
            changed = copy.deepcopy(self.workload)
            mutation(changed)
            with self.subTest(workload=changed), self.assertRaises(ValueError):
                b.validate_workload(self.case, self.cfg, changed, 30)

    def test_method_groups_never_merge_in_benchmark_aggregates(self):
        rows = [result("arbor"), result("saguaro")]
        self.assertEqual(len(b.group_rows(rows)), 2)
        old = dict(rows[0]); del old["method"]
        self.assertEqual(b.comparison_key(old), b.comparison_key(rows[0]))

    def test_ratio_requires_identical_actual_workload_and_two_complete_results(self):
        arbor, saguaro = result("arbor"), result("saguaro")
        pair = comparison.paired_comparison(arbor, saguaro)
        self.assertTrue(pair["comparable"])
        self.assertEqual(pair["arbor_over_saguaro_tps"], 2)
        for changes in ({"workload_sha256": "different"}, {"config_fingerprint": "different"},
                        {"status": "FAIL"}, {"completed_tps": 0}, {"p95_s": float("nan")},
                        {"method": "arbor"}, {"participants": [1, 3]}, {"batch": 10}):
            with self.subTest(changes=changes):
                pair = comparison.paired_comparison(arbor, dict(saguaro, **changes))
                self.assertFalse(pair["comparable"])
                self.assertNotIn("arbor_over_saguaro_tps", pair)

    def test_one_failed_repeat_suppresses_whole_group_ratio(self):
        pair = comparison.paired_comparison(result("arbor"), result("saguaro"))
        failed = dict(pair, repeat=2, comparable=False, reason="incomplete")
        group = comparison.summarize_pairs([pair, failed])[0]
        self.assertFalse(group["comparable"])
        self.assertNotIn("arbor_over_saguaro_tps", group)

    def test_partial_comparison_report_cannot_claim_pass(self):
        arbor, saguaro = result("arbor"), result("saguaro")
        report = dict(expected_pairs=1, cases=[arbor], method_comparisons=[], aggregates=[])
        with tempfile.TemporaryDirectory() as folder, mock.patch.object(b, "save_report"):
            comparison.save(Path(folder), report)
            self.assertEqual(report["status"], "INCOMPLETE")
            report["cases"].append(saguaro)
            report["method_comparisons"].append(comparison.paired_comparison(arbor, saguaro))
            comparison.save(Path(folder), report)
            self.assertEqual(report["status"], "PASS")
            report["cases"][-1].update(status="FAIL", failure_reasons=["unsettled"])
            comparison.save(Path(folder), report)
            self.assertEqual(report["status"], "FAIL")

    def test_saguaro_requires_no_open_2pc_or_locks(self):
        # Synthetic rows test the drain decision independently of real PBFT.
        rows = []
        for shard in [1, 2, 5]:
            for replica in range(4):
                rows.append(dict(shard=shard, replica=replica, alive=True, ready=True,
                    state_digest=f"s{shard}", kv_digest=f"k{shard}", chain_digest=f"c{shard}", applied_batches=3,
                    executed_transactions=25 if shard in [1, 2] else 0,
                    ordered_cst_transactions=25 if shard == 5 else 0,
                    completed_cst_transactions=25 if shard == 5 else 0,
                    sag_active_batches=0, sag_pending_prepares=0, sag_pending_decisions=0, sag_held_locks=0, sag_pending_completions=0))
        case_saguaro = dict(self.case, method="saguaro")
        self.assertTrue(b.settled(case_saguaro, self.cfg, rows))
        for field in ("sag_active_batches", "sag_pending_prepares", "sag_pending_decisions", "sag_held_locks", "sag_pending_completions"):
            altered = copy.deepcopy(rows); altered[0][field] = 1
            self.assertFalse(b.settled(case_saguaro, self.cfg, altered))
            altered = copy.deepcopy(rows); del altered[0][field]
            self.assertFalse(b.settled(case_saguaro, self.cfg, altered))
        # Traditional 2PC contacts its NCA and leaves, so unrelated internal
        # Arbor round-close counters do not impose extra baseline work.
        rows[-1].update(rounds_requested=9, cst_round=0)
        self.assertTrue(b.settled(case_saguaro, self.cfg, rows))


class InterruptedComparisonReports(unittest.TestCase):
    def make_report(self, repeats, expected_pairs):
        rows, pairs = [], []
        for repeat in range(1, repeats + 1):
            current = []
            for method in ("arbor", "saguaro"):
                row = dict(result(method), repeat=repeat, node_metrics={}, failure_reasons=[])
                rows.append(row)
                current.append(row)
            pairs.append(comparison.paired_comparison(*current))
        return dict(baseline="saguaro", expected_pairs=expected_pairs, cases=rows,
                    method_comparisons=pairs, comparisons=[])

    def test_interrupted_repeat_set_has_no_aggregate_or_paired_medians(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            report = self.make_report(1, 3)
            comparison.save(folder, report)
            saved = c.read(folder / "summary.json")
            self.assertEqual(saved["status"], "INCOMPLETE")
            self.assertEqual([row["status"] for row in saved["cases"]], ["PASS", "PASS"])
            for aggregate in saved["aggregates"]:
                self.assertEqual(aggregate["status"], "INCOMPLETE")
                for metric in b.METRICS:
                    self.assertIsNone(aggregate["median_" + metric])
            for pair in saved["paired_aggregates"]:
                self.assertFalse(pair["comparable"])
                self.assertFalse(any(key.startswith(("median_", "arbor_over_")) for key in pair))
            self.assertIn("INCOMPLETE", (folder / "summary.md").read_text())
            self.assertNotIn("100.00", (folder / "summary.md").read_text())

    def test_completed_repeat_set_preserves_final_pass_metrics(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            report = self.make_report(2, 2)
            comparison.save(folder, report)
            saved = c.read(folder / "summary.json")
            self.assertEqual(saved["status"], "PASS")
            self.assertEqual(saved["paired_aggregates"][0]["arbor_over_saguaro_tps"], 2)
            metrics = {row["method"]: row for row in saved["aggregates"]}
            self.assertEqual(metrics["arbor"]["median_completed_tps"], 100)
            self.assertEqual(metrics["saguaro"]["median_completed_tps"], 50)

    def test_failed_completed_repeat_set_keeps_failure_and_blank_ratio(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            report = self.make_report(2, 2)
            failed = report["cases"][-1]
            failed.update(status="FAIL", failure_reasons=["waiting locks"])
            failed.update({metric: None for metric in b.METRICS})
            report["method_comparisons"][-1] = comparison.paired_comparison(report["cases"][-2], failed)
            comparison.save(folder, report)
            saved = c.read(folder / "summary.json")
            self.assertEqual(saved["status"], "FAIL")
            aggregate = next(row for row in saved["aggregates"] if row["method"] == "saguaro")
            self.assertEqual(aggregate["status"], "FAIL")
            self.assertIsNone(aggregate["median_completed_tps"])
            self.assertFalse(saved["paired_aggregates"][0]["comparable"])
            self.assertNotIn("arbor_over_saguaro_tps", saved["paired_aggregates"][0])


if __name__ == "__main__":
    unittest.main(verbosity=2)
