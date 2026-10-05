#!/usr/bin/env python3
"""Check mixed-LCA benchmark accounting and diagnostics without a cluster."""
import contextlib
import copy
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT / "baseline"))
import cluster as c
import benchmark_mixed as b
import compare_mixed


def fixture():
    cfg = c.validate(c.read(ROOT / "config/three_layer_cross100.json"))
    workload = {"rate": 5000, "timeout_s": 180, "seed": 42, "requests": []}
    for index, (ps, count) in enumerate((([1, 2], 30), ([1, 3], 30), ([2, 3], 30), ([1, 2, 3], 10))):
        part = c.prepare_workload(cfg, count, 5000, 42 + index, f"group-{index}",
                                  participants=ps, batch=10, timeout=180)
        workload["requests"].extend(part["requests"])
    expected = b.expectations(cfg, workload)
    rows = []
    for sid in c.topology(cfg)[0]:
        for replica in range(4):
            rows.append({"shard": sid, "replica": replica, "method": "saguaro",
                         "ready": True, "alive": True, "changing_view": False,
                         "executed_transactions": expected["executed"][sid],
                         "ordered_cst_transactions": expected["ordered"][sid],
                         "completed_cst_transactions": expected["ordered"][sid],
                         "applied_batches": 2, "state_digest": f"state-{sid}",
                         "chain_digest": f"chain-{sid}", "kv_digest": f"kv-{sid}",
                         "pending_requests": 0, "pending_cst_batches": 0, "staged_cst_batches": 0,
                         "dedup_waiting_requests": 0, "network_queue": 0, "network_buffered_bytes": 0,
                         "sag_active_batches": 0, "sag_pending_prepares": 0, "sag_pending_decisions": 0,
                         "sag_pending_completions": 0, "sag_held_locks": 0,
                         "rounds_requested": 0, "cst_round": 0})
    return cfg, workload, expected, rows


