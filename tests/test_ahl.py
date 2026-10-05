#!/usr/bin/env python3
"""AHL's single root must run real 2PC/PBFT and preserve business semantics."""
import contextlib
import copy
import importlib.util
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import time
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("cluster_ahl", ROOT / "scripts/cluster.py")
c = importlib.util.module_from_spec(spec)
spec.loader.exec_module(c)
TEST_ROOT = ROOT / "test-results" / ("ahl-" + time.strftime("%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:8])
TWO_LAYER = [(1, 7), (2, 7), (3, 7), (4, 7), (7, None)]
MULTI_LAYER = [(1, 5), (2, 5), (3, 6), (4, 6), (5, 7), (6, 7), (7, None)]
SAG_QUEUES = ("sag_active_batches", "sag_pending_prepares", "sag_pending_decisions",
              "sag_pending_completions", "sag_held_locks")


def free_range(count):
    for base in range(33000, 59000, 47):
        held = []
        try:
            for port in range(base, base + count):
                sock = socket.socket()
                held.append(sock)
                sock.bind(("127.0.0.1", port))
            return base
        except OSError:
            pass
        finally:
            for sock in held:
                sock.close()
    raise RuntimeError("no free ports for AHL integration test")


def config(topology=TWO_LAYER):
    return {"replicas_per_shard": 4, "host": "127.0.0.1", "base_port": free_range(4 * len(topology)),
            "shards": [{"id": sid, "parent": parent} for sid, parent in topology],
            "consensus": {"batch_size": 8, "batch_wait_ms": 5, "cross_shard_batch_size": 8,
                          "cross_shard_batch_wait_ms": 10, "view_timeout_ms": 2000,
                          "checkpoint_batches": 4}, "execution": {"fib_iterations": 1},
            "network": {"intra_shard_delay_ms": 1, "default_inter_shard_delay_ms": 4,
                        "shard_links": [], "trace": False}}


@contextlib.contextmanager
def running(name):
    source = TEST_ROOT / (name + ".config.json")
    c.write(source, config())
    run = c.start(source, TEST_ROOT / name, method="ahl")
    try:
        yield run
    finally:
        c.stop_run(run)


def job(run, name, participants=None, shard=None, count=8, batch=4, shared=True):
    cfg = c.read(run / "config.json")
    workload = c.prepare_workload(cfg, count, 1000, 31415, name, participants=participants,
                                 shard=shard, batch=batch, timeout=45)
    if shared:
        for request in workload["requests"]:
            for tx in request["txs"]:
                if len(tx["participants"]) > 1:
                    for access in tx["accesses"]:
                        access["key"] = f"account:{access['shard']}:shared"
                    tx["key"] = tx["accesses"][0]["key"]
                else:
                    tx["key"] = f"account:{tx['participants'][0]}:shared"
    return workload


def client(run, name, workload):
    source, output = run / (name + ".workload.json"), run / (name + ".result.json")
    c.write(source, workload)
    rc = subprocess.run([str(c.run_binary(run)), "client", str(run / "config.json"),
                         str(source), str(output)], timeout=workload["timeout_s"] + 10).returncode
    return rc, c.read(output)


def assert_complete(case, rc, result, count):
    case.assertEqual(rc, 0, result)
    case.assertEqual(result["completed_requests"], result["requests"])
    case.assertEqual(result["executed_transactions"], count)
    case.assertEqual(result["errors"], 0)
    case.assertEqual(result["ordered_only_transactions"], 0)
    case.assertGreater(result["completed_tps"], 0)


def settled(run, workloads, alive_only=False, timeout=30):
    parent, leaves = c.topology(c.read(run / "config.json"))
    executed, root_ordered = {sid: set() for sid in parent}, set()
    for workload in workloads:
        for request in workload["requests"]:
            for tx in request["txs"]:
                for sid in tx["participants"]:
                    executed[sid].add(tx["id"])
                if len(tx["participants"]) > 1:
                    root_ordered.add(tx["id"])
    root = next(sid for sid, up in parent.items() if up is None)
    deadline, rows = time.monotonic() + timeout, []
    while time.monotonic() < deadline:
        rows = c.statuses(run)
        selected = [row for row in rows if row["alive"] or not alive_only]
        okay = True
        for row in selected:
            sid = row["shard"]
            if (not row["alive"] or row.get("changing_view") or row.get("method") != "ahl"
                    or row["executed_transactions"] != len(executed[sid])
                    or row["ordered_cst_transactions"] != (len(root_ordered) if sid == root else 0)
                    or (sid == root and row["completed_cst_transactions"] != len(root_ordered))
                    or any(row.get(field, 0) for field in SAG_QUEUES + (
                        "pending_requests", "dedup_waiting_requests", "pending_cst_batches", "staged_cst_batches"))):
                okay = False
        for sid in parent:
            peers = [row for row in selected if row["shard"] == sid]
            if len(peers) < 3 or (not alive_only and len(peers) != 4):
                okay = False
            if any(len({row[field] for row in peers}) != 1
                   for field in ("state_digest", "chain_digest", "kv_digest", "applied_batches")):
                okay = False
        if okay:
            return rows
        time.sleep(.1)
    raise AssertionError("AHL did not settle: " + json.dumps(rows))


def journal(run, shard, replica=0):
    path = run / f"shard{shard}" / f"node{replica}" / "commits.jsonl"
    return [json.loads(line) for line in path.read_text().splitlines()]


def assert_real_pbft(case, run, alive_only=False):
    for node in c.read(run / "manifest.json")["nodes"]:
        if alive_only and not c.is_our_process(node):
            continue
        for entry in journal(run, node["shard"], node["replica"]):
            certificate = entry["certificate"]
            proposal = certificate["proposal"]["body"]
            case.assertGreaterEqual(len({vote["body"]["from"] for vote in certificate["commits"]}), 3)
            case.assertGreaterEqual(len({vote["body"]["from"] for vote in certificate["prepares"]}), 2)
            for vote in certificate["commits"] + certificate["prepares"]:
                case.assertEqual(vote["body"]["shard"], node["shard"])
                case.assertEqual(vote["body"]["seq"], entry["seq"])
                case.assertEqual(vote["body"]["digest"], proposal["digest"])


def stop_node(run, shard, replica):
    node = next(node for node in c.read(run / "manifest.json")["nodes"]
                if (node["shard"], node["replica"]) == (shard, replica))
    if c.is_our_process(node):
        os.kill(node["pid"], signal.SIGTERM)
    deadline = time.monotonic() + 5
    while c.is_our_process(node) and time.monotonic() < deadline:
        time.sleep(.05)
    if c.is_our_process(node):
        raise AssertionError("failed to stop selected AHL forward")


class AHLIntegration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        TEST_ROOT.mkdir(parents=True, exist_ok=True)

    def test_two_leaf_root_and_real_two_phase_consensus(self):
        with running("two-leaf-phases") as run:
            workload = job(run, "two-leaf", [1, 2], count=16)
            self.assertEqual({request["target"] for request in workload["requests"]}, {7})
            assert_complete(self, *client(run, "load", workload), 16)
            rows = settled(run, [workload])
            assert_real_pbft(self, run)
            for sid in (1, 2, 7):
                values = [entry["certificate"]["proposal"]["body"]["value"] for entry in journal(run, sid)]
                self.assertEqual({value.get("sag_action") for value in values},
                                 {"INIT", "DECIDE"} if sid == 7 else {"PREPARE", "FINISH"})
                self.assertGreaterEqual(next(row["applied_batches"] for row in rows if row["shard"] == sid), 2)
                self.assertTrue(all("cst_orders" not in value and "cst_frontier" not in value for value in values))
            self.assertTrue(all(row["applied_batches"] == row["executed_transactions"] == 0
                                for row in rows if row["shard"] in (3, 4)))

    def test_all_pairs_and_three_participants_use_one_root(self):
        with running("single-coordinator") as run:
            workloads = []
            for index, participants in enumerate(([1, 2], [1, 3], [2, 3], [1, 2, 3])):
                workload = job(run, f"route-{index}", participants, count=8)
                self.assertEqual({request["target"] for request in workload["requests"]}, {7})
                workloads.append(workload)
                assert_complete(self, *client(run, f"load-{index}", workload), 8)
                rows = settled(run, workloads)
            self.assertTrue(all(row["ordered_cst_transactions"] == row["completed_cst_transactions"] == 32
                                for row in rows if row["shard"] == 7))
            self.assertTrue(all(row["executed_transactions"] == 24
                                for row in rows if row["shard"] in (1, 2, 3)))
            self.assertTrue(all(row["applied_batches"] == 0 for row in rows if row["shard"] == 4))
            assert_real_pbft(self, run)

    def test_original_multilayer_target_is_routed_without_changing_transactions(self):
        with running("original-multilayer-file") as run:
            original_cfg = c.validate(config(MULTI_LAYER))
            workload = c.prepare_workload(original_cfg, 8, 1000, 42, "shared-arbor-file",
                                         participants=[1, 2], batch=4, timeout=45)
            self.assertEqual({request["target"] for request in workload["requests"]}, {5})
            before = copy.deepcopy(workload)
            assert_complete(self, *client(run, "load", workload), 8)
            settled(run, [workload])
            self.assertEqual(workload, before)
            self.assertEqual(c.read(run / "load.workload.json"), before)
            received = [request["body"] for entry in journal(run, 7)
                        for value in [entry["certificate"]["proposal"]["body"]["value"]]
                        if value.get("sag_action") == "INIT" for request in value["requests"]]
            self.assertEqual({request["target"] for request in received}, {7})
            for original in before["requests"]:
                self.assertEqual(next(request["txs"] for request in received if request["id"] == original["id"]),
                                 original["txs"])
            assert_real_pbft(self, run)

    def test_binary_rejects_multilayer_before_loading_keys_or_binding(self):
        source, node_dir = TEST_ROOT / "invalid-direct.config.json", TEST_ROOT / "invalid-node"
        invalid = config(MULTI_LAYER)
        invalid["run_id"] = "invalid-direct-ahl"
        # Deliberately omit nodes/key paths: the method-specific topology
        # check must reject this before key I/O or a server can be started.
        c.write(source, invalid)
        result = subprocess.run([str(c.binary_for_method("ahl")), "node", str(source), "1", "0", str(node_dir)],
                                capture_output=True, text=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("AHL requires", result.stderr)
        self.assertNotIn("client_public_key", result.stderr)
        self.assertFalse(node_dir.exists())

    def test_intra_and_cross_retries_do_not_execute_twice(self):
        with running("intra-and-retry") as run:
            intra, cross = job(run, "intra", shard=1), job(run, "cross", [1, 2])
            assert_complete(self, *client(run, "intra", intra), 8)
            assert_complete(self, *client(run, "cross", cross), 8)
            before = settled(run, [intra, cross])
            # Saguaro's shared 2PC path answers already completed transaction
            # IDs as duplicates, including retries retaining the request ID.
            rc, result = client(run, "same-rid", cross)
            self.assertEqual(rc, 0, result)
            self.assertEqual(result["completed_requests"], result["requests"])
            self.assertEqual(result["executed_transactions"], 0)
            self.assertEqual(result["duplicate_transactions"], 8)
            self.assertEqual(result["errors"], 0)
            self.assertEqual(result["ordered_only_transactions"], 0)
            retried = settled(run, [intra, cross])
            snapshot_fields = ("shard", "replica", "kv_digest", "executed_transactions",
                               "state_digest", "chain_digest", "applied_batches")
            self.assertEqual([tuple(row[field] for field in snapshot_fields) for row in before],
                             [tuple(row[field] for field in snapshot_fields) for row in retried],
                             "same request retries must not execute or consume additional PBFT slots")
            alias = copy.deepcopy(cross)
            for request in alias["requests"]:
                request["id"] += ":alias"
            rc, result = client(run, "alias", alias)
            self.assertEqual(rc, 0, result)
            self.assertEqual(result["completed_requests"], result["requests"])
            self.assertEqual(result["executed_transactions"] + result["duplicate_transactions"], 8)
            after = settled(run, [intra, cross])
            self.assertEqual([tuple(row[field] for field in snapshot_fields) for row in before],
                             [tuple(row[field] for field in snapshot_fields) for row in after])
            assert_real_pbft(self, run)

    def test_root_and_participant_forward_failure_recovers(self):
        with running("forward-failure") as run:
            before = job(run, "before", [1, 2], count=4)
            assert_complete(self, *client(run, "before", before), 4)
            settled(run, [before])
            stop_node(run, 7, 0)
            stop_node(run, 1, 0)
            after = job(run, "after", [1, 2], count=8)
            assert_complete(self, *client(run, "after", after), 8)
            rows = settled(run, [before, after], alive_only=True)
            self.assertTrue(all(row["view"] >= 1 for row in rows if row["alive"] and row["shard"] in (1, 7)))
            assert_real_pbft(self, run, alive_only=True)


if __name__ == "__main__":
    unittest.main(verbosity=2)
