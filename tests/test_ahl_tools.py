#!/usr/bin/env python3
"""AHL dispatch, topology constraints, settlement and shared-workload comparisons."""
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

QUEUES = ("sag_active_batches", "sag_pending_prepares", "sag_pending_decisions",
          "sag_pending_completions", "sag_held_locks")


def configs():
    return (c.validate(c.read(ROOT / "config/three_layer_cross100.json")),
            c.validate(c.read(ROOT / "config/ahl_two_layer.json")))


def fixture():
    arbor, cfg = configs()
    workload = compare_mixed.prepare_mixed_workload(arbor, 100, 1000, 10, 42, 30)
    expected = mixed.expectations(cfg, workload, "ahl")
    rows = []
    for sid in c.topology(cfg)[0]:
        for replica in range(4):
            row = dict(shard=sid, replica=replica, method="ahl", alive=True, ready=True, changing_view=False,
                       executed_transactions=expected["executed"][sid], ordered_cst_transactions=expected["ordered"][sid],
                       completed_cst_transactions=expected["ordered"][sid], applied_batches=2,
                       state_digest=f"state-{sid}", kv_digest=f"kv-{sid}", chain_digest=f"chain-{sid}",
                       pending_requests=0, pending_cst_batches=0, staged_cst_batches=0,
                       dedup_waiting_requests=0, network_queue=0, network_buffered_bytes=0)
            row.update({field: 0 for field in QUEUES})
            rows.append(row)
    return cfg, workload, expected, rows


def result(method, repeat=1, fingerprint="same-config", sha="same-workload"):
    return dict(mode="cross", count=100, rate=1000, batch=10, seed=42, repeat=repeat,
                shard=None, participants=[1, 2], method=method, config_fingerprint=fingerprint,
                workload_sha256=sha, status="PASS", completed_tps=100 if method == "arbor" else 50,
                avg_latency_s=.1, p50_s=.05, p95_s=.2, p99_s=.3)


class DispatchAndTopology(unittest.TestCase):
    def test_ahl_binary_manifest_and_load_dispatch(self):
        self.assertIn("ahl", c.METHODS)
        self.assertEqual(c.binary_for_method("ahl"), ROOT / "build/bin/ahl_node")
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)
            c.write(run / "manifest.json", dict(method="ahl", nodes=[]))
            c.write(run / "config.json", c.validate(c.read(ROOT / "config/two_layer.json")))
            self.assertEqual(c.run_binary(run), c.binary_for_method("ahl"))
            with self.assertRaises(ValueError):
                c.run_binary(run, "saguaro")
            output = run / "client.json"
            def finish(argv, **kwargs):
                c.write(argv[-1], dict(executed_transactions=25))
                return subprocess.CompletedProcess(argv, 0)
            with mock.patch.object(c.subprocess, "run", side_effect=finish) as call, contextlib.redirect_stdout(io.StringIO()):
                rc, _ = c.load(run, count=25, rate=1000, participants=[1, 2], batch=8, output=output)
            self.assertEqual(rc, 0)
            self.assertEqual(call.call_args.args[0][0], str(c.binary_for_method("ahl")))
            workload = c.read(output.with_suffix(".workload.json"))
            self.assertEqual({request["target"] for request in workload["requests"]}, {5})

    def test_only_one_upper_shard_with_at_least_two_direct_leaves_is_allowed(self):
        arbor, ahl = configs()
        self.assertEqual(c.validate_method_config(ahl, "ahl"), ahl)
        self.assertEqual(c.validate_method_config(c.validate(c.read(ROOT / "config/two_layer.json")), "ahl")["shards"],
                         c.validate(c.read(ROOT / "config/two_layer.json"))["shards"])
        self.assertEqual(c.validate_method_config(arbor, "arbor"), arbor)
        self.assertEqual(c.validate_method_config(arbor, "saguaro"), arbor)
        one_leaf = copy.deepcopy(ahl)
        one_leaf["shards"] = [{"id": 1, "parent": 7}, {"id": 7, "parent": None}]
        for bad in (arbor, one_leaf):
            with self.subTest(shards=bad["shards"]), self.assertRaises(ValueError):
                c.validate_method_config(bad, "ahl")

    def test_launcher_rejects_multilayer_before_creating_or_binding(self):
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            binary, source, run = temp / "node", temp / "config.json", temp / "run"
            binary.touch()
            c.write(source, c.read(ROOT / "config/three_layer_cross100.json"))
            with mock.patch.object(c, "binary_for_method", return_value=binary), \
                    mock.patch.object(c.socket, "socket") as socket_call, \
                    mock.patch.object(c.subprocess, "Popen") as launch, self.assertRaises(ValueError):
                c.start(source, run, "ahl")
            socket_call.assert_not_called()
            launch.assert_not_called()
            self.assertFalse(run.exists())

    def test_clean_recognizes_ahl_writers(self):
        commands = (str(ROOT / "build/bin/ahl_node") + " client " + str(ROOT / "runtime/run/config.json"),
                    "python3 baseline/compare_mixed.py --baseline ahl", "python3 tests/test_ahl.py")
        with mock.patch.object(clean, "process_cwd", return_value=ROOT):
            for command in commands:
                with self.subTest(command=command), self.assertRaises(ValueError):
                    clean.assert_no_writers(ROOT, {987654: command})


