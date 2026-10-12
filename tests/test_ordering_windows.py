#!/usr/bin/env python3
"""Certified ordering-window safety and a small multi-coordinator real-PBFT run."""
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import cluster as c
import benchmark_mixed as mixed

UNIT_BIN = ROOT / "build/bin/test_ordering_windows"


class OrderingWindowLogic(unittest.TestCase):
    def test_authentication_completeness_ordering_backpressure_and_recovery(self):
        self.assertTrue(UNIT_BIN.is_file(), "build first with make build/bin/test_ordering_windows")
        with tempfile.TemporaryDirectory(prefix="arbor-ordering-window-") as directory:
            output = Path(directory)
            keys, nodes = output / "keys", output / "nodes"
            keys.mkdir(mode=0o700)
            nodes.mkdir()
            cfg = c.validate(c.read(ROOT / "config/three_layer_locality.json"))
            cfg["consensus"].update(ordering_window_blocks=2, empty_block_interval_ms=20,
                batch_size=1000, cross_shard_batch_size=100, batch_wait_ms=1,
                cross_shard_batch_wait_ms=10, view_timeout_ms=3000, checkpoint_batches=16)
            cfg.update(method="arbor", run_id=uuid.uuid4().hex)
            cfg["client_private_key"], cfg["client_public_key"] = c.keypair(keys, "client")
            cfg["nodes"] = []
            for shard in cfg["shards"]:
                for replica in range(4):
                    sid = shard["id"]
                    private, public = c.keypair(keys, f"s{sid}-n{replica}")
                    cfg["nodes"].append(dict(shard=sid, replica=replica, host="127.0.0.1",
                        port=32000 + len(cfg["nodes"]), private_key=private, public_key=public,
                        directory=str(nodes)))
            source = output / "config.json"
            c.write(source, cfg)
            process = subprocess.run([str(UNIT_BIN), str(source), str(nodes)],
                capture_output=True, text=True, timeout=60)
            self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
            report = json.loads(process.stdout)
            report_path = ROOT / "test-results" / ("ordering-windows-unit-" + uuid.uuid4().hex[:10] + ".json")
            report_path.parent.mkdir(parents=True, exist_ok=True)
            c.write(report_path, report)
            print(f"Ordering window checks={len(report['checks'])}; report={report_path}", flush=True)
            self.assertFalse(report["network_started"])
            self.assertEqual(report["m_values"], [1, 2, 4, 12])
            self.assertGreaterEqual(len(report["checks"]), 40)
            for name in (
                "a closing certificate cannot substitute for a missing manifest business order",
                "ascending windows take precedence over coordinator depth in a later window",
                "view change preserves the committed window order without applying an incomplete witness",
                "m greater than eight closes with real empty PBFT blocks despite outstanding business backpressure",
                "certified checkpoint catch-up preserves all committed window manifest history",
                "a complete head first becoming eligible after long idle receives one fresh PBFT deadline",
                "replayed closing proofs and repeated ready-head polls cannot prolong an existing deadline",
                "readiness activation cannot reset an ongoing certified view-change deadline",
            ):
                self.assertIn(name, report["checks"])


