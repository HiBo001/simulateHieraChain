#!/usr/bin/env python3
"""Arbor batching, early-proposal safety, and a small real-PBFT workload."""
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
import benchmark as b
import benchmark_mixed as mixed

UNIT_BIN = ROOT / "build/bin/test_arbor_batching"


class SelectionLogic(unittest.TestCase):
    def test_authenticated_selection_fairness_cache_and_recovery(self):
        self.assertTrue(UNIT_BIN.is_file(), "build first with make build/bin/test_arbor_batching")
        with tempfile.TemporaryDirectory(prefix="arbor-batching-") as directory:
            run = Path(directory)
            keys, node_directory = run / "keys", run / "node"
            keys.mkdir(mode=0o700)
            node_directory.mkdir()
            cfg = c.validate(c.read(ROOT / "config/three_layer_locality.json"))
            cfg["base_port"] = 31000
            cfg["consensus"].update(batch_size=1000, cross_shard_batch_size=100,
                                     batch_wait_ms=5, cross_shard_batch_wait_ms=600,
                                     view_timeout_ms=3000, checkpoint_batches=16)
            cfg.update(method="arbor", run_id=uuid.uuid4().hex)
            cfg["client_private_key"], cfg["client_public_key"] = c.keypair(keys, "client")
            cfg["nodes"] = []
            for shard in cfg["shards"]:
                for replica in range(4):
                    sid = shard["id"]
                    private, public = c.keypair(keys, f"s{sid}-n{replica}")
                    cfg["nodes"].append(dict(shard=sid, replica=replica, host="127.0.0.1",
                                             port=31000 + len(cfg["nodes"]), private_key=private,
                                             public_key=public, directory=str(node_directory)))
            config = run / "config.json"
            c.write(config, cfg)
            result = subprocess.run([str(UNIT_BIN), str(config), str(node_directory)],
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            report = json.loads(result.stdout)
            self.assertFalse(report["network_started"])
            self.assertGreaterEqual(len(report["checks"]), 40)
            self.assertIn("interleaved participant groups fill one 100-transaction batch", report["checks"])
            self.assertIn("real PBFT proposals rotate participant groups despite a continuously replenished first group",
                          report["checks"])
            self.assertIn("snapshot restoration invalidates stale selections and excludes already ordered requests",
                          report["checks"])
            self.assertIn("verified remote reads unblock the retained NCA frontier without proposal retransmission or SYNC",
                          report["checks"])
            self.assertIn("cached proposals cannot cancel an issued view change or grant a vote during recovery",
                          report["checks"])
            self.assertIn("signed messages that exceed the total byte budget are rejected without evicting the retained digest",
                          report["checks"])


class InterleavedLiveBatching(unittest.TestCase):
    def test_complete_cross_shard_execution_coalesces_interleaved_participants(self):
        self.assertTrue(c.BIN.is_file(), "build first with make build/bin/arbor_node")
        folder = ROOT / "test-results" / ("arbor-batching-" + uuid.uuid4().hex[:10])
        folder.mkdir(parents=True)
        raw = c.read(ROOT / "config/two_layer.json")
        raw["shards"] = [{"id": sid, "parent": 5} for sid in (1, 2, 3)] + [{"id": 5, "parent": None}]
        raw["network"]["shard_links"] = []
        raw["network"]["default_inter_shard_delay_ms"] = 3
        raw["network"]["intra_shard_delay_ms"] = 1
        raw["base_port"] = b.free_ports(raw["host"], 16)
        raw["consensus"].update(batch_size=1000, cross_shard_batch_size=100,
                                cross_shard_batch_wait_ms=600, batch_wait_ms=5,
                                view_timeout_ms=5000, checkpoint_batches=16)
        source = folder / "config.json"
        c.write(source, raw)
        run = None
        try:
            run = c.start(source, folder / "run", method="arbor")
            cfg = c.read(run / "config.json")
            parts = [c.prepare_workload(cfg, 100, 10000, 42 + index, f"batching:{index}",
                        participants=ps, batch=10, timeout=30)["requests"]
                     for index, ps in enumerate(([1, 2], [1, 3], [2, 3]))]
            requests = []
            for index in range(10):
                for group in range(3):
                    request = copy.deepcopy(parts[group][index])
                    request["id"] = f"interleaved:{len(requests):03d}"
                    requests.append(request)
            workload = dict(rate=10000, timeout_s=30, seed=42, requests=requests)
            expected = mixed.expectations(cfg, workload)
            job, output = folder / "workload.json", folder / "client.json"
            c.write(job, workload)
            process = subprocess.run([str(c.BIN), "client", str(run / "config.json"), str(job), str(output)],
                                     capture_output=True, text=True, timeout=40)
            self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
            client = c.read(output)
            self.assertEqual(client["executed_transactions"], 300)
            self.assertEqual(client["requests"], 30)
            self.assertEqual(client["completed_requests"], 30)
            for field in ("errors", "duplicate_transactions", "ordered_only_transactions"):
                self.assertEqual(client[field], 0)
            settled, statuses = mixed.wait_settled(cfg, expected, run, "arbor", 20)
            c.write(folder / "status-after.json", statuses)
            self.assertTrue(settled, mixed.settlement_errors(cfg, expected, statuses, "arbor"))
            original = {request["id"]: request for request in requests}
            seen_requests, batches = set(), []
            commits = run / "shard5/node0/commits.jsonl"
            for line in commits.read_text().splitlines():
                value = json.loads(line)["certificate"]["proposal"]["body"]["value"]
                carried = value["requests"]
                if not carried:
                    continue
                batches.append(sum(len(request["body"]["txs"]) for request in carried))
                participants = set()
                for envelope in carried:
                    body = envelope["body"]
                    self.assertNotIn(body["id"], seen_requests)
                    seen_requests.add(body["id"])
                    self.assertEqual(body["txs"], original[body["id"]]["txs"],
                                     "PBFT must carry each complete original signed client request")
                    participants.add(tuple(body["txs"][0]["participants"]))
                self.assertEqual(len(participants), 1)
            self.assertEqual(seen_requests, set(original))
            self.assertEqual(sum(batches), 300)
            self.assertLessEqual(max(batches), 100)
            self.assertGreaterEqual(batches.count(100), 2,
                "interleaved participant groups should coalesce into full business batches")
            c.write(folder / "validation.json", dict(status="PASS", transactions=300,
                    requests=30, business_batches=len(batches), batch_sizes=batches,
                    completed_tps=client["completed_tps"], avg_latency_s=client["avg_latency_s"],
                    p95_s=client["p95_s"]))
        finally:
            if run is not None:
                c.stop_run(run)


if __name__ == "__main__":
    unittest.main(verbosity=2)