class AHLSettlement(unittest.TestCase):
    def setUp(self):
        self.cfg, self.workload, self.expected, self.rows = fixture()

    def test_multilayer_route_hints_count_only_the_single_ahl_root(self):
        self.assertEqual(self.expected["participants_per_transaction"], {"2": 90, "3": 10})
        self.assertEqual(self.expected["executed"][1], 70)
        self.assertEqual(self.expected["ordered"], {1: 0, 2: 0, 3: 0, 4: 0, 7: 100})
        self.assertEqual(mixed.settlement_errors(self.cfg, self.expected, self.rows, "ahl"), [])
        self.assertEqual({request["target"] for request in self.workload["requests"]}, {5, 7})
        for invalid in (None, True, -1, "5"):
            changed = copy.deepcopy(self.workload)
            changed["requests"][0]["target"] = invalid
            with self.subTest(target=invalid), self.assertRaises(ValueError):
                mixed.expectations(self.cfg, changed, "ahl")

    def test_every_2pc_queue_and_held_lock_blocks_settlement(self):
        for field in QUEUES:
            for value in (1, None, True, -1, "0"):
                rows = copy.deepcopy(self.rows)
                rows[0][field] = value
                with self.subTest(field=field, value=value):
                    self.assertTrue(mixed.settlement_errors(self.cfg, self.expected, rows, "ahl"))
            rows = copy.deepcopy(self.rows)
            del rows[0][field]
            with self.subTest(field=field, value="missing"):
                self.assertTrue(mixed.settlement_errors(self.cfg, self.expected, rows, "ahl"))
        changed = copy.deepcopy(self.rows)
        changed[-1]["completed_cst_transactions"] -= 1
        self.assertTrue(mixed.settlement_errors(self.cfg, self.expected, changed, "ahl"))

    def test_single_group_benchmark_requires_ahl_2pc_drain(self):
        case = dict(mode="cross", count=25, participants=[1, 2], shard=None, method="ahl")
        rows = copy.deepcopy(self.rows)
        for row in rows:
            row["executed_transactions"] = 25 if row["shard"] in (1, 2) else 0
            row["ordered_cst_transactions"] = row["completed_cst_transactions"] = 25 if row["shard"] == 7 else 0
        self.assertEqual(b.settlement_errors(case, self.cfg, rows), [])
        for field in QUEUES:
            for value in (1, None, True, -1, "0"):
                changed = copy.deepcopy(rows)
                changed[0][field] = value
                with self.subTest(field=field, value=value):
                    self.assertTrue(b.settlement_errors(case, self.cfg, changed))
            changed = copy.deepcopy(rows)
            del changed[0][field]
            self.assertTrue(b.settlement_errors(case, self.cfg, changed))

    def test_single_group_shared_multilayer_hint_is_only_accepted_for_ahl(self):
        arbor, ahl = configs()
        case = dict(mode="cross", count=25, participants=[1, 2], shard=None, method="ahl",
                    batch=10, rate=1000, seed=42)
        workload = c.prepare_workload(arbor, 25, 1000, 42, "shared-single", participants=[1, 2],
                                      batch=10, timeout=30)
        self.assertEqual({request["target"] for request in workload["requests"]}, {5})
        b.validate_workload(case, ahl, workload, 30)
        b.validate_workload(dict(case, method="arbor"), arbor, workload, 30)
        with self.assertRaises(ValueError):
            b.validate_workload(dict(case, method="saguaro"), ahl, workload, 30)