class OrderingWindowLive(unittest.TestCase):
    def test_multilayer_windows_m1_m2_m4_complete_and_have_identical_leaf_order(self):
        self.assertTrue(c.BIN.is_file(), "build first with make build/bin/arbor_node")
        folder = ROOT / "test-results" / ("ordering-windows-" + uuid.uuid4().hex[:10])
        folder.mkdir(parents=True)
        raw = c.read(ROOT / "config/three_layer_locality.json")
        raw["shards"] = [{"id": sid, "parent": parent} for sid, parent in
                         [(1, 5), (2, 5), (3, 6), (4, 6), (5, 7), (6, 7), (7, None)]]
        raw["network"].update(intra_shard_delay_ms=1, default_inter_shard_delay_ms=3,
                              shard_links=[], trace=False)
        raw["consensus"].update(batch_size=32, cross_shard_batch_size=4,
            batch_wait_ms=2, cross_shard_batch_wait_ms=10, empty_block_interval_ms=20,
            view_timeout_ms=5000, checkpoint_batches=4)
        cfg = c.validate(raw)
        requests = []
        for i, ps in enumerate(([1, 2], [1, 3], [3, 4])):
            requests.extend(c.prepare_workload(cfg, 8, 1000, 42+i, f"windows:{i}",
                participants=ps, batch=2, timeout=45)["requests"])
        # Interleave different coordinators; all m values replay these exact
        # transaction IDs, participant sets, account keys and arrival order.
        ordered = []
        for index in range(4):
            for group in range(3):
                ordered.append(copy.deepcopy(requests[4*group+index]))
        # Deliberately overlap each leaf's account across coordinator streams.
        # This exercises the order consistency that motivated the new protocol.
        for request in ordered:
            for tx in request["txs"]:
                for access in tx["accesses"]:
                    access["key"] = f"account:{access['shard']}:window-shared"
                tx["key"] = tx["accesses"][0]["key"]
        workload = dict(rate=1000, timeout_s=45, seed=42, requests=ordered)
        workload_path = folder / "workload.json"
        c.write(workload_path, workload)
        for m in (1, 2, 4):
            with self.subTest(m=m):
                raw["consensus"]["ordering_window_blocks"] = m
                config_path = folder / f"m{m}.json"
                c.write(config_path, raw)
                result = mixed.run(config_path, workload_path, folder / f"m{m}",
                                   "arbor", drain_timeout=30)
                self.assertEqual(result["status"], "PASS", result["failure_reasons"])
                self.assertEqual(result["client"]["executed_transactions"], 24)
                self.assertTrue(result["window_idle_pause"]["paused"])
                run = Path(result["run_dir"])
                leaf_batches = {}
                for leaf in (1, 2, 3, 4):
                    replica_batches = []
                    for replica in range(4):
                        certs = []
                        for line in (run / f"shard{leaf}/node{replica}/commits.jsonl").read_text().splitlines():
                            entry = json.loads(line)
                            pp = entry["certificate"]["proposal"]["body"]
                            self.assertGreaterEqual(len({v["body"]["from"] for v in entry["certificate"]["commits"]}), 3)
                            value = pp["value"]
                            if "cst_orders" not in value:
                                continue
                            order = value["cst_orders"][0]["proposal"]["body"]
                            co, block = order["shard"], order["value"]["cst_round"]
                            window = (block-1)//m+1
                            depth = 0 if co == 7 else 1
                            certs.append((window, depth, co, block, order["seq"]))
                            frontier = value["cst_frontier"]
                            self.assertEqual({p["proposal"]["body"]["shard"] for p in frontier},
                                             {7, 5 if leaf in (1, 2) else 6})
                            for close in frontier:
                                closing = close["proposal"]["body"]
                                self.assertEqual(closing["value"]["cst_round"], window*m)
                                manifest = closing["value"]["cst_window_manifest"][str(leaf)]
                                self.assertEqual(manifest["end"]-manifest["start"], len(manifest["orders"]))
                        self.assertEqual(certs, sorted(certs), "leaf execution must follow window then coordinator depth")
                        replica_batches.append(certs)
                    self.assertTrue(all(certs == replica_batches[0] for certs in replica_batches))
                    leaf_batches[leaf] = replica_batches[0]
                # Every root batch involves leaves 1 and 3. Their root order
                # identities and window assignment must be exactly the same.
                self.assertEqual([r for r in leaf_batches[1] if r[2] == 7],
                                 [r for r in leaf_batches[3] if r[2] == 7])