class MixedAccounting(unittest.TestCase):
    def setUp(self):
        self.cfg, self.workload, self.expected, self.rows = fixture()

    def test_90_10_mix_has_distinct_leaf_and_lca_counts(self):
        self.assertEqual(self.expected["transactions"], 100)
        self.assertEqual(self.expected["requests"], 10)
        self.assertEqual(self.expected["participants_per_transaction"], {"2": 90, "3": 10})
        self.assertEqual(self.expected["groups"], {"1,2": 30, "1,3": 30, "2,3": 30, "1,2,3": 10})
        self.assertEqual(self.expected["executed"], {1: 70, 2: 70, 3: 70, 4: 0, 5: 0, 6: 0, 7: 0})
        self.assertEqual(self.expected["ordered"], {1: 0, 2: 0, 3: 0, 4: 0, 5: 30, 6: 0, 7: 70})
        self.assertEqual(b.settlement_errors(self.cfg, self.expected, self.rows, "saguaro"), [])

    def test_client_total_does_not_replace_each_leaf_count(self):
        for row in self.rows:
            if row["shard"] == 1:
                row["executed_transactions"] = 100
        self.assertTrue(b.settlement_errors(self.cfg, self.expected, self.rows, "saguaro"))

    def test_wrong_lca_is_rejected_even_when_root_is_an_ancestor(self):
        self.workload["requests"][0]["target"] = 7
        with self.assertRaises(ValueError):
            b.expectations(self.cfg, self.workload)

    def test_duplicate_transactions_and_requests_are_rejected(self):
        for duplicate in ("request", "transaction"):
            with self.subTest(duplicate=duplicate):
                workload = copy.deepcopy(self.workload)
                if duplicate == "request":
                    workload["requests"][1]["id"] = workload["requests"][0]["id"]
                else:
                    workload["requests"][1]["txs"][0]["id"] = workload["requests"][0]["txs"][0]["id"]
                with self.assertRaises(ValueError):
                    b.expectations(self.cfg, workload)

    def test_malformed_workload_objects_raise_clear_validation_error(self):
        variants = [None, [], 1]
        for where, bad in (("request", None), ("transaction", 1), ("access", None), ("access-shard", [])):
            workload = copy.deepcopy(self.workload)
            if where == "request":
                workload["requests"][0] = bad
            elif where == "transaction":
                workload["requests"][0]["txs"][0] = bad
            elif where == "access":
                workload["requests"][0]["txs"][0]["accesses"][0] = bad
            else:
                workload["requests"][0]["txs"][0]["accesses"][0]["shard"] = bad
            variants.append(workload)
        for workload in variants:
            with self.subTest(workload=workload), self.assertRaises(ValueError):
                b.expectations(self.cfg, workload)

    def test_incomplete_or_malformed_node_rows_fail_without_crashing(self):
        variants = [None, {}, [], self.rows[:-1]]
        for field, value in (("shard", []), ("replica", {}), ("state_digest", {}),
                             ("applied_batches", "2"), ("executed_transactions", True),
                             ("ready", []), ("changing_view", None), ("sag_held_locks", -1)):
            rows = copy.deepcopy(self.rows)
            rows[0][field] = value
            variants.append(rows)
        rows = copy.deepcopy(self.rows); rows[0] = None; variants.append(rows)
        rows = copy.deepcopy(self.rows); rows[0] = {}; variants.append(rows)
        rows = copy.deepcopy(self.rows); rows[0] = copy.deepcopy(rows[1]); variants.append(rows)
        rows = copy.deepcopy(self.rows); del rows[0]["sag_pending_completions"]; variants.append(rows)
        for index, rows in enumerate(variants):
            with self.subTest(index=index):
                self.assertTrue(b.settlement_errors(self.cfg, self.expected, rows, "saguaro"))

    def test_execution_completion_and_replica_digests_all_matter(self):
        for field, value in (("state_digest", "other"), ("kv_digest", "other"),
                             ("chain_digest", "other"), ("applied_batches", 3),
                             ("executed_transactions", 69), ("alive", False), ("changing_view", True)):
            rows = copy.deepcopy(self.rows); rows[0][field] = value
            with self.subTest(field=field):
                self.assertTrue(b.settlement_errors(self.cfg, self.expected, rows, "saguaro"))
        for field in ("ordered_cst_transactions", "completed_cst_transactions"):
            rows = copy.deepcopy(self.rows)
            next(row for row in rows if row["shard"] == 7)[field] = 69
            with self.subTest(field=field):
                self.assertTrue(b.settlement_errors(self.cfg, self.expected, rows, "saguaro"))

    def test_remaining_locks_2pc_or_network_queues_prevent_pass(self):
        for field in ("sag_active_batches", "sag_pending_prepares", "sag_pending_decisions",
                      "sag_pending_completions", "sag_held_locks", "pending_requests",
                      "pending_cst_batches", "staged_cst_batches", "dedup_waiting_requests",
                      "network_queue", "network_buffered_bytes"):
            rows = copy.deepcopy(self.rows); rows[0][field] = 1
            with self.subTest(field=field):
                self.assertTrue(b.settlement_errors(self.cfg, self.expected, rows, "saguaro"))

    def test_arbor_outstanding_round_also_prevents_pass(self):
        for row in self.rows:
            row["method"] = "arbor"
        self.assertEqual(b.settlement_errors(self.cfg, self.expected, self.rows, "arbor"), [])
        next(row for row in self.rows if row["shard"] == 7)["rounds_requested"] = 1
        self.assertTrue(b.settlement_errors(self.cfg, self.expected, self.rows, "arbor"))

    def test_wait_requires_two_consecutive_settled_snapshots(self):
        bad = copy.deepcopy(self.rows); bad[0]["sag_held_locks"] = 1
        snapshots = [self.rows, bad, self.rows, self.rows]
        with mock.patch.object(b.c, "statuses", side_effect=snapshots) as statuses, mock.patch.object(b.time, "sleep"):
            okay, rows = b.wait_settled(self.cfg, self.expected, Path("unused"), "saguaro", 5)
        self.assertTrue(okay)
        self.assertEqual(rows, self.rows)
        self.assertEqual(statuses.call_count, 4)

    def test_saved_source_config_can_be_validated_again_by_cluster_start(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source, workload, binary = root / "config.json", root / "workload.json", root / "node"
            c.write(source, c.read(ROOT / "config/three_layer_cross100.json"))
            c.write(workload, self.workload)
            binary.write_text("placeholder; no binary execution in this unit test")
            output = root / "output"

            def validate_saved_config(path, run, method):
                saved = c.read(path)
                self.assertNotIn("resolved_links", saved["network"],
                                 "computed validation fields must not enter source-config.json")
                self.assertEqual(saved["base_port"], 45000)
                c.validate(saved)
                raise RuntimeError("mocked cluster start; validation succeeded")

            with mock.patch.object(b.c, "binary_for_method", return_value=binary), \
                 mock.patch.object(b, "free_ports", return_value=45000), \
                 mock.patch.object(b.c, "start", side_effect=validate_saved_config), \
                 self.assertRaisesRegex(RuntimeError, "mocked cluster start"):
                b.run(source, workload, output, "saguaro")
            self.assertTrue((output / "summary.json").is_file())
            self.assertNotIn("resolved_links", c.read(source)["network"], "input config must remain unchanged")


class LocalityAccounting(unittest.TestCase):
    def setUp(self):
        self.arbor = c.validate(c.read(ROOT / "config/three_layer_locality.json"))
        self.ahl = c.validate(c.read(ROOT / "config/ahl_two_layer_locality.json"))
        self.workload = compare_mixed.prepare_mixed_workload(self.arbor, 200, 1000, 10, 42, 30)

    def test_all_methods_report_actual_same_locality_using_reference_clusters(self):
        for method in ("arbor", "saguaro", "sharper", "ahl"):
            with self.subTest(method=method):
                cfg = self.ahl if method == "ahl" else self.arbor
                expected = b.expectations(cfg, self.workload, method)
                self.assertEqual(expected["participants_per_transaction"], {"2": 180, "3": 20})
                locality = expected["locality"]
                self.assertEqual(locality["clusters"], {"5": [1, 2, 8], "6": [3, 4, 9]})
                self.assertEqual(locality["cross_cluster_transactions"], 10)
                self.assertEqual(locality["intra_cluster_transactions"], 190)
                self.assertEqual(locality["cross_cluster_ratio"], .05)
                self.assertEqual(sum(expected["executed"].values()), 420)
                if method == "ahl":
                    self.assertEqual(expected["ordered"][7], 200)
                elif method == "sharper":
                    self.assertEqual(sum(expected["ordered"].values()), 0)
                else:
                    self.assertEqual(expected["ordered"][7], 10)
                    self.assertEqual(expected["ordered"][5] + expected["ordered"][6], 190)

    def test_metadata_cannot_hide_nonlocal_transactions_or_change_reference_parents(self):
        for mutation in ("counter", "clusters", "parent"):
            changed = copy.deepcopy(self.workload)
            if mutation == "counter":
                changed["locality"]["cross_cluster_transactions"] = 0
            elif mutation == "clusters":
                changed["locality"]["clusters"]["5"] = [1, 2, 3]
            else:
                changed["locality"]["reference_parent"]["8"] = 6
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                b.expectations(self.arbor, changed)

    def test_legacy_workload_without_metadata_keeps_original_accounting(self):
        cfg, workload, _, _ = fixture()
        self.assertNotIn("locality", b.expectations(cfg, workload))


class VisibleClientProgress(unittest.TestCase):
    def test_progress_is_relayed_once_with_partial_lines_preserved(self):
        with tempfile.TemporaryDirectory() as folder:
            log = Path(folder) / "client.log"
            log.write_text("")
            process = mock.Mock(args=["client"])
            chunks = iter(["progress submitted=10 comple", "ted=0\ninternal detail\nprogress submitted=20 completed=10\n",
                           "completed=20 ordered_only=0\n"])
            calls = 0

            def wait(timeout):
                nonlocal calls
                calls += 1
                with log.open("a") as stream:
                    stream.write(next(chunks))
                if calls < 3:
                    raise subprocess.TimeoutExpired(process.args, timeout)
                return 0

            process.wait.side_effect = wait
            output = io.StringIO()
            with contextlib.redirect_stdout(output), mock.patch.object(b.time, "monotonic", side_effect=[0, 0, 5, 10]):
                rc = b.wait_client(process, log, 12)
            self.assertEqual(rc, 0)
            self.assertEqual([call.kwargs["timeout"] for call in process.wait.call_args_list], [5, 5, 2])
            self.assertEqual(output.getvalue().splitlines(), ["progress submitted=10 completed=0",
                "progress submitted=20 completed=10", "completed=20 ordered_only=0"])
            self.assertIn("internal detail", log.read_text(), "the audit log must retain all client output")

    def test_timeout_remains_bounded_with_progress_polling(self):
        with tempfile.TemporaryDirectory() as folder:
            log = Path(folder) / "client.log"; log.write_text("")
            process = mock.Mock(args=["client"])
            process.wait.side_effect = subprocess.TimeoutExpired(process.args, 1)
            with mock.patch.object(b.time, "monotonic", side_effect=[0, 0, 2]), self.assertRaises(subprocess.TimeoutExpired):
                b.wait_client(process, log, 1)
            process.wait.assert_called_once_with(timeout=1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