class AHLComparison(unittest.TestCase):
    def test_ahl_ratio_and_failed_round_handling(self):
        arbor, ahl = result("arbor"), result("ahl")
        pair = compare.paired_comparison(arbor, ahl, "ahl")
        self.assertEqual(pair["arbor_over_ahl_tps"], 2)
        self.assertEqual(pair["ahl_tps"], 50)
        for changes in (dict(status="FAIL"), dict(workload_sha256="other"),
                        dict(config_fingerprint="other"), dict(method="saguaro")):
            failed = compare.paired_comparison(arbor, dict(ahl, **changes), "ahl")
            self.assertFalse(failed["comparable"])
            self.assertNotIn("arbor_over_ahl_tps", failed)
            self.assertFalse(compare.summarize_pairs([pair, dict(failed, repeat=2)])[0]["comparable"])

    def test_dual_configuration_requires_explicit_validated_context(self):
        arbor_cfg, ahl_cfg = configs()
        context = compare.validate_comparison_configs(arbor_cfg, ahl_cfg, "ahl", separate_config=True)
        arbor = result("arbor", fingerprint=b.config_fingerprint(arbor_cfg))
        ahl = result("ahl", fingerprint=b.config_fingerprint(ahl_cfg))
        self.assertFalse(compare.paired_comparison(arbor, ahl, "ahl")["comparable"])
        for row in (arbor, ahl):
            row.update(comparison_group=context["comparison_group"], shared_workload_sha256="same-workload",
                       input_workload_sha256="same-workload")
        self.assertTrue(compare.paired_comparison(arbor, ahl, "ahl", comparison_context=context)["comparable"])
        changed = dict(ahl, shared_workload_sha256="other")
        self.assertFalse(compare.paired_comparison(arbor, changed, "ahl", comparison_context=context)["comparable"])

    def test_dual_configuration_checks_business_leafs_parameters_and_shared_delays(self):
        arbor, ahl = configs()
        for change in ("leaves", "execution", "consensus", "network", "common_link", "default_link"):
            changed = copy.deepcopy(ahl)
            if change == "leaves":
                changed["shards"] = [row for row in changed["shards"] if row["id"] != 4]
            elif change == "execution":
                changed["execution"]["fib_iterations"] += 1
            elif change == "consensus":
                changed["consensus"]["cross_shard_batch_size"] -= 1
            elif change == "network":
                changed["network"]["intra_shard_delay_ms"] += 1
            elif change == "common_link":
                changed["network"]["resolved_links"]["1:3"] += 1
            else:
                changed["network"]["default_inter_shard_delay_ms"] += 1
            with self.subTest(change=change), self.assertRaises(ValueError):
                compare.validate_comparison_configs(arbor, changed, "ahl", separate_config=True)
        with self.assertRaises(ValueError):
            compare.validate_comparison_configs(arbor, ahl, "saguaro", separate_config=True)

    def test_mixed_comparison_uses_identical_file_with_different_topologies(self):
        arbor, ahl_cfg = configs()
        workload = compare_mixed.prepare_mixed_workload(arbor, 100, 1000, 10, 42, 30)
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            binary, original_path, output = temp / "node", temp / "original.json", temp / "results"
            binary.touch()
            original = json.dumps(workload, separators=(",", ":")).encode()
            original_path.write_bytes(original)
            seen = []
            def run(config, shared, folder, method, **kwargs):
                cfg, current = c.validate(c.read(config)), c.read(shared)
                self.assertEqual(current, workload)
                seen.append((method, cfg, Path(shared), hashlib.sha256(Path(shared).read_bytes()).hexdigest()))
                expected = mixed.expectations(cfg, current, method)
                return dict(status="PASS", method=method, expected=expected,
                            client=dict(executed_transactions=100, requests=10, completed_requests=10, elapsed_s=1),
                            config_fingerprint=b.config_fingerprint(cfg), workload_sha256=seen[-1][3],
                            input_workload_sha256=seen[-1][3], node_metrics=dict(bytes_sent=100),
                            completed_tps=100 if method == "arbor" else 50,
                            avg_latency_s=.1, p50_s=.05, p95_s=.2, p99_s=.3, failure_reasons=[])
            with mock.patch.object(c, "binary_for_method", return_value=binary), \
                    mock.patch.object(mixed, "run", side_effect=run), contextlib.redirect_stdout(io.StringIO()):
                rc = compare_mixed.main(["--baseline", "ahl", "--config", str(ROOT / "config/three_layer_cross100.json"),
                                         "--baseline-config", str(ROOT / "config/ahl_two_layer.json"),
                                         "--workload", str(original_path), "--repeat", "2", "--skip-build",
                                         "--output-dir", str(output)])
            self.assertEqual(rc, 0)
            self.assertEqual([row[0] for row in seen], ["arbor", "ahl", "ahl", "arbor"])
            self.assertEqual(len({row[2] for row in seen}), 1)
            self.assertEqual(len({row[3] for row in seen}), 1)
            self.assertEqual(original_path.read_bytes(), original)
            for method, cfg, _, _ in seen:
                self.assertEqual(cfg["shards"], arbor["shards"] if method == "arbor" else ahl_cfg["shards"])
            report = c.read(output / "summary.json")
            self.assertEqual(report["input_workload_sha256"], hashlib.sha256(original).hexdigest())
            self.assertEqual(report["status"], "PASS")
            self.assertEqual(report["paired_aggregates"][0]["arbor_over_ahl_tps"], 2)
            self.assertIn("AHL", (output / "summary.md").read_text())

    def test_single_group_entrypoint_passes_shared_workload_and_dual_configuration(self):
        arbor_cfg, ahl_cfg = configs()
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            binary, output = temp / "node", temp / "results"
            binary.touch()
            seen = []
            def run_case(case, source, folder, timeout, drain_timeout, method="arbor", workload=None):
                cfg, current = c.validate(source), c.read(workload)
                b.validate_workload(dict(case, method=method), cfg, current, timeout)
                seen.append((method, cfg, Path(workload), hashlib.sha256(Path(workload).read_bytes()).hexdigest()))
                return dict(case, method=method, status="PASS", config_fingerprint=b.config_fingerprint(cfg),
                            workload_sha256=seen[-1][3], input_workload_sha256=seen[-1][3],
                            node_metrics=dict(bytes_sent=100),
                            completed_tps=100 if method == "arbor" else 50,
                            avg_latency_s=.1, p50_s=.05, p95_s=.2, p99_s=.3, failure_reasons=[])
            with mock.patch.object(c, "binary_for_method", return_value=binary), \
                    mock.patch.object(b, "run_case", side_effect=run_case), contextlib.redirect_stdout(io.StringIO()):
                rc = compare.main(["--baseline", "ahl", "--config", str(ROOT / "config/three_layer_cross100.json"),
                                   "--baseline-config", str(ROOT / "config/ahl_two_layer.json"),
                                   "--count", "100", "--rate", "1000", "--batch", "10", "--repeat", "1",
                                   "--timeout", "30", "--skip-build", "--output-dir", str(output)])
            self.assertEqual(rc, 0)
            self.assertEqual([row[0] for row in seen], ["arbor", "ahl"])
            self.assertEqual(seen[0][2:], seen[1][2:])
            self.assertEqual(seen[0][1]["shards"], arbor_cfg["shards"])
            self.assertEqual(seen[1][1]["shards"], ahl_cfg["shards"])
            self.assertEqual({request["target"] for request in c.read(seen[0][2])["requests"]}, {5})
            report = c.read(output / "summary.json")
            self.assertEqual(report["status"], "PASS")
            self.assertEqual(report["method_comparisons"][0]["arbor_over_ahl_tps"], 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