class OrderingWindowFourLayerLive(unittest.TestCase):
    def test_three_coordinator_depths_conflicting_accounts_complete_in_consistent_order(self):
        self.assertTrue(c.BIN.is_file(), "build first with make build/bin/arbor_node")
        folder = ROOT / "test-results" / ("ordering-windows-four-layer-" + uuid.uuid4().hex[:10])
        folder.mkdir(parents=True)
        raw = c.read(ROOT / "config/four_layer.json")
        raw["network"].update(intra_shard_delay_ms=1, default_inter_shard_delay_ms=3,
                              shard_links=[], trace=False)
        raw["consensus"].update(batch_size=32, cross_shard_batch_size=4,
            batch_wait_ms=2, cross_shard_batch_wait_ms=10, empty_block_interval_ms=50,
            ordering_window_blocks=2, view_timeout_ms=5000, checkpoint_batches=4)
        cfg = c.validate(raw)
        groups = ([1, 2], [1, 2, 3], [1, 2, 8])
        parts = [c.prepare_workload(cfg, 8, 1000, 81+index, f"four-layer:{index}",
            participants=participants, batch=2, timeout=60)["requests"]
                 for index, participants in enumerate(groups)]
        requests = [copy.deepcopy(parts[group][index])
                    for index in range(4) for group in range(3)]
        for request in requests:
            for tx in request["txs"]:
                for access in tx["accesses"]:
                    access["key"] = f"account:{access['shard']}:four-layer-shared"
                tx["key"] = tx["accesses"][0]["key"]
        # Leaves 1 and 2 participate in every batch, so the three coordinator
        # streams both conflict and must have an identical complete order there.
        self.assertEqual({request["target"] for request in requests}, {5, 7, 9})
        workload = dict(rate=1000, timeout_s=60, seed=81, requests=requests)
        config_path, workload_path = folder / "config.json", folder / "workload.json"
        c.write(config_path, raw)
        c.write(workload_path, workload)
        result = mixed.run(config_path, workload_path, folder / "result", "arbor", drain_timeout=30)
        self.assertEqual(result["status"], "PASS", result["failure_reasons"])
        client = result["client"]
        self.assertEqual(client["executed_transactions"], 24)
        self.assertEqual(client["completed_requests"], 12)
        for field in ("errors", "duplicate_transactions", "ordered_only_transactions"):
            self.assertEqual(client[field], 0)
        self.assertTrue(result["window_idle_pause"]["paused"])
        run = Path(result["run_dir"])
        statuses = c.read(folder / "result/status-after.json")
        for shard in raw["shards"]:
            peers = [row for row in statuses if row["shard"] == shard["id"]]
            self.assertEqual(len(peers), 4)
            for field in ("state_digest", "kv_digest", "chain_digest", "applied_batches"):
                self.assertEqual(len({row[field] for row in peers}), 1,
                                 f"shard {shard['id']} {field} must converge")
            for row in peers:
                self.assertFalse(row["changing_view"])
                for field in ("pending_requests", "pending_cst_batches", "staged_cst_batches",
                              "dedup_waiting_requests", "network_queue", "network_buffered_bytes"):
                    self.assertEqual(row[field], 0, f"shard {shard['id']} {field} must drain")
        leaf_orders = {}
        input_ids = {tx["id"] for request in requests for tx in request["txs"]}
        for leaf in (1, 2):
            replicas = []
            for replica in range(4):
                order, observed, version = [], set(), 0
                for line in (run / f"shard{leaf}/node{replica}/commits.jsonl").read_text().splitlines():
                    entry = json.loads(line)
                    self.assertGreaterEqual(len({vote["body"]["from"]
                        for vote in entry["certificate"]["commits"]}), 3)
                    value = entry["certificate"]["proposal"]["body"]["value"]
                    if "cst_orders" not in value:
                        continue
                    body = value["cst_orders"][0]["proposal"]["body"]
                    coordinator, block = body["shard"], body["value"]["cst_round"]
                    window, depth = (block-1)//2+1, {9: 0, 7: 1, 5: 2}[coordinator]
                    order.append((window, depth, coordinator, block, body["seq"]))
                    frontier = value["cst_frontier"]
                    self.assertEqual({close["proposal"]["body"]["shard"] for close in frontier}, {5, 7, 9})
                    for close in frontier:
                        closing = close["proposal"]["body"]
                        self.assertEqual(closing["value"]["cst_round"], 2*window)
                        manifest = closing["value"]["cst_window_manifest"][str(leaf)]
                        self.assertEqual(manifest["end"]-manifest["start"], len(manifest["orders"]))
                        self.assertGreaterEqual(len({vote["body"]["from"] for vote in close["commits"]}), 3)
                    mine = next(proof["record"] for proof in entry["execution_witness"]["proofs"]
                                if proof["record"]["shard"] == leaf)
                    self.assertEqual(mine["reads"][f"account:{leaf}:four-layer-shared"]["version"], version)
                    for write in mine["writes"]:
                        self.assertFalse(write["duplicate"])
                        self.assertNotIn(write["id"], observed)
                        observed.add(write["id"])
                        version += 1
                self.assertEqual(order, sorted(order), "execution must respect window then ancestor depth")
                self.assertEqual({row[2] for row in order}, {5, 7, 9})
                self.assertEqual(observed, input_ids)
                self.assertEqual(version, 24, "the one conflicting account must receive exactly 24 version updates")
                replicas.append(order)
            self.assertTrue(all(order == replicas[0] for order in replicas))
            leaf_orders[leaf] = replicas[0]
        self.assertEqual(leaf_orders[1], leaf_orders[2],
                         "all conflicting root/intermediate/lower batches must share one order at both leaves")


if __name__ == "__main__":
    unittest.main(verbosity=2)
