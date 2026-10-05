#!/usr/bin/env python3
"""SharPer method dispatch, settlement, and fair paired comparison checks."""
import contextlib
import copy
import hashlib
import io
import json
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
import benchmark as b
import benchmark_mixed as mixed
import clean
import compare
import compare_mixed


QUEUES = ("sharper_active_batches", "sharper_pending_batches", "sharper_waiting_execution")


def fixture():
    cfg = c.validate(c.read(ROOT / "config/three_layer_cross100.json"))
    workload = compare_mixed.prepare_mixed_workload(cfg, 100, 1000, 10, 42, 30)
    expected = mixed.expectations(cfg, workload, "sharper")
    rows = []
    for sid in c.topology(cfg)[0]:
        for replica in range(4):
            row = dict(shard=sid, replica=replica, method="sharper", alive=True, ready=True,
                       changing_view=False, executed_transactions=expected["executed"][sid],
                       ordered_cst_transactions=0, completed_cst_transactions=0, applied_batches=2,
                       state_digest=f"state-{sid}", kv_digest=f"kv-{sid}", chain_digest=f"chain-{sid}",
                       pending_requests=0, pending_cst_batches=0, staged_cst_batches=0,
                       dedup_waiting_requests=0, network_queue=0, network_buffered_bytes=0)
            row.update({field: 0 for field in QUEUES})
            rows.append(row)
    return cfg, workload, expected, rows


def result(method, repeat=1):
    return dict(mode="cross", count=100, rate=1000, batch=10, seed=42, repeat=repeat,
                shard=None, participants=[1, 2], method=method, config_fingerprint="same-config",
                workload_sha256="same-workload", status="PASS", completed_tps=100 if method == "arbor" else 50,
                avg_latency_s=.1, p50_s=.05, p95_s=.2, p99_s=.3)


class DispatchAndCleanup(unittest.TestCase):
    def test_sharper_binary_and_manifest_dispatch(self):
        self.assertEqual(c.binary_for_method("sharper"), ROOT / "build/bin/sharper_node")
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)
            c.write(run / "manifest.json", dict(method="sharper", nodes=[]))
            self.assertEqual(c.run_binary(run), c.binary_for_method("sharper"))
            with self.assertRaises(ValueError):
                c.run_binary(run, "arbor")

    def test_load_selects_sharper_and_preserves_unsigned_nca_target(self):
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)
            cfg = c.validate(c.read(ROOT / "config/two_layer.json"))
            c.write(run / "config.json", cfg)
            c.write(run / "manifest.json", dict(method="sharper", nodes=[]))
            output = run / "client.json"
            def finish(argv, **kwargs):
                c.write(argv[-1], dict(executed_transactions=25))
                return subprocess.CompletedProcess(argv, 0)
            with mock.patch.object(c.subprocess, "run", side_effect=finish) as call, contextlib.redirect_stdout(io.StringIO()):
                rc, _ = c.load(run, count=25, rate=1000, participants=[1, 2], batch=8, output=output)
            self.assertEqual(rc, 0)
            self.assertEqual(call.call_args.args[0][0], str(c.binary_for_method("sharper")))
            job = c.read(output.with_suffix(".workload.json"))
            self.assertEqual({request["target"] for request in job["requests"]}, {c.lca(cfg, [1, 2])})

    def test_cleanup_blocks_sharper_clients_and_new_comparator(self):
        commands = (str(ROOT / "build/bin/sharper_node") + " client " + str(ROOT / "runtime/run/config.json"),
                    "python3 " + str(ROOT / "baseline/compare_mixed.py"),
                    "python3 baseline/compare_mixed.py --baseline sharper", "python3 tests/test_sharper_tools.py")
        with mock.patch.object(clean, "process_cwd", return_value=ROOT):
            for command in commands:
                with self.subTest(command=command), self.assertRaises(ValueError):
                    clean.assert_no_writers(ROOT, {987654: command})


