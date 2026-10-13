#!/usr/bin/env python3
"""Authenticated fixed-prefix streaming on three- and four-layer trees."""
import copy
import json
import os
from pathlib import Path
import sys
import signal
import threading
import unittest
import uuid
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import cluster as c
import benchmark_mixed as mixed
import benchmark_ordering_windows as windows


class StreamingConfiguration(unittest.TestCase):
    def test_flag_requires_boolean_multilayer_windows_and_is_arbor_only(self):
        original = c.read(ROOT / "config/three_layer_locality.json")
        source = copy.deepcopy(original)
        source["consensus"].update(ordering_window_blocks=1, ancestor_streaming=True)
        normalized = c.validate(source)
        self.assertTrue(c.validate_method_config(normalized, "arbor")["consensus"]["ancestor_streaming"])
        for method in ("saguaro", "sharper"):
            selected = c.validate_method_config(normalized, method)
            self.assertEqual(selected, c.validate(original))
        self.assertEqual(windows.baseline_config(source), original)
        for bad in (1, "true", None):
            changed = copy.deepcopy(source)
            changed["consensus"]["ancestor_streaming"] = bad
            with self.assertRaises(ValueError): c.validate(changed)
        changed = copy.deepcopy(source)
        changed["consensus"]["ordering_window_blocks"] = 0
        with self.assertRaises(ValueError): c.validate(changed)


class StreamingLive(unittest.TestCase):
    def run_tree(self, filename, groups, m, fail_primary=False):
        folder = ROOT / "test-results" / ("ancestor-streaming-" + uuid.uuid4().hex[:10])
        folder.mkdir(parents=True)
        source = c.read(ROOT / "config" / filename)
        source["network"].update(intra_shard_delay_ms=1, default_inter_shard_delay_ms=3,
                                  shard_links=[], trace=False)
        source["consensus"].update(ordering_window_blocks=m, ancestor_streaming=True,
            empty_block_interval_ms=20, batch_size=32, cross_shard_batch_size=4,
            batch_wait_ms=2, cross_shard_batch_wait_ms=10, view_timeout_ms=5000, checkpoint_batches=4)
        if fail_primary: source["consensus"]["view_timeout_ms"] = 1500
        cfg = c.validate(source)
        parts = [c.prepare_workload(cfg, 8, 1000, 81+i, f"stream-live:{i}", participants=ps,
                                   batch=2, timeout=60)["requests"] for i, ps in enumerate(groups)]
        requests = [copy.deepcopy(parts[group][index]) for index in range(4) for group in range(len(groups))]
        for request in requests:
            for tx in request["txs"]:
                for access in tx["accesses"]:
                    access["key"] = f"account:{access['shard']}:stream-shared"
                tx["key"] = tx["accesses"][0]["key"]
        config, job = folder / "config.json", folder / "workload.json"
        c.write(config, source)
        c.write(job, dict(rate=1000, timeout_s=60, seed=81, requests=requests))
        if fail_primary:
            original_start = c.start
            paused, timers = [], []
            def resume(node):
                if c.is_our_process(node): os.kill(node["pid"], signal.SIGCONT)
            def start_with_failure(*args, **kwargs):
                run = original_start(*args, **kwargs)
                manifest = c.read(run / "manifest.json")
                root = next(sid for sid, parent in c.topology(cfg)[0].items() if parent is None)
                node = next(node for node in manifest["nodes"] if node["shard"] == root and node["replica"] == 0)
                os.kill(node["pid"], signal.SIGSTOP);paused.append(node)
                timer = threading.Timer(3, lambda: resume(node))
                timer.daemon = True;timer.start();timers.append(timer)
                return run
            try:
                with mock.patch.object(c, "start", side_effect=start_with_failure):
                    result = mixed.run(config, job, folder / "result", "arbor", drain_timeout=30)
            finally:
                for timer in timers: timer.cancel()
                for node in paused: resume(node)
        else:
            result = mixed.run(config, job, folder / "result", "arbor", drain_timeout=30)
        self.assertEqual(result["status"], "PASS", result["failure_reasons"])
        self.assertEqual(result["client"]["executed_transactions"], 8 * len(groups))
        if fail_primary:
            statuses = c.read(folder / "result/status-after.json")
            self.assertTrue(any(row["view_changes"] > 0 for row in statuses))
        parent, leaves = c.topology(cfg)
        run = Path(result["run_dir"])
        orders = {}
        for leaf in leaves:
            ancestors = []
            sid = parent[leaf]
            while sid is not None:
                ancestors.append(sid);sid = parent[sid]
            ancestors.reverse()
            replicas = []
            for replica in range(4):
                sequence = []
                cursor = 0
                for text in (run / f"shard{leaf}/node{replica}/commits.jsonl").read_text().splitlines():
                    value = json.loads(text)["certificate"]["proposal"]["body"]["value"]
                    if "cst_frontier" not in value: continue
                    for proof in value["cst_frontier"]:
                        cursor += 1
                        body = proof["proposal"]["body"]
                        self.assertEqual(body["shard"], ancestors[(cursor-1) % len(ancestors)])
                        self.assertEqual(body["value"]["cst_round"], 1 + (cursor-1) // len(ancestors))
                    if "cst_orders" in value:
                        body = value["cst_orders"][0]["proposal"]["body"]
                        sequence.append((body["value"]["cst_round"], ancestors.index(body["shard"]),
                                         body["shard"], body["seq"]))
                self.assertEqual(sequence, sorted(sequence))
                self.assertEqual(c.read(run / f"shard{leaf}/node{replica}/status.json").get("ancestor_stream_cursor", 0), cursor)
                replicas.append(sequence)
            self.assertTrue(all(row == replicas[0] for row in replicas))
            orders[leaf] = replicas[0]
        # Every shared coordinator's business order has the same identity/order
        # at all participating leaves; the real dependency exchange completed.
        for ps in groups:
            coordinator = c.lca(cfg, ps)
            common = [[row for row in orders[leaf] if row[2] == coordinator] for leaf in ps]
            self.assertTrue(all([(row[0], row[2], row[3]) for row in sequence] ==
                                [(row[0], row[2], row[3]) for row in common[0]] for sequence in common))

    def test_three_layers_conflicting_accounts(self):
        self.run_tree("three_layer_locality.json", ([1, 2], [1, 3], [3, 4]), 1)

    def test_four_layers_and_m4_without_closing_window_barrier(self):
        self.run_tree("four_layer.json", ([1, 2], [1, 2, 3], [1, 2, 8]), 4)

    def test_coordinator_view_change_and_replica_catchup_preserve_stream_order(self):
        self.run_tree("three_layer_locality.json", ([1, 2], [1, 3], [3, 4]), 1, fail_primary=True)


if __name__ == "__main__":
    unittest.main(verbosity=2)
