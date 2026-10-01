#!/usr/bin/env python3
"""End-to-end two-leaf execution, dependency exchange and atomic visibility."""
import hashlib
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
spec = importlib.util.spec_from_file_location("cluster", ROOT / "scripts/cluster.py")
c = importlib.util.module_from_spec(spec)
spec.loader.exec_module(c)
TEST_ROOT = ROOT / "test-results" / ("stage2b-" + time.strftime("%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:8])
TEST_ROOT.mkdir(parents=True)


def free_range(count):
    for base in range(23000, 59000, 37):
        sockets = []
        try:
            for port in range(base, base + count):
                sock = socket.socket()
                sockets.append(sock)
                sock.bind(("127.0.0.1", port))
            return base
        except OSError:
            pass
        finally:
            for sock in sockets:
                sock.close()
    raise RuntimeError("no free test ports")


def packed(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def digest(value):
    return hashlib.sha256(value.encode()).hexdigest()


def wait_status(run, predicate, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        rows = c.statuses(run)
        if predicate(rows):
            return rows
        time.sleep(.1)
    raise AssertionError("cluster did not reach expected state: " + json.dumps(rows))


def config(name):
    raw = c.read(ROOT / "config/two_layer.json")
    raw["base_port"] = free_range(12)
    raw["consensus"] = {"batch_size": 4, "batch_wait_ms": 5,
                        "view_timeout_ms": 1200, "checkpoint_batches": 4}
    path = TEST_ROOT / (name + ".json")
    c.write(path, raw)
    return path


class CrossShardExecution(unittest.TestCase):
    def test_one_leaf_backup_offline_still_forms_quorum(self):
        run = c.start(config("one-backup-offline"), TEST_ROOT / "one-backup-offline")
        try:
            manifest = c.read(run / "manifest.json")
            victim = next(n for n in manifest["nodes"] if n["shard"] == 2 and n["replica"] == 3)
            os.kill(victim["pid"], signal.SIGTERM)
            time.sleep(.2)
            rc, result = c.load(run, count=4, rate=40, participants=[1, 2], batch=2, timeout=15)
            self.assertEqual(rc, 0)
            self.assertEqual(result["executed_transactions"], 4)
            rows = wait_status(run, lambda rows: sum(
                r["alive"] and r["executed_transactions"] == 4
                for r in rows if r["shard"] == 2) == 3)
            self.assertEqual(sum(r["alive"] for r in rows if r["shard"] == 2), 3)
        finally:
            c.stop_run(run)

    def test_remote_reads_change_writes_and_ack_completes_client(self):
        run = c.start(config("dependencies"), TEST_ROOT / "dependencies")
        try:
            cfg = c.read(run / "config.json")
            workload = c.prepare_workload(cfg, 2, 50, 7, "dependent", participants=[1, 2], batch=2, timeout=15)
            key1, key2 = "account:1:shared", "account:2:shared"
            for tx, values in zip(workload["requests"][0]["txs"], [(10, 20), (30, 40)]):
                tx["accesses"] = [{"shard": 1, "key": key1, "value": values[0]},
                                  {"shard": 2, "key": key2, "value": values[1]}]
                tx["key"], tx["value"] = key1, values[0]
            job, output = run / "dependent.workload.json", run / "dependent.result.json"
            c.write(job, workload)
            rc = subprocess.run([str(c.BIN), "client", str(run / "config.json"), str(job), str(output)],
                                check=False).returncode
            result = c.read(output)
            self.assertEqual(rc, 0)
            self.assertEqual(result["executed_transactions"], 2)
            self.assertEqual(result["ordered_only_transactions"], 0)
            self.assertEqual(result["completed_requests"], 1)
            self.assertEqual(len(result["timings"]), 2)
            self.assertTrue(all(t["latency_s"] > 0 for t in result["timings"]))

            rows = wait_status(run, lambda rows: all(
                sum(r["executed_transactions"] == 2 and r["staged_cst_batches"] == 0 and
                    r["finalized_cst_batches"] == 1 for r in rows if r["shard"] == leaf) == 4
                for leaf in (1, 2)))
            initial = {"version": 0, "value": 0, "digest": digest("initial")}
            a, b = initial, initial
            for tx in workload["requests"][0]["txs"]:
                v1, v2 = (access["value"] for access in tx["accesses"])
                a_next = {"version": a["version"] + 1, "value": v1 + b["value"] + 1,
                          "fib": 1, "digest": digest(packed(a) + packed(b) + packed(tx) + "1")}
                b_next = {"version": b["version"] + 1, "value": v2 + a["value"] + 1,
                          "fib": 1, "digest": digest(packed(b) + packed(a) + packed(tx) + "2")}
                a, b = a_next, b_next
            self.assertEqual((a["value"], b["value"]), (52, 52))
            for leaf, key, state in [(1, key1, a), (2, key2, b)]:
                leaf_rows = [r for r in rows if r["shard"] == leaf]
                self.assertEqual(len({r["state_digest"] for r in leaf_rows}), 1)
                self.assertTrue(all(r["kv_entries"] == 1 and r["kv_digest"] == digest(packed({key: state}))
                                    for r in leaf_rows))
            root_rows = [r for r in rows if r["shard"] == 5]
            self.assertTrue(all(r["ordered_cst_transactions"] == 2 for r in root_rows))
            self.assertTrue(all(r["executed_transactions"] == 0 for r in root_rows))

            root_node = next(n for n in cfg["nodes"] if n["shard"] == 5 and n["replica"] == 0)
            commits = [json.loads(line) for line in
                       (Path(root_node["directory"]) / "commits.jsonl").read_text().splitlines()]
            decisions = [entry["certificate"] for entry in commits if
                         "cst_decisions" in entry["certificate"]["proposal"]["body"]["value"]]
            self.assertEqual(len(decisions), 1)
            self.assertGreaterEqual(len(decisions[0]["commits"]), 3)
            leaf_node = next(n for n in cfg["nodes"] if n["shard"] == 1 and n["replica"] == 0)
            bad_ack = c.signed({"type": "CST_ACK", "run": cfg["run_id"], "shard": 1, "from": 0,
                                "view": 0, "target": 5, "batch_key": "5:1",
                                "decision_digest": "0" * 64, "result_digest": "0" * 64},
                               leaf_node["private_key"], run)
            before = c.read(Path(root_node["directory"]) / "status.json")["rejected_messages"]
            c.send_frame(root_node["host"], root_node["port"], bad_ack)
            wait_status(run, lambda rows: next(r for r in rows if r["shard"] == 5 and r["replica"] == 0)
                        ["rejected_messages"] > before, timeout=3)

            replay = copy.deepcopy(workload)
            replay["requests"][0]["id"] = "dependent:new-request-same-transactions"
            replay_job, replay_output = run / "replay.workload.json", run / "replay.result.json"
            c.write(replay_job, replay)
            replay_rc = subprocess.run([str(c.BIN), "client", str(run / "config.json"),
                                        str(replay_job), str(replay_output)], check=False).returncode
            replay_result = c.read(replay_output)
            self.assertEqual(replay_rc, 0)
            self.assertEqual(replay_result["executed_transactions"], 0)
            self.assertEqual(replay_result["duplicate_transactions"], 2)
            replay_rows = wait_status(run, lambda rows: all(
                sum(r["finalized_cst_batches"] == 2 for r in rows if r["shard"] == leaf) == 4
                for leaf in (1, 2)))
            self.assertTrue(all(r["executed_transactions"] == 2 and
                                r["kv_digest"] == digest(packed({key1: a}))
                                for r in replay_rows if r["shard"] == 1))
            self.assertTrue(all(r["executed_transactions"] == 2 and
                                r["kv_digest"] == digest(packed({key2: b}))
                                for r in replay_rows if r["shard"] == 2))

            conflict = copy.deepcopy(workload)
            conflict["requests"][0]["id"] = "dependent:conflicting-request"
            conflict["requests"][0]["txs"][0]["accesses"][0]["value"] = 999
            conflict["requests"][0]["txs"][0]["value"] = 999
            conflict_job, conflict_output = run / "conflict.workload.json", run / "conflict.result.json"
            c.write(conflict_job, conflict)
            conflict_rc = subprocess.run([str(c.BIN), "client", str(run / "config.json"),
                                          str(conflict_job), str(conflict_output)], check=False).returncode
            conflict_result = c.read(conflict_output)
            self.assertEqual(conflict_rc, 2)
            self.assertEqual(conflict_result["errors"], 2)
            self.assertEqual(conflict_result["executed_transactions"], 0)
        finally:
            c.stop_run(run)

    def test_missing_participant_cannot_commit_partial_writes(self):
        run = c.start(config("missing-participant"), TEST_ROOT / "missing-participant")
        try:
            manifest = c.read(run / "manifest.json")
            for node in manifest["nodes"]:
                if node["shard"] == 2:
                    os.kill(node["pid"], signal.SIGTERM)
            time.sleep(.3)
            rc, result = c.load(run, count=2, rate=20, participants=[1, 2], batch=2, timeout=3)
            self.assertEqual(rc, 2)
            self.assertEqual(result["executed_transactions"], 0)
            self.assertEqual(result["ordered_only_transactions"], 0)
            rows = wait_status(run, lambda rows: sum(
                r["leaf_ordered_cst_transactions"] == 2 and r["staged_cst_batches"] == 1
                for r in rows if r["shard"] == 1) == 4, timeout=5)
            self.assertTrue(all(r["executed_transactions"] == 0 and r["kv_entries"] == 0
                                for r in rows if r["shard"] == 1))
            self.assertTrue(all(r["decided_cst_batches"] == 0 for r in rows if r["shard"] == 5))
        finally:
            c.stop_run(run)


if __name__ == "__main__":
    print(f"第二阶段 B 测试日志保留在: {TEST_ROOT}", flush=True)
    unittest.main(verbosity=2)
