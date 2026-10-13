#!/usr/bin/env python3
"""120 conflicting transactions under the certified 2-child/1-root schedule."""
import copy
import json
from pathlib import Path
import sys
import time
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import benchmark_guard as guards
import benchmark_mixed as mixed
import benchmark_quick as quick
import cluster as c


class RatioTwoLive(unittest.TestCase):
    def test_three_layers_conflicting_accounts_no_faults(self):
        folder = ROOT / "test-results" / ("ratio2-live-" + uuid.uuid4().hex[:10])
        folder.mkdir(parents=True)
        raw = c.read(ROOT / "config/three_layer_locality.json")
        raw["network"].update(intra_shard_delay_ms=1, default_inter_shard_delay_ms=3,
                              shard_links=[], trace=False)
        raw["consensus"].update(ordering_window_blocks=1, ancestor_streaming=True,
            ancestor_order="lower_first", ancestor_lower_blocks_per_root=2,
            empty_block_interval_ms=20, batch_size=32, cross_shard_batch_size=4,
            batch_wait_ms=2, cross_shard_batch_wait_ms=10, view_timeout_ms=5000,
            checkpoint_batches=4)
        cfg = c.validate(raw)
        groups = ([1, 2], [1, 2, 3], [3, 4])
        parts = [c.prepare_workload(cfg, 40, 1000, 91+i, f"ratio2-live:{i}",
                    participants=ps, batch=2, timeout=60)["requests"]
                 for i, ps in enumerate(groups)]
        requests = [copy.deepcopy(parts[group][index])
                    for index in range(20) for group in range(len(groups))]
        for request in requests:
            for tx in request["txs"]:
                for access in tx["accesses"]:
                    access["key"] = f"account:{access['shard']}:ratio2-shared"
                tx["key"] = tx["accesses"][0]["key"]
        config_path, workload_path = folder / "config.json", folder / "workload.json"
        c.write(config_path, raw)
        c.write(workload_path, dict(rate=1000, timeout_s=60, seed=91, requests=requests))
        guard = guards.RunGuard(overall_deadline=time.monotonic()+90, case_seconds=60,
                                no_progress_seconds=15)
        result = mixed.run(config_path, workload_path, folder / "result", "arbor",
                           drain_timeout=15, guard=guard)
        self.assertEqual(result["status"], "PASS", result["failure_reasons"])
        self.assertTrue(result["valid_performance_sample"])
        self.assertEqual(result["client"]["executed_transactions"], 120)
        statuses = c.read(folder / "result/status-after.json")
        for row in statuses:
            self.assertEqual(row["ancestor_order"], "lower_first")
            self.assertEqual(row["ancestor_lower_blocks_per_root"], 2)
            self.assertFalse(row["changing_view"])
            self.assertEqual(row["view_changes"], 0)
        run = Path(result["run_dir"])
        audit = quick.audit_leaf_order(run, cfg, "lower_2")
        c.write(folder / "order-audit.json", audit)
        self.assertEqual(audit["status"], "PASS")
        parent, leaves = c.topology(cfg)
        expected = {leaf: {tx["id"] for request in requests for tx in request["txs"]
                           if leaf in tx["participants"]} for leaf in leaves}
        orders = {}
        for leaf in leaves:
            ancestors, sid = [], parent[leaf]
            while sid is not None:
                ancestors.append(sid);sid = parent[sid]
            replica_orders = []
            for replica in range(cfg["replicas_per_shard"]):
                status = c.read(run / f"shard{leaf}/node{replica}/status.json")
                self.assertEqual(status["ancestor_coordinator_order"], ancestors)
                sequence, observed, version = [], set(), 0
                journal = run / f"shard{leaf}/node{replica}/commits.jsonl"
                for text in journal.read_text().splitlines():
                    entry = json.loads(text)
                    value = entry["certificate"]["proposal"]["body"]["value"]
                    if "cst_orders" not in value:
                        continue
                    body = value["cst_orders"][0]["proposal"]["body"]
                    position = quick.stream_position(ancestors, body["shard"],
                                                    body["value"]["cst_round"], True, 2)
                    sequence.append((position, body["value"]["cst_round"], body["shard"], body["seq"]))
                    mine = next(proof["record"] for proof in entry["execution_witness"]["proofs"]
                                if proof["record"]["shard"] == leaf)
                    self.assertEqual(mine["reads"][f"account:{leaf}:ratio2-shared"]["version"], version)
                    for write in mine["writes"]:
                        self.assertFalse(write["duplicate"])
                        self.assertNotIn(write["id"], observed)
                        observed.add(write["id"]);version += 1
                self.assertEqual(sequence, sorted(sequence))
                self.assertEqual(observed, expected[leaf])
                self.assertEqual(version, len(expected[leaf]))
                replica_orders.append(sequence)
            self.assertTrue(all(sequence == replica_orders[0] for sequence in replica_orders))
            orders[leaf] = replica_orders[0]
        for ps in groups:
            coordinator = c.lca(cfg, ps)
            common = [[(row[1], row[2], row[3]) for row in orders[leaf] if row[2] == coordinator]
                      for leaf in ps]
            self.assertTrue(all(sequence == common[0] for sequence in common))


if __name__ == "__main__":
    unittest.main(verbosity=2)