class CoordinatorFreeSettlement(unittest.TestCase):
    def setUp(self):
        self.cfg, self.workload, self.expected, self.rows = fixture()

    def test_mixed_execution_counts_remain_per_leaf_without_coordinator_work(self):
        self.assertEqual(self.expected["participants_per_transaction"], {"2": 90, "3": 10})
        self.assertEqual(self.expected["executed"][1], 70)
        self.assertTrue(all(value == 0 for value in self.expected["ordered"].values()))
        self.assertEqual(mixed.settlement_errors(self.cfg, self.expected, self.rows, "sharper"), [])
        # Preserve Arbor/NCA expectations on the exact same unsigned input.
        self.assertGreater(sum(mixed.expectations(self.cfg, self.workload)["ordered"].values()), 0)
        self.assertEqual(mixed.settlement_errors(self.cfg, mixed.expectations(self.cfg, self.workload), self.rows, "sharper"), [])

    def test_queues_and_coordinator_counts_cannot_be_hidden(self):
        for field, value in [(field, 1) for field in QUEUES] + [
                ("ordered_cst_transactions", 1), ("completed_cst_transactions", 1),
                ("executed_transactions", 100), ("network_queue", 1)]:
            rows = copy.deepcopy(self.rows)
            rows[0][field] = value
            with self.subTest(field=field):
                self.assertTrue(mixed.settlement_errors(self.cfg, self.expected, rows, "sharper"))
        for field in QUEUES:
            for bad in (None, True, -1, "0"):
                rows = copy.deepcopy(self.rows)
                rows[-1][field] = bad
                with self.subTest(field=field, bad=bad):
                    self.assertTrue(mixed.settlement_errors(self.cfg, self.expected, rows, "sharper"))

    def test_single_group_benchmark_requires_sharper_drain_and_zero_nca_counts(self):
        cfg = c.validate(c.read(ROOT / "config/two_layer.json"))
        case = dict(mode="cross", count=25, participants=[1, 2], shard=None, method="sharper")
        rows = [copy.deepcopy(row) for row in self.rows if row["shard"] in (1, 2, 5)]
        for row in rows:
            row["executed_transactions"] = 25 if row["shard"] in (1, 2) else 0
            row.update(rounds_requested=9, cst_round=0, sag_held_locks=9)
        self.assertEqual(b.settlement_errors(case, cfg, rows), [])
        for field in QUEUES:
            changed = copy.deepcopy(rows); del changed[0][field]
            self.assertTrue(b.settlement_errors(case, cfg, changed))
            changed = copy.deepcopy(rows); changed[0][field] = 1
            self.assertTrue(b.settlement_errors(case, cfg, changed))
        rows[-1]["completed_cst_transactions"] = 25
        self.assertTrue(b.settlement_errors(case, cfg, rows))

    def test_unsigned_leaf_target_is_rejected_before_replay(self):
        self.workload["requests"][0]["target"] = self.workload["requests"][0]["txs"][0]["participants"][0]
        with self.assertRaises(ValueError):
            mixed.expectations(self.cfg, self.workload, "sharper")


class FairComparisons(unittest.TestCase):
    def test_sharper_ratio_is_arbor_divided_by_sharper_and_failures_are_excluded(self):
        arbor, sharper = result("arbor"), result("sharper")
        pair = compare.paired_comparison(arbor, sharper, "sharper")
        self.assertEqual(pair["arbor_over_sharper_tps"], 2)
        self.assertNotIn("arbor_over_saguaro_tps", pair)
        for changes in (dict(status="FAIL"), dict(workload_sha256="other"),
                        dict(config_fingerprint="other"), dict(p95_s=float("nan")), dict(method="saguaro")):
            failed = compare.paired_comparison(arbor, dict(sharper, **changes), "sharper")
            self.assertFalse(failed["comparable"])
            self.assertNotIn("arbor_over_sharper_tps", failed)
            groups = compare.summarize_pairs([pair, dict(failed, repeat=2)])
            self.assertFalse(groups[0]["comparable"])
            self.assertNotIn("arbor_over_sharper_tps", groups[0])

    def test_old_saguaro_fields_remain_and_baselines_form_separate_groups(self):
        old = compare.paired_comparison(result("arbor"), result("saguaro"))
        self.assertEqual(old["arbor_over_saguaro_tps"], 2)
        self.assertEqual(old["saguaro_tps"], 50)
        new = compare.paired_comparison(result("arbor"), result("sharper"), "sharper")
        self.assertEqual(len(compare.summarize_pairs([old, new])), 2)

    def test_generation_is_deterministic_with_90_10_and_original_nca_targets(self):
        cfg, workload, _, _ = fixture()
        self.assertEqual(workload, compare_mixed.prepare_mixed_workload(cfg, 100, 1000, 10, 42, 30))
        self.assertNotEqual(workload, compare_mixed.prepare_mixed_workload(cfg, 100, 1000, 10, 43, 30))
        self.assertEqual(mixed.expectations(cfg, workload)["groups"], {"1,2": 30, "1,3": 30, "2,3": 30, "1,2,3": 10})
        self.assertEqual(len({request["id"] for request in workload["requests"]}), len(workload["requests"]))

    def test_mixed_entrypoint_reuses_shared_file_and_alternates_methods(self):
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            binary = temp / "node"; binary.touch()
            output = temp / "results"
            seen = []
            def run(config, job, folder, method, **kwargs):
                seen.append((method, Path(job), hashlib.sha256(Path(job).read_bytes()).hexdigest()))
                cfg, workload = c.validate(c.read(config)), c.read(job)
                expected = mixed.expectations(cfg, workload, method)
                return dict(status="PASS", method=method, expected=expected, client=dict(
                    executed_transactions=expected["transactions"], completed_requests=expected["requests"],
                    requests=expected["requests"], elapsed_s=1), node_metrics=dict(bytes_sent=100),
                    workload_sha256=seen[-1][2], config_fingerprint=b.config_fingerprint(cfg),
                    completed_tps=100 if method == "arbor" else 50,
                    avg_latency_s=.1, p50_s=.05, p95_s=.2, p99_s=.3, failure_reasons=[])
            with mock.patch.object(c, "binary_for_method", return_value=binary), \
                    mock.patch.object(mixed, "run", side_effect=run), contextlib.redirect_stdout(io.StringIO()):
                rc = compare_mixed.main(["--count", "100", "--rate", "1000", "--batch", "10",
                                         "--repeat", "2", "--skip-build", "--output-dir", str(output)])
            self.assertEqual(rc, 0)
            self.assertEqual([item[0] for item in seen], ["arbor", "sharper", "sharper", "arbor"])
            self.assertEqual(len({item[1] for item in seen}), 1)
            self.assertEqual(len({item[2] for item in seen}), 1)
            report = c.read(output / "summary.json")
            self.assertEqual(report["status"], "PASS")
            self.assertEqual(report["paired_aggregates"][0]["arbor_over_sharper_tps"], 2)
            self.assertIn("SharPer", (output / "summary.md").read_text())

    def test_external_workload_is_preserved_and_failed_repeat_suppresses_ratios(self):
        cfg, workload, _, _ = fixture()
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            binary = temp / "node"; binary.touch()
            job = temp / "original.json"
            original = json.dumps(workload, separators=(",", ":")).encode()
            job.write_bytes(original)
            output = temp / "results"
            seen = []
            def run(config, shared, folder, method, **kwargs):
                current = c.read(shared)
                self.assertEqual(current, workload)
                seen.append((method, hashlib.sha256(Path(shared).read_bytes()).hexdigest()))
                expected = mixed.expectations(cfg, current, method)
                failed = len(seen) == 3  # SharPer runs first on the second repeat.
                metrics = {field: (None if failed else .1) for field in b.METRICS}
                metrics["completed_tps"] = None if failed else 100
                return dict(metrics, method=method, status="FAIL" if failed else "PASS", expected=expected,
                            client=dict(executed_transactions=100, requests=10, completed_requests=10, elapsed_s=1),
                            config_fingerprint=b.config_fingerprint(cfg), workload_sha256=seen[-1][1],
                            node_metrics=dict(bytes_sent=100), failure_reasons=["queue did not drain"] if failed else [])
            with mock.patch.object(c, "binary_for_method", return_value=binary), \
                    mock.patch.object(mixed, "run", side_effect=run), contextlib.redirect_stdout(io.StringIO()):
                rc = compare_mixed.main(["--workload", str(job), "--repeat", "2", "--skip-build",
                                         "--output-dir", str(output)])
            self.assertEqual(rc, 2)
            self.assertEqual(job.read_bytes(), original)
            self.assertEqual(len({item[1] for item in seen}), 1)
            report = c.read(output / "summary.json")
            self.assertEqual(report["input_workload_sha256"], hashlib.sha256(original).hexdigest())
            self.assertEqual(report["status"], "FAIL")
            self.assertFalse(report["paired_aggregates"][0]["comparable"])
            self.assertNotIn("arbor_over_sharper_tps", report["paired_aggregates"][0])


if __name__ == "__main__":
    unittest.main(verbosity=2)
